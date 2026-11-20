#include "native_mesh_writer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void ImportWrite32(void *pointer, uint32_t value) {
    uint8_t *p = pointer;
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

int ImportWriteFinish(RageImportedWrite *write, uint32_t meshes) {
    if (write == NULL || write->offsets == NULL || meshes != write->meshLimit ||
        write->currentMesh > meshes || write->vertexCount != write->vertexLimit ||
        write->indexCount != write->indexLimit) return 0;
    while (write->currentMesh < meshes) {
        ++write->currentMesh;
        ImportWrite32(write->offsets + (size_t)write->currentMesh * 4, write->indexCount);
    }
    return 1;
}

int ImportTextureEqual(const RageImportedTextureKey *left,
                                  const RageImportedTextureKey *right) {
    return left->tpage == right->tpage && left->clut == right->clut &&
           left->terrainEnvironmentClut == right->terrainEnvironmentClut &&
           left->hasWindow == right->hasWindow &&
           (!left->hasWindow ||
            (left->windowWidthU == right->windowWidthU &&
             left->windowWidthV == right->windowWidthV &&
             left->windowOffsetU == right->windowOffsetU &&
             left->windowOffsetV == right->windowOffsetV));
}

static uint32_t ImportMaterialIndex(const RageImportedMeshEntry *entry,
                                        const RageImportedFace *face) {
    uint32_t material;
    if (!face->textured) return UINT32_MAX;
    for (material = 0; material < entry->materialCount; material++)
        if (ImportTextureEqual(&entry->materials[material],
                                   &face->texture)) return material;
    return UINT32_MAX;
}

