#version 450
// XR eyes: fill hairline cracks between track polygons. PS1 meshes leave
// sub-pixel gaps along shared edges; at headset resolution the sky shows
// through them as bright lines and specks. A pixel that is sky (depth still
// at the clear value) but has geometry on both sides, horizontally or
// vertically within two pixels, is a crack: replace it with those
// neighbours. Real sky, including thin slivers between objects wider than
// the crack width, is left alone.
layout(location = 0) in vec2 uv;
layout(set = 2, binding = 0) uniform sampler2D frame;
layout(set = 2, binding = 1) uniform sampler2D depth;
layout(location = 0) out vec4 fragColor;

// Colour grade for the headset image: x saturation, y contrast, z gamma
// (1, 1, 1 leaves the picture as rendered) and w sharpening (0 is off).
layout(set = 3, binding = 0, std140) uniform Grade {
    vec4 grade;
} tone;

ivec2 size;

ivec2 Clamp(ivec2 p) { return clamp(p, ivec2(0), size - 1); }

#ifdef COVERAGE_ALPHA
// MSAA eyes: depth is multisampled and cannot be read here; alpha holds the
// resolved geometry coverage instead (sky writes none).
bool Solid(ivec2 p) { return texelFetch(frame, Clamp(p), 0).a >= 0.5; }
#else
bool Solid(ivec2 p) { return texelFetch(depth, Clamp(p), 0).r < 1.0; }
#endif

vec3 Color(ivec2 p) { return texelFetch(frame, Clamp(p), 0).rgb; }

// Looks along one axis for geometry within two pixels on both sides.
bool Bridge(ivec2 p, ivec2 axis, out vec3 fill) {
    ivec2 before = Solid(p - axis) ? p - axis
                 : (Solid(p - 2 * axis) ? p - 2 * axis : ivec2(-1));
    ivec2 after = Solid(p + axis) ? p + axis
                : (Solid(p + 2 * axis) ? p + 2 * axis : ivec2(-1));
    if (before.x < 0 || after.x < 0 || before.y < 0 || after.y < 0) {
        fill = vec3(0.0);
        return false;
    }
    fill = 0.5 * (Color(before) + Color(after));
    return true;
}

void main() {
    size = textureSize(frame, 0);
    ivec2 p = Clamp(ivec2(uv * vec2(size)));
    vec3 color = Color(p);
    if (!Solid(p)) {
        vec3 horizontal, vertical;
        bool h = Bridge(p, ivec2(1, 0), horizontal);
        bool v = Bridge(p, ivec2(0, 1), vertical);
        if (h && v) color = 0.5 * (horizontal + vertical);
        else if (h) color = horizontal;
        else if (v) color = vertical;
#ifdef CRACK_DEBUG
        if (h || v) color = vec3(1.0, 0.0, 1.0);
#endif
    }
    if (tone.grade.w > 0.0) {
        // Sharpen against the four neighbours, limited to their range so
        // edges gain contrast without haloes.
        vec3 n = Color(p + ivec2(0, -1));
        vec3 s = Color(p + ivec2(0, 1));
        vec3 e = Color(p + ivec2(1, 0));
        vec3 w = Color(p + ivec2(-1, 0));
        vec3 low = min(color, min(min(n, s), min(e, w)));
        vec3 high = max(color, max(max(n, s), max(e, w)));
        color = clamp(color + (4.0 * color - n - s - e - w) * tone.grade.w,
                      low, high);
    }
    float luma = dot(color, vec3(0.299, 0.587, 0.114));
    color = mix(vec3(luma), color, tone.grade.x);
    color = (color - 0.5) * tone.grade.y + 0.5;
    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / tone.grade.z));
    fragColor = vec4(color, 1.0);
}