int ImportWriteFace(uint32_t mesh, const RageImportedFace *face,
                               void *context) {
    RageImportedWrite *write = context;
    uint32_t material = ImportMaterialIndex(write->entry, face);
    uint32_t encodedMaterial = material;
    uint32_t corner, piece, pieceCount = 1;
    const uint32_t source = write->sourceFace;
    const RageImportedPiece *pieces = NULL;
    static const uint32_t order[] = {0, 2, 1, 1, 2, 3};
    if (write->drapes != NULL && source < write->drapeCount &&
        write->drapes[source].count != 0) {
        pieces = write->pieces + write->drapes[source].first;
        pieceCount = write->drapes[source].count;
    }
    /* The sizing pass is not permission to write beyond its allocation if
     * source topology changes. Check before touching any output byte. */
    if (mesh >= write->meshLimit || mesh < write->currentMesh ||
        write->vertexCount > write->vertexLimit ||
        (write->vertexLimit - write->vertexCount) / 4u < pieceCount ||
        write->indexCount > write->indexLimit ||
        (write->indexLimit - write->indexCount) / 6u < pieceCount ||
        (face->textured && material == UINT32_MAX)) return 0;
    while (write->currentMesh < mesh) {
        write->currentMesh++;
        ImportWrite32(write->offsets + (size_t)write->currentMesh * 4,
                      write->indexCount);
    }
    if (face->depthBias != 0 ||
        (write->entry->cached.assetSet == RAGE_RENDER_ASSET_TERRAIN &&
         ((face->flags & 2) != 0 || face->prim < 2 ||
          (face->prim >= 2 && (face->prim & 1) == 0)))) {
        uint32_t materialIndex =
            material == UINT32_MAX ? 0xFFFFu : material;
        encodedMaterial = materialIndex | RAGE_RUNTIME_MATERIAL_METADATA |
            ((uint32_t)(uint8_t)face->depthBias <<
             RAGE_RUNTIME_MATERIAL_DEPTH_BIAS_SHIFT);
        if (write->entry->cached.assetSet == RAGE_RENDER_ASSET_TERRAIN &&
            (face->flags & 2) != 0)
            encodedMaterial |= RAGE_RUNTIME_MATERIAL_TERRAIN_NEAR_ONLY;
        if (write->entry->cached.assetSet == RAGE_RENDER_ASSET_TERRAIN &&
            face->prim < 2)
            encodedMaterial |= RAGE_RUNTIME_MATERIAL_TERRAIN_ENV_CLUT;
        if (write->entry->cached.assetSet == RAGE_RENDER_ASSET_TERRAIN &&
            face->prim < 2)
            encodedMaterial |= RAGE_RUNTIME_MATERIAL_FOGGED_NORMAL_ENV;
        else if (write->entry->cached.assetSet == RAGE_RENDER_ASSET_TERRAIN &&
                 (face->prim & 1) == 0)
            encodedMaterial |= RAGE_RUNTIME_MATERIAL_FOGGED;
    }
    if (write->entry->cached.assetSet == RAGE_RENDER_ASSET_COURSE &&
        face->prim == 3 && material != UINT32_MAX)
        encodedMaterial |= RAGE_RUNTIME_MATERIAL_SCROLL_U;
    if (pieces != NULL) {
        /* Draped road paint: tell the draw builder explicitly, because the
         * pieces no longer have the long thin shape its heuristic expects. */
        if ((encodedMaterial & RAGE_RUNTIME_MATERIAL_METADATA) == 0)
            encodedMaterial = (material == UINT32_MAX ? 0xFFFFu : material) |
                RAGE_RUNTIME_MATERIAL_METADATA |
                ((uint32_t)(uint8_t)face->depthBias <<
                 RAGE_RUNTIME_MATERIAL_DEPTH_BIAS_SHIFT) |
                (encodedMaterial & RAGE_RUNTIME_MATERIAL_SCROLL_U);
        encodedMaterial |= RAGE_RUNTIME_MATERIAL_TERRAIN_ROAD_DECAL;
    }
    for (piece = 0; piece < pieceCount; piece++) {
        for (corner = 0; corner < 4; corner++) {
            const SVec *position = &face->vertices[face->vertex[corner]];
            const SVec *normal = face->hasNormals
                ? &face->normals[face->normal[corner]] : NULL;
            RageRuntimeVertex vertex = {
                .position = {(float)position->vx, (float)-position->vy, (float)-position->vz},
                .normal = {normal != NULL ? (float)normal->vx : 0.0f,
                           normal != NULL ? (float)-normal->vy : 1.0f,
                           normal != NULL ? (float)-normal->vz : 0.0f},
                .color = {face->color[0], face->color[1], face->color[2], 255},
                .uv = {face->textured ? ((float)face->uv[corner][0] + 0.5f) / 256.0f : 0.0f,
                       face->textured ? ((float)face->uv[corner][1] + 0.5f) / 256.0f : 0.0f},
                .material = encodedMaterial
            };
            if (pieces != NULL) {
                const RageImportedPiece *p = &pieces[piece];
                memcpy(vertex.position, p->position[corner], sizeof(vertex.position));
                vertex.uv[0] = (p->uv[corner][0] + 0.5f) / 256.0f;
                vertex.uv[1] = (p->uv[corner][1] + 0.5f) / 256.0f;
            }
            if (write->faceOffsets != NULL && source < write->faceOffsetCount) {
                const float *offset = write->faceOffsets[source];
                vertex.position[0] += offset[0];
                vertex.position[1] += offset[1];
                vertex.position[2] += offset[2];
            }
            if (!RuntimeVertexEncode(write->vertexCursor, RAGE_RUNTIME_VERTEX_BYTES, &vertex))
                return 0;
            write->vertexCursor += RAGE_RUNTIME_VERTEX_BYTES;
        }
        for (corner = 0; corner < 6; corner++)
            ImportWrite32(write->indexCursor + corner * 4,
                              write->vertexCount + order[corner]);
        write->indexCursor += 6 * 4;
        write->vertexCount += 4;
        write->indexCount += 6;
    }
    write->sourceFace++;
    return 1;
}

/* Sign tilt from vertical (|normal.y|), wall verticality, and how far a sign
 * may be moved. Course 1's pit banners lean 0.15 and sink up to ~9 units; the
 * banners opposite them clear their wall by 5.5-7, which is the target. */
#define SIGN_TILT_MIN 0.08f
#define SIGN_TILT_MAX 0.35f
#define SIGN_WALL_VERTICAL 0.02f
#define SIGN_WALL_PARALLEL 0.95f
#define SIGN_WALL_REACH 40.0f
#define SIGN_CLEARANCE 6.0f
/* The pit banners need 13.6-13.8. A chamfer on a tapered column (course
 * index 2) also passes every other test and would need 21: the cap keeps
 * architecture in place. */
#define SIGN_MAX_PUSH 16.0f
#define SIGN_SAME_POINT 0.5f

static void SignSub(const float a[3], const float b[3], float out[3]) {
    out[0] = a[0] - b[0];
    out[1] = a[1] - b[1];
    out[2] = a[2] - b[2];
}

static float SignDot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* Unit normal from the diagonals (corner order 0,1 / 2,3 as written). */
static int SignQuadNormal(const RageImportedQuad quad, float normal[3]) {
    float a[3], b[3], length;
    SignSub(quad[3], quad[0], a);
    SignSub(quad[2], quad[1], b);
    normal[0] = a[1] * b[2] - a[2] * b[1];
    normal[1] = a[2] * b[0] - a[0] * b[2];
    normal[2] = a[0] * b[1] - a[1] * b[0];
    length = sqrtf(SignDot(normal, normal));
    if (!(length > 1e-3f)) return 0;
    normal[0] /= length;
    normal[1] /= length;
    normal[2] /= length;
    return 1;
}

static int SignSamePoint(const float a[3], const float b[3], float unit) {
    float d[3], limit = SIGN_SAME_POINT * unit;
    SignSub(a, b, d);
    return SignDot(d, d) < limit * limit;
}

static int SignFreeStanding(const RageImportedQuad *quads, uint32_t first,
                            uint32_t end, uint32_t sign, float unit) {
    uint32_t face, i, j;
    for (i = 0; i < 4; i++)
        for (j = i + 1; j < 4; j++)
            if (SignSamePoint(quads[sign][i], quads[sign][j], unit)) return 0;
    for (face = first; face < end; face++) {
        if (face == sign) continue;
        for (i = 0; i < 4; i++)
            for (j = 0; j < 4; j++)
                if (SignSamePoint(quads[sign][i], quads[face][j], unit)) return 0;
    }
    return 1;
}

uint32_t ImportResolveSignPenetration(const RageImportedQuad *quads,
                                      const uint32_t *meshes, uint32_t count,
                                      float unit, float (*offsets)[3]) {
    uint32_t sign, face, corner, moved = 0, first = 0, end = 0;
    if (quads == NULL || meshes == NULL || offsets == NULL || !(unit > 0.0f))
        return 0;
    memset(offsets, 0, sizeof(*offsets) * count);
    for (sign = 0; sign < count; sign++) {
        const float (*q)[3] = quads[sign];
        float normal[3], out[3], tangent[3], up[3], length;
        float topY, tMin = 0.0f, tMax = 0.0f, yMin = q[0][1], yMax = q[0][1];
        float push = 0.0f, pushDir[3] = {0.0f, 0.0f, 0.0f};
        /* Faces of one mesh are contiguous and share its local origin; only
         * those can be compared. */
        if (sign >= end) {
            first = sign;
            end = sign + 1;
            while (end < count && meshes[end] == meshes[first]) end++;
        }
        if (!SignQuadNormal(q, normal)) continue;
        if (fabsf(normal[1]) < SIGN_TILT_MIN || fabsf(normal[1]) > SIGN_TILT_MAX)
            continue;
        out[0] = normal[0];
        out[1] = 0.0f;
        out[2] = normal[2];
        length = sqrtf(SignDot(out, out));
        out[0] /= length;
        out[2] /= length;
        /* A sign hangs from its top edge, which is on the open side. */
        for (corner = 1; corner < 4; corner++) {
            if (q[corner][1] < yMin) yMin = q[corner][1];
            if (q[corner][1] > yMax) yMax = q[corner][1];
        }
        topY = 0.5f * (yMin + yMax);
        up[0] = up[1] = up[2] = 0.0f;
        for (corner = 0; corner < 4; corner++) {
            float sideways[3];
            SignSub(q[corner], q[0], sideways);
            if (q[corner][1] >= topY) {
                up[0] += sideways[0];
                up[2] += sideways[2];
            } else {
                up[0] -= sideways[0];
                up[2] -= sideways[2];
            }
        }
        if (SignDot(up, out) < 0.0f) {
            out[0] = -out[0];
            out[2] = -out[2];
        }
        tangent[0] = -out[2];
        tangent[1] = 0.0f;
        tangent[2] = out[0];
        for (corner = 0; corner < 4; corner++) {
            float t = SignDot(tangent, q[corner]);
            if (corner == 0 || t < tMin) tMin = t;
            if (corner == 0 || t > tMax) tMax = t;
        }
        for (face = first; face < end; face++) {
            const float (*w)[3] = quads[face];
            float wallNormal[3], wt0 = 0.0f, wt1 = 0.0f, wy0 = w[0][1], wy1 = w[0][1];
            float dMin = 0.0f, dMax = 0.0f, facing;
            int far = 0;
            if (face == sign || !SignQuadNormal(w, wallNormal) ||
                fabsf(wallNormal[1]) > SIGN_WALL_VERTICAL) continue;
            wallNormal[1] = 0.0f;
            length = sqrtf(SignDot(wallNormal, wallNormal));
            wallNormal[0] /= length;
            wallNormal[2] /= length;
            facing = SignDot(wallNormal, out);
            if (fabsf(facing) < SIGN_WALL_PARALLEL) continue;
            if (facing < 0.0f) {
                wallNormal[0] = -wallNormal[0];
                wallNormal[2] = -wallNormal[2];
            }
            for (corner = 0; corner < 4; corner++) {
                float rel[3], d;
                SignSub(q[corner], w[0], rel);
                d = SignDot(wallNormal, rel);
                if (fabsf(d) > SIGN_WALL_REACH * unit) far = 1;
                if (corner == 0 || d < dMin) dMin = d;
                if (corner == 0 || d > dMax) dMax = d;
            }
            if (far || !(dMin < 0.0f && dMax > 0.0f)) continue;
            for (corner = 0; corner < 4; corner++) {
                float t = SignDot(tangent, w[corner]);
                if (corner == 0 || t < wt0) wt0 = t;
                if (corner == 0 || t > wt1) wt1 = t;
                if (w[corner][1] < wy0) wy0 = w[corner][1];
                if (w[corner][1] > wy1) wy1 = w[corner][1];
            }
            if (wt1 <= tMin + unit || wt0 >= tMax - unit ||
                wy1 <= yMin || wy0 >= yMax) continue;
            if (SIGN_CLEARANCE * unit - dMin > push) {
                push = SIGN_CLEARANCE * unit - dMin;
                memcpy(pushDir, wallNormal, sizeof(pushDir));
            }
        }
        if (push <= 0.0f || push > SIGN_MAX_PUSH * unit ||
            !SignFreeStanding(quads, first, end, sign, unit)) continue;
        offsets[sign][0] = pushDir[0] * push;
        offsets[sign][1] = 0.0f;
        offsets[sign][2] = pushDir[2] * push;
        moved++;
    }
    return moved;
}


/* Road-paint draping, all distances in world units (times `unit`). The paint
 * sits within ~6 of its road; anything further away is a different surface
 * (a bridge deck above, a road below). */
#define DRAPE_HEIGHT_REACH 12.0f
#define DRAPE_MIN_PIECE 0.5f
#define DRAPE_MAX_CUTS 64
#define DRAPE_MAX_RANGES 16
/* Barycentric slack, so a corner exactly on a shared road edge is found. */
#define DRAPE_INSIDE_EPSILON 5e-3f

static int DrapeIsTriangleFace(const RageImportedQuad quad) {
    float d[3];
    SignSub(quad[2], quad[3], d);
    return SignDot(d, d) < 1e-6f;
}

/* Same test as RenderTriangleIsRoadDecal, scaled to quad coordinates. */
static int DrapeTriangleIsMarking(const float *a, const float *b,
                                  const float *c, float unit) {
    float ab[3], bc[3], ca[3], n[3], e0, e1, e2, shortest, longest, length;
    SignSub(b, a, ab);
    SignSub(c, b, bc);
    SignSub(a, c, ca);
    e0 = sqrtf(SignDot(ab, ab));
    e1 = sqrtf(SignDot(bc, bc));
    e2 = sqrtf(SignDot(ca, ca));
    shortest = fminf(e0, fminf(e1, e2));
    longest = fmaxf(e0, fmaxf(e1, e2));
    if (shortest > 16.0f * unit || longest < 64.0f * unit ||
        longest < shortest * 8.0f) return 0;
    n[0] = ab[1] * ca[2] - ab[2] * ca[1];
    n[1] = ab[2] * ca[0] - ab[0] * ca[2];
    n[2] = ab[0] * ca[1] - ab[1] * ca[0];
    length = sqrtf(SignDot(n, n));
    return length > 0.0f && fabsf(n[1]) >= length * 0.85f;
}

static int DrapeTriangleIsRoad(const float *a, const float *b, const float *c) {
    float ab[3], ac[3], n[3], length;
    SignSub(b, a, ab);
    SignSub(c, a, ac);
    n[0] = ab[1] * ac[2] - ab[2] * ac[1];
    n[1] = ab[2] * ac[0] - ab[0] * ac[2];
    n[2] = ab[0] * ac[1] - ab[1] * ac[0];
    length = sqrtf(SignDot(n, n));
    return length > 1e-3f && fabsf(n[1]) >= length * 0.85f;
}

/* Triangles of a written quad, matching the index order {0,2,1},{1,2,3}. */
static const uint8_t kDrapeTriangle[2][3] = {{0, 2, 1}, {1, 2, 3}};

/* Faces of one mesh, and the offset that takes the marking's local space
 * into that mesh's local space. */
typedef struct DrapeRange {
    uint32_t first, end;
    float dx, dz;
} DrapeRange;

static int DrapeRoadHeight(const RageImportedQuad *quads,
                           const uint8_t *marking, const DrapeRange *ranges,
                           uint32_t rangeCount, float x, float z,
                           float referenceY, float reach, float *y) {
    uint32_t range, face;
    int found = 0;
    float best = reach;
    for (range = 0; range < rangeCount; range++) {
        const float px = x - ranges[range].dx, pz = z - ranges[range].dz;
        for (face = ranges[range].first; face < ranges[range].end; face++) {
            uint32_t tri, triangles;
            if (marking[face]) continue;
            triangles = DrapeIsTriangleFace(quads[face]) ? 1u : 2u;
            for (tri = 0; tri < triangles; tri++) {
                const float *a = quads[face][kDrapeTriangle[tri][0]];
                const float *b = quads[face][kDrapeTriangle[tri][1]];
                const float *c = quads[face][kDrapeTriangle[tri][2]];
                float det, l1, l2, l3, height;
                if (!DrapeTriangleIsRoad(a, b, c)) continue;
                det = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
                if (fabsf(det) < 1e-6f) continue;
                l1 = ((b[2] - c[2]) * (px - c[0]) + (c[0] - b[0]) * (pz - c[2])) / det;
                l2 = ((c[2] - a[2]) * (px - c[0]) + (a[0] - c[0]) * (pz - c[2])) / det;
                l3 = 1.0f - l1 - l2;
                if (l1 < -DRAPE_INSIDE_EPSILON || l2 < -DRAPE_INSIDE_EPSILON ||
                    l3 < -DRAPE_INSIDE_EPSILON) continue;
                height = l1 * a[1] + l2 * b[1] + l3 * c[1];
                if (fabsf(height - referenceY) <= best) {
                    best = fabsf(height - referenceY);
                    *y = height;
                    found = 1;
                }
            }
        }
    }
    return found;
}

/* Parameters where segment p0->p1 (in XZ) crosses road-triangle edges. */
static int DrapeCollectCuts(const RageImportedQuad *quads,
                            const uint8_t *marking, const DrapeRange *ranges,
                            uint32_t rangeCount, const float *p0,
                            const float *p1, float yMin, float yMax,
                            float *cuts, uint32_t *cutCount) {
    uint32_t range, face;
    const float rx = p1[0] - p0[0], rz = p1[2] - p0[2];
    for (range = 0; range < rangeCount; range++) {
        const float ox = p0[0] - ranges[range].dx, oz = p0[2] - ranges[range].dz;
        for (face = ranges[range].first; face < ranges[range].end; face++) {
            uint32_t tri, edge, triangles;
            if (marking[face]) continue;
            triangles = DrapeIsTriangleFace(quads[face]) ? 1u : 2u;
            for (tri = 0; tri < triangles; tri++) {
                const float *v[3];
                v[0] = quads[face][kDrapeTriangle[tri][0]];
                v[1] = quads[face][kDrapeTriangle[tri][1]];
                v[2] = quads[face][kDrapeTriangle[tri][2]];
                if (!DrapeTriangleIsRoad(v[0], v[1], v[2])) continue;
                if (fmaxf(v[0][1], fmaxf(v[1][1], v[2][1])) < yMin ||
                    fminf(v[0][1], fminf(v[1][1], v[2][1])) > yMax) continue;
                for (edge = 0; edge < 3; edge++) {
                    const float *e0 = v[edge], *e1 = v[(edge + 1) % 3];
                    const float sx = e1[0] - e0[0], sz = e1[2] - e0[2];
                    const float den = rx * sz - rz * sx;
                    float t, u;
                    if (fabsf(den) < 1e-6f) continue;
                    t = ((e0[0] - ox) * sz - (e0[2] - oz) * sx) / den;
                    u = ((e0[0] - ox) * rz - (e0[2] - oz) * rx) / den;
                    if (!(t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f)) continue;
                    if (*cutCount == DRAPE_MAX_CUTS) return 0;
                    cuts[(*cutCount)++] = t;
                }
            }
        }
    }
    return 1;
}

static void DrapeLerp(const float *a, const float *b, float t, float *out,
                      unsigned components) {
    unsigned i;
    for (i = 0; i < components; i++) out[i] = a[i] + (b[i] - a[i]) * t;
}

uint32_t ImportDrapeRoadMarkings(const RageImportedQuad *quads,
                                 const float (*uvs)[4][2],
                                 const uint32_t *meshes,
                                 const float (*origins)[2], uint32_t count,
                                 float unit, float cellPitch,
                                 RageImportedDrape *drapes,
                                 RageImportedPiece **pieces) {
    uint8_t *marking;
    DrapeRange *meshRanges;
    RageImportedPiece *out = NULL;
    uint32_t face, used = 0, capacity = 0, rangeCount = 0, own = 0;
    if (quads == NULL || uvs == NULL || meshes == NULL || drapes == NULL ||
        pieces == NULL || !(unit > 0.0f)) return 0;
    *pieces = NULL;
    memset(drapes, 0, sizeof(*drapes) * count);
    marking = calloc(count ? count : 1, 1);
    meshRanges = calloc(count ? count : 1, sizeof(*meshRanges));
    if (marking == NULL || meshRanges == NULL) {
        free(marking);
        free(meshRanges);
        return 0;
    }
    for (face = 0; face < count; face++) {
        const float (*q)[3] = quads[face];
        marking[face] = !DrapeIsTriangleFace(quads[face]) &&
            DrapeTriangleIsMarking(q[0], q[2], q[1], unit) &&
            DrapeTriangleIsMarking(q[1], q[2], q[3], unit);
        if (face == 0 || meshes[face] != meshes[face - 1]) {
            meshRanges[rangeCount].first = face;
            /* dx/dz hold the mesh origin here (NAN: not placed). */
            meshRanges[rangeCount].dx = origins != NULL ? origins[face][0] : NAN;
            meshRanges[rangeCount].dz = origins != NULL ? origins[face][1] : NAN;
            rangeCount++;
        }
        meshRanges[rangeCount - 1].end = face + 1;
    }
    for (face = 0; face < count; face++) {
        const float (*q)[3] = quads[face];
        float cuts[DRAPE_MAX_CUTS], keep[DRAPE_MAX_CUTS + 2];
        DrapeRange ranges[DRAPE_MAX_RANGES];
        uint32_t cutCount = 0, keepCount = 0, nearCount = 0, i, j, start = used;
        uint32_t s0, e0, s1, e1;
        float along02, along01, length, yMin, yMax, reach = DRAPE_HEIGHT_REACH * unit;
        int ok = 1;
        while (face >= meshRanges[own].end) own++;
        if (!marking[face]) continue;
        /* Its own mesh first, then neighbours placed within one cell. */
        ranges[nearCount].first = meshRanges[own].first;
        ranges[nearCount].end = meshRanges[own].end;
        ranges[nearCount].dx = ranges[nearCount].dz = 0.0f;
        nearCount++;
        if (cellPitch > 0.0f && !isnan(meshRanges[own].dx)) {
            for (i = 0; i < rangeCount && nearCount < DRAPE_MAX_RANGES; i++) {
                float dx = meshRanges[i].dx - meshRanges[own].dx;
                float dz = meshRanges[i].dz - meshRanges[own].dz;
                if (i == own || isnan(dx) || isnan(dz) ||
                    fabsf(dx) > cellPitch * 1.01f ||
                    fabsf(dz) > cellPitch * 1.01f) continue;
                ranges[nearCount].first = meshRanges[i].first;
                ranges[nearCount].end = meshRanges[i].end;
                ranges[nearCount].dx = dx;
                ranges[nearCount].dz = dz;
                nearCount++;
            }
        }
        /* Long sides: L = s0->e0, R = s1->e1. */
        {
            float d[3];
            SignSub(q[2], q[0], d); along02 = sqrtf(SignDot(d, d));
            SignSub(q[3], q[1], d); along02 += sqrtf(SignDot(d, d));
            SignSub(q[1], q[0], d); along01 = sqrtf(SignDot(d, d));
            SignSub(q[3], q[2], d); along01 += sqrtf(SignDot(d, d));
        }
        if (along02 >= along01) { s0 = 0; e0 = 2; s1 = 1; e1 = 3; length = 0.5f * along02; }
        else { s0 = 0; e0 = 1; s1 = 2; e1 = 3; length = 0.5f * along01; }
        yMin = fminf(fminf(q[0][1], q[1][1]), fminf(q[2][1], q[3][1])) - reach;
        yMax = fmaxf(fmaxf(q[0][1], q[1][1]), fmaxf(q[2][1], q[3][1])) + reach;
        if (!DrapeCollectCuts(quads, marking, ranges, nearCount, q[s0], q[e0],
                              yMin, yMax, cuts, &cutCount) ||
            !DrapeCollectCuts(quads, marking, ranges, nearCount, q[s1], q[e1],
                              yMin, yMax, cuts, &cutCount)) continue;
        /* Sort, then keep cuts at least DRAPE_MIN_PIECE apart and from the
         * ends: [0, kept..., 1]. */
        for (i = 1; i < cutCount; i++) {
            float value = cuts[i];
            for (j = i; j > 0 && cuts[j - 1] > value; j--) cuts[j] = cuts[j - 1];
            cuts[j] = value;
        }
        keep[keepCount++] = 0.0f;
        for (i = 0; i < cutCount; i++)
            if ((cuts[i] - keep[keepCount - 1]) * length >= DRAPE_MIN_PIECE * unit &&
                (1.0f - cuts[i]) * length >= DRAPE_MIN_PIECE * unit)
                keep[keepCount++] = cuts[i];
        keep[keepCount++] = 1.0f;
        for (i = 0; ok && i + 1 < keepCount; i++) {
            RageImportedPiece piece;
            const float ends[2] = {keep[i], keep[i + 1]};
            const uint32_t side[4][2] = {{s0, e0}, {s0, e0}, {s1, e1}, {s1, e1}};
            const uint32_t slot[4] = {s0, e0, s1, e1};
            const uint32_t which[4] = {0, 1, 0, 1};
            uint32_t k;
            for (k = 0; k < 4; k++) {
                float *position = piece.position[slot[k]];
                DrapeLerp(q[side[k][0]], q[side[k][1]], ends[which[k]], position, 3);
                DrapeLerp(uvs[face][side[k][0]], uvs[face][side[k][1]],
                          ends[which[k]], piece.uv[slot[k]], 2);
                if (!DrapeRoadHeight(quads, marking, ranges, nearCount,
                                     position[0], position[2], position[1],
                                     reach, &position[1])) {
                    ok = 0;
                    break;
                }
            }
            if (!ok) break;
            if (used == capacity) {
                uint32_t grown = capacity ? capacity * 2u : 256u;
                RageImportedPiece *bigger = realloc(out, sizeof(*out) * grown);
                if (bigger == NULL) {
                    free(out);
                    free(marking);
                    free(meshRanges);
                    memset(drapes, 0, sizeof(*drapes) * count);
                    return 0;
                }
                out = bigger;
                capacity = grown;
            }
            out[used++] = piece;
        }
        if (!ok) {
            used = start;
            continue;
        }
        drapes[face].first = start;
        drapes[face].count = used - start;
    }
    free(marking);
    free(meshRanges);
    if (used == 0) {
        free(out);
        return 0;
    }
    *pieces = out;
    return used;
}
