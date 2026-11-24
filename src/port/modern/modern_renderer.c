#include "modern_renderer.h"
#include "modern_assets.h"
#include "modern_native_gpu.h"
#include "modern_sky_geometry.h"
#include "modern_sky_reprojection.h"
#include "modern_overlay_state.h"
#include "modern_overlay_batches.h"
#include "modern_presentation_clock.h"
#include "modern_fmv_zoom.h"
#include "../classic_motion.h"
#include "rage/render_world_game.h"
#include "rage/track_asset_identity.h"

#include <psyz/overlay_sdl3_gpu.h>
#include <psyz/present_sdl3_gpu.h>
#include <psyz/video.h>
#include <psyz/overlay.h>
#include <psyz/xr.h>
#include "game/camera_types.h"
#include "render/render_native_vertex.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modern_texture_dump.h"
#include "modern_vram_snapshot.h"
#include "../platform_paths.h"
#include "game/player_car_internal.h"
#include "game/race.h"
#include "game/state.h"
#include "game/render_internal.h"
#include "game/track_internal.h"
#include "rage/render_world_scene.h"
#include "modern_renderer_diagnostics.h"
#include "../runtime_config.h"
#include "../xr_profile.h"
#include "../graphics_settings.h"
#include "../native_asset_importer.h"
#include "shaders/modern_vert_spv.h"
#include "shaders/modern_frag_spv.h"
#include "shaders/post_vert_spv.h"
#include "shaders/post_frag_spv.h"
#include "shaders/modern_frag_msl.h"
#include "shaders/modern_vert_msl.h"
#include "shaders/post_frag_msl.h"
#include "shaders/composite_frag_spv.h"
#include "shaders/crack_fill_frag_spv.h"
#include "shaders/crack_fill_debug_frag_spv.h"
#include "shaders/crack_fill_alpha_frag_spv.h"
#include "shaders/crack_fill_alpha_debug_frag_spv.h"
#include "shaders/composite_frag_msl.h"
#include "shaders/post_vert_msl.h"

/* The modern presentation path combines native RenderWorld geometry with
 * captured 2D sky, HUD and mirror-frame layers. Sky geometry is reprojected
 * for the interpolated camera. Captured PS1 3D faces are not rendered here.
 * Enhanced classic uses the same affine GPU pipeline to replay the full GP0
 * stream in its original painter order, with optional screen interpolation.
 * Menus, FMV and 480-line screens present the compat image directly. */

static int s_enabled;
static int s_classicEnhancements = 1;
static float s_classicFraction;
static int s_classicPacketIndex = -1;
static const RenderWorld *s_classicSkyWorld;
static int s_presentOffscreen;
static int s_initialized;
static int s_shutdownRegistered;
static int s_shutdownNeeded;
static ModernVramSnapshotCache s_sampledVram;
static SDL_GPUTexture *s_ownedSampledVram;
static PsyzOverlayDestroyCB s_prev_overlay_destroy;
static SDL_Scancode s_toggleScancode = SDL_SCANCODE_F10;
static int s_toggleWasDown;
static int s_markerCaptureEnabled;
static int s_skyOnlyDiagnostic;
static Uint64 s_profilePrepareNs;
static int s_profileTiming;

extern int32_t g_FrameCounter;
/* Opt-in wall-clock phases include work between native-render callbacks.
 * The timestamps allow exact correlation with modern-frame trace records. */
void PortProfileFramePhase(const char *phase) {
    static int enabled = -1;
    static const char *previous;
    static Uint64 started;
    if (enabled < 0)
        enabled = RuntimeConfigEnabled("diagnostics.performance_trace");
    if (!enabled) return;
    Uint64 now = SDL_GetTicksNS();
    if (previous != NULL)
        fprintf(stderr, "frame-phase frame=%u phase=%s start_ns=%llu end_ns=%llu ms=%.3f\n",
                (unsigned)g_FrameCounter, previous,
                (unsigned long long)started, (unsigned long long)now,
                (now - started) / 1000000.0);
    previous = phase;
    started = now;
}

static SDL_Window *s_window;
static SDL_GPUDevice *s_device;
static PsyzOverlayInitCB_SDL3GPU s_prev_overlay_init;
static PsyzPresentSourceCB_SDL3GPU s_prev_present_source;
static RagePortConfig s_config;

static const RenderWorld *ModernSkyOnlyWorld(
    const RenderWorld *world) {
    static RenderWorld skyOnly;
    if (!s_skyOnlyDiagnostic || world == NULL) return world;
    skyOnly = *world;
    skyOnly.instanceCount = 0;
    skyOnly.mirrorActive = 0;
    return &skyOnly;
}

/* ---- GPU resources ---- */

static int s_targetW, s_targetH;
static float s_logicalW = 320.0f; /* 16:9 widens this; 240 rows stay */
static float s_overscanX;         /* (logicalW - 320) / 2 */
static SDL_GPUTexture *s_target;
static SDL_GPUTexture *s_depth;
/* XR stereo: one colour/depth pair per eye at the headset's eye size. */
static SDL_GPUTexture *s_eyeTarget[2];
static SDL_GPUTexture *s_eyeDepth[2];
static SDL_GPUTexture *s_eyeFinal[2]; /* after the crack fill */
/* 4x MSAA colour/depth; colour resolves into s_eyeTarget. */
static SDL_GPUTexture *s_eyeMsaa[2];
static SDL_GPUTexture *s_eyeMsaaDepth[2];
/* Fixed foveation: a Vulkan fragment density map per eye, attached to the
 * scene target so the edges of each eye shade at a coarser rate. */
static SDL_GPUTexture *s_eyeFdm[2];
static int s_xrCrackFillUsed;
static int s_eyeMsaaOn;
static SDL_GPUGraphicsPipeline *s_pipeCrackFill;
/* The s_eyeMsaaOn value s_pipeCrackFill's shader variant was built for. */
static int s_pipeCrackFillMsaa;
/* XR HUD panel: the 2D HUD layer alone on a transparent background
 * (premultiplied alpha, as the overlay pipelines blend), s_target-sized. */
static SDL_GPUTexture *s_hudTarget;
/* When set, overlay passes draw here instead of s_target. */
static SDL_GPUTexture *s_overlayTarget;
static unsigned s_eyeW, s_eyeH;
static SDL_GPUTexture *s_mirrorTarget;
static SDL_GPUTexture *s_mirrorDepth;
static int s_mirrorTargetW, s_mirrorTargetH;
static SDL_GPUSampler *s_sampler;
static SDL_GPUGraphicsPipeline *s_pipe2d;
static SDL_GPUGraphicsPipeline *s_pipe2dSub;
static SDL_GPUBuffer *s_vertexBuffer;
static SDL_GPUTransferBuffer *s_vertexTransfer;
static SDL_GPUTexture *s_postTarget;
static SDL_GPUGraphicsPipeline *s_pipePost;
static SDL_GPUSampler *s_samplerLinear;

/* Ring of recent presented frames, copied on the GPU every present so a
 * transient artifact can be dumped AFTER being seen (the synchronous burst
 * capture stalls presentation and suppresses timing-dependent flashes). */
#define MODERN_RING 16
static SDL_GPUTexture *s_ring[MODERN_RING];
static uint32_t s_ringFrame[MODERN_RING];
static float s_ringT[MODERN_RING];
static int s_ringNext;
static RageTrackTextureGeneration *s_ringGenerations[MODERN_RING];
static RenderWorldSnapshot s_ringWorlds[MODERN_RING];
static RageSceneSnapshot *s_ringScene; /* MODERN_RING copies */
static int s_ringEnabled;
static int s_resourcesReady;
static int s_nativeGpuReady;
static unsigned int s_resourceGeneration;
static uint32_t s_lastRenderedFrame = 0xFFFFFFFFu;
static int s_haveRenderedFrame;
static int s_waitClassicFrame;

enum {
    MODERN_PIPE_2D,
    MODERN_PIPE_2D_SUB,
};

#define MODERN_MAX_VERTICES 400000
#define MODERN_MAX_SPANS 16384
static ModernOverlayBatches s_overlay;

/* The far bucket boundary: captured 2D packets at or beyond this ordering
 * table index draw behind the 3D scene (sky layers); everything nearer
 * draws over it. */
#define MODERN_BACKGROUND_BUCKET 576

#define MODERN_FAR 262144.0f

/* ---- MSL shaders ---- */

#include "modern_shader_sources.h"
static SDL_GPUTexture *s_finalTarget;
static SDL_GPUGraphicsPipeline *s_pipeComposite;

static void ModernDisableFrameHistory(void) {
    int slot;

    for (slot = 0; slot < MODERN_RING; ++slot) {
        TrackTextureGenerationRelease(s_ringGenerations[slot]);
        s_ringGenerations[slot] = NULL;
        RenderWorldSnapshotRelease(&s_ringWorlds[slot]);
    }

    if (s_device != NULL) {
        for (slot = 0; slot < MODERN_RING; slot++) {
            if (s_ring[slot] != NULL) {
                SDL_ReleaseGPUTexture(s_device, s_ring[slot]);
                s_ring[slot] = NULL;
            }
        }
    }
    free(s_ringScene);
    s_ringScene = NULL;
    s_ringEnabled = 0;
    s_ringNext = 0;
}

/* ---- resource creation ---- */

/* Uniform buffers the next fragment shader created here declares (the XR
 * crack-fill pass takes one for its colour grade; the rest take none). */
static int s_fragmentUniformBuffers;

static SDL_GPUShader *ModernCreateShader(
    const unsigned char *spirv, size_t spirvSize,
    const char *msl, size_t mslSize, SDL_GPUShaderStage stage,
    const char *mslEntry, int samplers) {
    SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(s_device);
    SDL_GPUShaderCreateInfo info = {0};
    info.stage = stage;
    info.num_samplers = (Uint32)samplers;
    if (stage == SDL_GPU_SHADERSTAGE_FRAGMENT)
        info.num_uniform_buffers = (Uint32)s_fragmentUniformBuffers;
    if ((formats & SDL_GPU_SHADERFORMAT_SPIRV) && spirv != NULL) {
        info.code = spirv;
        info.code_size = spirvSize;
        info.entrypoint = "main";
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    } else if ((formats & SDL_GPU_SHADERFORMAT_MSL) && msl != NULL) {
        info.code = (const Uint8 *)msl;
        info.code_size = mslSize;
        info.entrypoint = mslEntry;
        info.format = SDL_GPU_SHADERFORMAT_MSL;
    } else {
        return NULL;
    }
    return SDL_CreateGPUShader(s_device, &info);
}

static SDL_GPUGraphicsPipeline *ModernCreateFullscreenPipeline(
    const unsigned char *vsSpirv, size_t vsSpirvSize,
    const char *vsMsl, size_t vsMslSize, const char *vsMslEntry,
    const unsigned char *fsSpirv, size_t fsSpirvSize,
    const char *fsMsl, size_t fsMslSize, const char *fsMslEntry, int samplers) {
    SDL_GPUShader *vs = ModernCreateShader(
        vsSpirv, vsSpirvSize, vsMsl, vsMslSize, SDL_GPU_SHADERSTAGE_VERTEX,
        vsMslEntry, 0);
    SDL_GPUShader *fs = ModernCreateShader(
        fsSpirv, fsSpirvSize, fsMsl, fsMslSize, SDL_GPU_SHADERSTAGE_FRAGMENT,
        fsMslEntry, samplers);
    SDL_GPUGraphicsPipeline *pipeline = NULL;
    if (vs && fs) {
        const SDL_GPUColorTargetDescription target = {
            .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        };
        SDL_GPUGraphicsPipelineCreateInfo info = {0};
        info.vertex_shader = vs;
        info.fragment_shader = fs;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.target_info.color_target_descriptions = &target;
        info.target_info.num_color_targets = 1;
        pipeline = SDL_CreateGPUGraphicsPipeline(s_device, &info);
    }
    if (vs) SDL_ReleaseGPUShader(s_device, vs);
    if (fs) SDL_ReleaseGPUShader(s_device, fs);
    return pipeline;
}

static SDL_GPUGraphicsPipeline *ModernCreatePostPipeline(void) {
    return ModernCreateFullscreenPipeline(
        post_vert_spv, post_vert_spv_len,
        (const char *)post_vert_msl, post_vert_msl_len, "vs_post",
        post_frag_spv, post_frag_spv_len,
        (const char *)post_frag_msl, post_frag_msl_len, "fs_post", 1);
}

static SDL_GPUGraphicsPipeline *ModernCreateCompositePipeline(void) {
    return ModernCreateFullscreenPipeline(
        post_vert_spv, post_vert_spv_len,
        (const char *)post_vert_msl, post_vert_msl_len, "vs_post",
        composite_frag_spv, composite_frag_spv_len,
        (const char *)composite_frag_msl, composite_frag_msl_len, "fs_composite", 1);
}
static SDL_GPUGraphicsPipeline *ModernCreateOverlayPipeline(
    SDL_GPUShader *vs, SDL_GPUShader *fs, int subtract) {
    const SDL_GPUVertexBufferDescription buffer = {
        .slot = 0,
        .pitch = sizeof(ModernVertex),
        .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
    };
    const SDL_GPUVertexAttribute attributes[] = {
        {.location = 0, .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
         .offset = offsetof(ModernVertex, x)},
        {.location = 1, .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
         .offset = offsetof(ModernVertex, u)},
        {.location = 2, .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4,
         .offset = offsetof(ModernVertex, color)},
        {.location = 3, .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT,
         .offset = offsetof(ModernVertex, attr)},
        {.location = 4, .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT,
         .offset = offsetof(ModernVertex, twin)},
        {.location = 5, .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT,
         .offset = offsetof(ModernVertex, clut)},
    };
    const SDL_GPUBlendOp op = subtract ? SDL_GPU_BLENDOP_REVERSE_SUBTRACT
                                       : SDL_GPU_BLENDOP_ADD;
    const SDL_GPUColorTargetDescription target = {
        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .blend_state = {
            .src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
            .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .color_blend_op = op,
            .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
            .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .alpha_blend_op = op,
            .enable_blend = true,
        },
    };
    SDL_GPUGraphicsPipelineCreateInfo info = {0};
    info.vertex_shader = vs;
    info.fragment_shader = fs;
    info.vertex_input_state.vertex_buffer_descriptions = &buffer;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes =
        sizeof(attributes) / sizeof(attributes[0]);
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = false;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.target_info.has_depth_stencil_target = true;
    return SDL_CreateGPUGraphicsPipeline(s_device, &info);
}

static void ModernDestroyResources(void) {
    int hadResources = s_resourcesReady;
    if (s_device != NULL) {
#define RAGE_RELEASE(kind, value) do {                                        \
        if ((value) != NULL) SDL_ReleaseGPU##kind(s_device, (value));          \
        (value) = NULL;                                                        \
    } while (0)
        ModernFmvZoomRelease(s_device);
        RAGE_RELEASE(Texture, s_target);
        RAGE_RELEASE(Texture, s_depth);
        RAGE_RELEASE(Texture, s_eyeTarget[0]);
        RAGE_RELEASE(Texture, s_eyeTarget[1]);
        RAGE_RELEASE(Texture, s_eyeDepth[0]);
        RAGE_RELEASE(Texture, s_eyeDepth[1]);
        RAGE_RELEASE(Texture, s_eyeFinal[0]);
        RAGE_RELEASE(Texture, s_eyeFinal[1]);
        RAGE_RELEASE(Texture, s_eyeMsaa[0]);
        RAGE_RELEASE(Texture, s_eyeMsaa[1]);
        RAGE_RELEASE(Texture, s_eyeMsaaDepth[0]);
        RAGE_RELEASE(Texture, s_eyeMsaaDepth[1]);
        RAGE_RELEASE(Texture, s_eyeFdm[0]);
        RAGE_RELEASE(Texture, s_eyeFdm[1]);
        RAGE_RELEASE(GraphicsPipeline, s_pipeCrackFill);
        RAGE_RELEASE(Texture, s_hudTarget);
        s_eyeW = s_eyeH = 0;
        RAGE_RELEASE(Texture, s_mirrorTarget);
        RAGE_RELEASE(Texture, s_mirrorDepth);
        RAGE_RELEASE(Sampler, s_sampler);
        RAGE_RELEASE(GraphicsPipeline, s_pipe2d);
        RAGE_RELEASE(GraphicsPipeline, s_pipe2dSub);
        RAGE_RELEASE(Buffer, s_vertexBuffer);
        RAGE_RELEASE(TransferBuffer, s_vertexTransfer);
        RAGE_RELEASE(Texture, s_postTarget);
        RAGE_RELEASE(GraphicsPipeline, s_pipePost);
        RAGE_RELEASE(Sampler, s_samplerLinear);
        RAGE_RELEASE(Texture, s_finalTarget);
        RAGE_RELEASE(Texture, s_ownedSampledVram);
        RAGE_RELEASE(GraphicsPipeline, s_pipeComposite);
#undef RAGE_RELEASE
    }
    ModernDisableFrameHistory();
    ModernOverlayBatchesRelease(&s_overlay);
    ModernNativeGpuShutdown();
    s_nativeGpuReady = 0;
    s_resourcesReady = 0;
    s_haveRenderedFrame = 0;
    s_waitClassicFrame = 0;
    s_lastRenderedFrame = 0xFFFFFFFFu;
    ModernVramSnapshotReset(&s_sampledVram);
    ClassicMotionReset();
    if (hadResources && RuntimeConfigEnabled("diagnostics.renderer_lifecycle")) {
        fprintf(stderr, "rage-port: modern resources destroyed generation=%u\n",
                s_resourceGeneration);
    }
}

static int ModernEnsureResources(void) {
    SDL_GPUShader *vs;
    SDL_GPUShader *fs;
    float scale;
    if (s_resourcesReady) return 1;
    if (s_device == NULL) return 0;

    scale = s_config.modernInternalScale;
    if (scale < 0.5f) scale = 0.5f;
    /* Widen the field of view while preserving vertical framing. */
    s_logicalW = PortAspectLogicalWidth(s_config.modernAspect);
    s_overscanX = (s_logicalW - 320.0f) * 0.5f;
    s_targetW = (int)(s_logicalW * scale + 0.5f) & ~1;
    s_targetH = (int)(240.0f * scale + 0.5f) & ~1;
    s_mirrorTargetW = (int)(148.0f * scale + 0.5f) & ~1;
    s_mirrorTargetH = (int)(36.0f * scale + 0.5f) & ~1;

    s_ringEnabled = s_enabled && s_markerCaptureEnabled && RuntimeConfigEnabled("diagnostics.marker_history");
    {
        SDL_GPUTextureCreateInfo info = {0};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                     SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = (Uint32)s_targetW;
        info.height = (Uint32)s_targetH;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        s_target = SDL_CreateGPUTexture(s_device, &info);
        info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        s_depth = SDL_CreateGPUTexture(s_device, &info);
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                     SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = (Uint32)s_mirrorTargetW;
        info.height = (Uint32)s_mirrorTargetH;
        s_mirrorTarget = SDL_CreateGPUTexture(s_device, &info);
        info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        s_mirrorDepth = SDL_CreateGPUTexture(s_device, &info);
    }
    {
        SDL_GPUSamplerCreateInfo info = {0};
        info.min_filter = SDL_GPU_FILTER_NEAREST;
        info.mag_filter = SDL_GPU_FILTER_NEAREST;
        s_sampler = SDL_CreateGPUSampler(s_device, &info);
    }
    vs = ModernCreateShader(
        modern_vert_spv, modern_vert_spv_len,
        (const char *)modern_vert_msl, modern_vert_msl_len,
        SDL_GPU_SHADERSTAGE_VERTEX, "vs_main", 0);
    fs = ModernCreateShader(
        modern_frag_spv, modern_frag_spv_len,
        (const char *)modern_frag_msl, modern_frag_msl_len,
        SDL_GPU_SHADERSTAGE_FRAGMENT, "fs_main", 1);
    if (vs && fs) {
        s_pipe2d = ModernCreateOverlayPipeline(vs, fs, 0);
        s_pipe2dSub = ModernCreateOverlayPipeline(vs, fs, 1);
    }
    if (vs) SDL_ReleaseGPUShader(s_device, vs);
    if (fs) SDL_ReleaseGPUShader(s_device, fs);
    {
        SDL_GPUBufferCreateInfo info = {0};
        info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        info.size = MODERN_MAX_VERTICES * sizeof(ModernVertex);
        s_vertexBuffer = SDL_CreateGPUBuffer(s_device, &info);
    }
    {
        SDL_GPUTransferBufferCreateInfo info = {0};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = MODERN_MAX_VERTICES * sizeof(ModernVertex);
        s_vertexTransfer = SDL_CreateGPUTransferBuffer(s_device, &info);
    }
    if (!ModernOverlayBatchesAllocate(&s_overlay, MODERN_MAX_VERTICES,
                                      MODERN_MAX_SPANS)) {
        ModernDestroyResources();
        return 0;
    }
    if (s_enabled &&
        !ModernNativeGpuInit(s_device, s_config.modernTextureFilterLinear)) {
        ModernDestroyResources();
        return 0;
    }
    s_nativeGpuReady = s_enabled;

    if (s_ringEnabled) {
        SDL_GPUTextureCreateInfo info = {0};
        int slot;
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                     SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = (Uint32)s_targetW;
        info.height = (Uint32)s_targetH;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        for (slot = 0; slot < MODERN_RING; slot++) {
            s_ring[slot] = SDL_CreateGPUTexture(s_device, &info);
            if (s_ring[slot] == NULL) break;
        }
        if (slot == MODERN_RING) {
            s_ringScene = malloc(MODERN_RING * sizeof(RageSceneSnapshot));
        }
        if (slot != MODERN_RING || s_ringScene == NULL) {
            fprintf(stderr,
                    "rage-port: frame-history setup failed, disabling: %s\n",
                    SDL_GetError());
            ModernDisableFrameHistory();
        }
    }

    if (s_config.modernPost != RAGE_MODERN_POST_NONE || s_config.modernGrading) {
        SDL_GPUSamplerCreateInfo samplerInfo = {0};
        samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
        samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
        s_samplerLinear = SDL_CreateGPUSampler(s_device, &samplerInfo);
    }
    if (s_config.modernPost != RAGE_MODERN_POST_NONE) {
        SDL_GPUTextureCreateInfo info = {0};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                     SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = (Uint32)s_targetW;
        info.height = (Uint32)s_targetH;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        s_postTarget = SDL_CreateGPUTexture(s_device, &info);
        s_pipePost = ModernCreatePostPipeline();
        if (!s_postTarget || !s_pipePost || !s_samplerLinear) {
            fprintf(stderr,
                    "rage-port: post-process setup failed, disabling: %s\n",
                    SDL_GetError());
            s_config.modernPost = RAGE_MODERN_POST_NONE;
        }
    }
    if (s_config.modernGrading) {
        SDL_GPUTextureCreateInfo info = {0};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                     SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = (Uint32)s_targetW;
        info.height = (Uint32)s_targetH;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        s_finalTarget = SDL_CreateGPUTexture(s_device, &info);
        s_pipeComposite = ModernCreateCompositePipeline();
        if (!s_finalTarget || !s_pipeComposite || !s_samplerLinear) {
            fprintf(stderr,
                    "rage-port: grading setup failed, disabling: %s\n",
                    SDL_GetError());
            s_config.modernGrading = 0;
        }
    }

    if (!s_target || !s_depth || !s_mirrorTarget || !s_mirrorDepth ||
        !s_sampler || !s_pipe2d || !s_pipe2dSub ||
        !s_vertexBuffer || !s_vertexTransfer || !s_overlay.vertices ||
        !s_overlay.spans) {
        fprintf(stderr, "rage-port: modern renderer resource setup failed: %s\n",
                SDL_GetError());
        ModernDestroyResources();
        return 0;
    }
    s_resourcesReady = 1;
    s_resourceGeneration++;
    fprintf(stderr, "rage-port: %s renderer target %dx%d\n",
            s_enabled ? "modern" : "classic", s_targetW,
            s_targetH);
    if (RuntimeConfigEnabled("diagnostics.renderer_lifecycle"))
        fprintf(stderr,
                "rage-port: modern resources created generation=%u size=%dx%d\n",
                s_resourceGeneration, s_targetW, s_targetH);
    return 1;
}

/* ---- frame building ---- */

enum {
    MODERN_LAYER_SKY,
    MODERN_LAYER_HUD,
    MODERN_LAYER_MIRROR_FOREGROUND,
    MODERN_LAYER_PAUSE_FOREGROUND,
};
static const RenderCamera *s_skyPacketCamera;
static const RenderCamera *s_skyPresentationCamera;

/* ---- arbitrary-FPS presentation timing ---- */

static ModernPresentationClock s_presentationClock;

static Uint64 ModernPresentationInterval(void) {
    if (s_config.modernFps > 0)
        return (Uint64)(1000000000.0 / s_config.modernFps);
    const SDL_DisplayMode *mode = s_window != NULL
        ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(s_window)) : NULL;
    Uint64 interval = mode != NULL && mode->refresh_rate > 1.0f
        ? (Uint64)(1000000000.0 / mode->refresh_rate) : 16666667u;
    static Uint64 reportedInterval;
    if (interval != reportedInterval) {
        fprintf(stderr, "rage-port: modern display pacing driver=%s refresh_hz=%.3f source=%s\n",
            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "unavailable",
            1000000000.0 / (double)interval, mode ? "display" : "fallback");
        reportedInterval = interval;
    }
    return interval;
}

static int ModernShouldInterpolate(const RageSceneSnapshot *snapshot) {
    return s_config.modernFps != RAGE_MODERN_FPS_LOGIC &&
           snapshot != NULL &&
           GameRenderWorldSceneCanInterpolate(
               (GameSceneId)snapshot->sceneId, snapshot->sceneTimer);
}

void ModernLogicFrameReady(uint32_t frame) {
    ModernPresentationClockObserveFrame(&s_presentationClock, frame,
                                        SDL_GetTicksNS());
    /* The game has published an immutable semantic world at this point.
     * The logic-rate presenter may still consume the preceding publication,
     * while high-FPS uses an interpolation of both. Prepare both immutable
     * snapshots once per tick so neither presentation can turn a cache miss
     * into a render hitch at a scene or camera transition. */
    if (s_enabled) {
        ModernAssetsPrepareWorld(GameRenderWorldPrevious());
        ModernAssetsPrepareWorld(GameRenderWorldCurrent());
    }
}

/* ---- 2D packet replay ---- */

static void ModernOrtho(ModernVertex *out, float px, float py) {
    /* The 2D layer stays 4:3, centered inside a widened target. Edge
     * mapping, not the compat rasterizer's half-pixel centres: at scale
     * factors above 1 the +0.5 convention shifts the whole layer by half a
     * logical pixel, leaving one-line gaps around full-screen masks. */
    out->x = (px + s_overscanX) / (s_logicalW * 0.5f) - 1.0f;
    out->y = -(py / 120.0f - 1.0f);
    out->z = 0.0f;
    out->w = 1.0f;
}

static void ClassicEmitPauseLabel(const Modern2DState *state,
                                  const Modern2DState *spanState,
                                  int x, int y, uint8_t intensity) {
    static const char pixels[7][41] = {
        "012566f00012bf00016f012f00b2356fd012569f",
        "022ed59f00215f00015f021f00216fffd022ffff",
        "021f01bf02256f0001af022f00825e000022f000",
        "022821f0622f4dc0529f022f000065c000146160",
        "614ceff0127718f0329f013f0000b66d0626fff0",
        "326fff0622ff35f0328f02bf0628b16f0313f000",
        "323f00022ff016f0616321ff031221ff03263210",
    };
    static const uint16_t palette[16] = {
        0x0000, 0x7BDE, 0x77BD, 0x739C, 0x6739, 0x6318, 0x5AD6, 0x56B5,
        0x4E73, 0x39CE, 0x318C, 0x294A, 0x1CE7, 0x1084, 0x0842, 0x0800,
    };
    uint8_t previousLayer = s_overlay.currentLayer;

    s_overlay.currentLayer = MODERN_LAYER_PAUSE_FOREGROUND;
    for (int row = 0; row < 7; row++) {
        for (int column = 0; column < 40;) {
            int first = column;
            int index = pixels[row][column] <= '9'
                            ? pixels[row][column] - '0'
                            : pixels[row][column] - 'a' + 10;
            uint16_t source;
            ModernVertex corners[4];
            ModernSpan *span;

            column++;
            if (index == 0) continue;
            while (column < 40 &&
                   pixels[row][column] == pixels[row][first])
                column++;
            source = palette[index];
            for (int vertex = 0; vertex < 4; vertex++) {
                ModernVertex *out = &corners[vertex];
                int right = vertex & 1;
                int bottom = vertex >> 1;
                memset(out, 0, sizeof(*out));
                ModernOrtho(out, (float)(x + (right ? column : first)),
                            (float)(y + row + bottom));
                out->color[0] = (uint8_t)((((source >> 0) & 31) * intensity) >> 4);
                out->color[1] = (uint8_t)((((source >> 5) & 31) * intensity) >> 4);
                out->color[2] = (uint8_t)((((source >> 10) & 31) * intensity) >> 4);
                out->color[3] = 255;
                out->attr = state->tpage | 0x8000u;
                out->twin = state->twin;
            }
            span = ModernOverlayBatchesBegin(&s_overlay, MODERN_PIPE_2D,
                                              spanState);
            ModernOverlayBatchesEmitQuad(&s_overlay, span, corners);
        }
    }
    s_overlay.currentLayer = previousLayer;
}

static void ModernReplay2DPacket(const RageCapturePacket *packet,
                                 Modern2DState *state) {
    const uint32_t *words = packet->words;
    uint32_t command = words[0] >> 24;
    if (command >= 0xE0) {
        /* Environment packets (DR_MODE, DR_TWIN, DR_AREA...) carry one
         * E-command per word; apply them all. */
        int word;
        for (word = 0; word < packet->size; word++) {
            ModernOverlayStateApplyWord(state, words[word]);
        }
        return;
    }
    if (state->areaEmpty) return;
    {
        int isPoly = (command & 0xE0u) == 0x20u;
        int isLine = (command & 0xE0u) == 0x40u;
        int isRect = (command & 0xE0u) == 0x60u;
        int textured = (command & 0x04u) != 0;
        int gouraud = (command & 0x10u) != 0;
        int quad = (command & 0x08u) != 0;
        int semi = (command & 0x02u) != 0;
        int raw = (command & 0x01u) != 0;
        Modern2DState spanState = *state;
        if (CapturePacketIsMainSky(packet)) {
            /* The PS1 drawing area is 320 pixels wide. The extended authored
             * grid is intentionally allowed into the native ultrawide area;
             * the render target remains its final clip boundary. */
            spanState.hasScissor = 0;
        }
        /* The overlay renders at 320x240 logical coordinates; scale the
         * scissor to the target. */
        /* Widen only the main full-width drawing area. Mirror and animated
         * partial clipping rectangles keep their original boundaries. */
        if (!s_enabled && spanState.hasScissor && packet->table == 0 &&
            spanState.scissor.x == 0 && spanState.scissor.w >= 320) {
            int margin = (int)ceilf(s_overscanX);
            spanState.scissor.x -= margin;
            spanState.scissor.w += 2 * margin;
        }
        if (spanState.hasScissor) {
            ModernOverlayStateScaleScissor(&spanState.scissor, s_logicalW,
                                           s_overscanX, s_targetW, s_targetH);
            if (spanState.scissor.w <= 0 || spanState.scissor.h <= 0) return;
        }
        if (isPoly) {
            ModernVertex corners[4];
            uint32_t prim_tpage = state->tpage;
            uint32_t clut = 0;
            int count = quad ? 4 : 3;
            int vertex;
            int cursor = 1; /* word 0 = command + colour 0 */
            uint32_t colors[4];
            float rawX[4], rawY[4];
            int minX = 4096, minY = 4096, maxX = -4096, maxY = -4096;
            colors[0] = words[0] & 0xFFFFFFu;
            for (vertex = 0; vertex < count; vertex++) {
                uint32_t xy;
                if (vertex > 0) {
                    if (gouraud) colors[vertex] = words[cursor++] & 0xFFFFFFu;
                    else colors[vertex] = colors[0];
                }
                xy = words[cursor++];
                {
                    int px = (int)((xy & 0x7FFu) ^ 1024) - 1024 +
                             state->offsetX;
                    int py = (int)(((xy >> 16) & 0x7FFu) ^ 1024) - 1024 +
                             state->offsetY;
                    rawX[vertex] = (float)px;
                    rawY[vertex] = (float)py;
                    if (px < minX) minX = px;
                    if (px > maxX) maxX = px;
                    if (py < minY) minY = py;
                    if (py > maxY) maxY = py;
                }
                memset(&corners[vertex], 0, sizeof(corners[vertex]));
                if (textured) {
                    uint32_t uvWord = words[cursor++];
                    corners[vertex].u = (float)(uvWord & 0xFFu);
                    corners[vertex].v = (float)((uvWord >> 8) & 0xFFu);
                    if (vertex == 0) clut = (uvWord >> 16) & 0xFFFFu;
                    if (vertex == 1) prim_tpage = (uvWord >> 16) & 0x1FFu;
                }
            }
            /* A polygon carries its texture page in the primitive. Unlike a
             * GP0(E1) draw-mode command, it does not change the page used by
             * later sprites. Classic includes the race's 3D polygons here;
             * leaking their pages hid the PAUSE sprite that follows them. */
            /* Full-screen overlays (fades, night filters) stretch across a
             * widened view; otherwise they mask only the 4:3 centre. */
            if (packet->table == 0 && !(packet->flags & RAGE_CAPTURE_PACKET_3D) &&
                s_overscanX > 0.0f && minX <= 0 && minY <= 0 && maxX >= 320 &&
                maxY >= 240) {
                for (vertex = 0; vertex < count; vertex++) {
                    if (rawX[vertex] <= 0.0f) rawX[vertex] = -s_overscanX;
                    else if (rawX[vertex] >= 320.0f)
                        rawX[vertex] = 320.0f + s_overscanX;
                }
            }
            {
                if (!s_enabled && (packet->flags & RAGE_CAPTURE_PACKET_3D))
                    ClassicMotionCoordinates(s_classicPacketIndex,
                                             s_classicFraction, rawX, rawY);
                int smoothSky = textured && quad &&
                    CapturePacketIsMainSky(packet) &&
                    ModernSkyBuildSmoothQuad(packet, s_skyPacketCamera,
                                             s_skyPresentationCamera, rawX, rawY);
                for (vertex = 0; vertex < count; vertex++) {
                    if (!smoothSky &&
                        CapturePacketIsMainSky(packet))
                    ModernSkyReprojectPoint(s_skyPacketCamera,
                                            s_skyPresentationCamera,
                                            &rawX[vertex], &rawY[vertex]);
                }
                if (!textured && quad &&
                    CapturePacketIsMainSky(packet))
                    ModernSkyExtendBand(rawX, rawY, s_logicalW);
                for (vertex = 0; vertex < count; vertex++)
                    ModernOrtho(&corners[vertex], rawX[vertex], rawY[vertex]);
            }
            for (vertex = 0; vertex < count; vertex++) {
                ModernVertex *out = &corners[vertex];
                uint32_t color = raw && textured ? 0x808080u : colors[vertex];
                out->color[0] = (uint8_t)(color & 0xFFu);
                out->color[1] = (uint8_t)((color >> 8) & 0xFFu);
                out->color[2] = (uint8_t)((color >> 16) & 0xFFu);
                out->color[3] = (uint8_t)(semi ? 0 : 255);
                out->attr = textured ? prim_tpage : (prim_tpage | 0x8000u);
                out->twin = state->twin;
                out->clut = clut;
            }
            {
                uint32_t abr = (prim_tpage >> 5) & 3u;
                int pipeline = (semi && abr == 2u) ? MODERN_PIPE_2D_SUB
                                                   : MODERN_PIPE_2D;
                ModernSpan *span = ModernOverlayBatchesBegin(&s_overlay,
                                                              pipeline, &spanState);
                if (quad) ModernOverlayBatchesEmitQuad(&s_overlay, span, corners);
                else ModernOverlayBatchesEmitTriangle(&s_overlay, span, corners);
            }
        } else if (isRect) {
            ModernVertex corners[4];
            int sizeMode = (int)((command >> 3) & 3u);
            int cursor = 1;
            uint32_t xy, uvWord = 0, clut = 0;
            int px, py, w, h;
            int u0 = 0, v0 = 0;
            int pauseForeground;
            xy = words[cursor++];
            px = (int)((xy & 0x7FFu) ^ 1024) - 1024;
            py = (int)(((xy >> 16) & 0x7FFu) ^ 1024) - 1024;
            if (textured) {
                uvWord = words[cursor++];
                u0 = (int)(uvWord & 0xFFu);
                v0 = (int)((uvWord >> 8) & 0xFFu);
                clut = (uvWord >> 16) & 0xFFFFu;
            }
            if (sizeMode == 0) {
                uint32_t wh = words[cursor++];
                w = (int)(wh & 0x3FFu);
                h = (int)((wh >> 16) & 0x1FFu);
            } else if (sizeMode == 1) {
                w = h = 1;
            } else if (sizeMode == 2) {
                w = h = 8;
            } else {
                w = h = 16;
            }
            pauseForeground = !s_enabled && textured &&
                              u0 == 0xD8 && v0 == 0x38 &&
                              clut == 0x7893 && w == 0x28 && h == 8;
            px += state->offsetX;
            py += state->offsetY;
            if (pauseForeground) {
                ClassicEmitPauseLabel(state, &spanState, px, py,
                                      (uint8_t)(words[0] & 0xFFu));
                return;
            }
            if (packet->table == 0 && s_overscanX > 0.0f && !textured && px <= 0 && py <= 0 &&
                px + w >= 320 && py + h >= 240) {
                /* Full-screen fade/filter tiles cover the widened view. */
                px = (int)(-s_overscanX - 1.0f);
                w = 320 - 2 * px;
            }
            {
                int vertex;
                for (vertex = 0; vertex < 4; vertex++) {
                    ModernVertex *out = &corners[vertex];
                    int right = vertex & 1;
                    int bottom = vertex >> 1;
                    memset(out, 0, sizeof(*out));
                    ModernOrtho(out, (float)(px + (right ? w : 0)),
                                (float)(py + (bottom ? h : 0)));
                    out->u = (float)(u0 + (right ? w : 0));
                    out->v = (float)(v0 + (bottom ? h : 0));
                    {
                        uint32_t color = raw && textured ? 0x808080u
                                                        : (words[0] & 0xFFFFFFu);
                        out->color[0] = (uint8_t)(color & 0xFFu);
                        out->color[1] = (uint8_t)((color >> 8) & 0xFFu);
                        out->color[2] = (uint8_t)((color >> 16) & 0xFFu);
                        out->color[3] = (uint8_t)(semi ? 0 : 255);
                    }
                    out->attr = textured ? state->tpage
                                         : (state->tpage | 0x8000u);
                    out->twin = state->twin;
                    out->clut = clut;
                }
            }
            {
                uint32_t abr = (state->tpage >> 5) & 3u;
                int pipeline = (semi && abr == 2u) ? MODERN_PIPE_2D_SUB
                                                   : MODERN_PIPE_2D;
                ModernSpan *span;
                span = ModernOverlayBatchesBegin(&s_overlay, pipeline,
                                                  &spanState);
                ModernOverlayBatchesEmitQuad(&s_overlay, span, corners);
            }
        } else if (isLine) {
            /* Fixed two/three-point lines; expand each segment to a thin
             * quad, PS1-style single-pixel thickness. */
            int points[8][2];
            uint32_t colors[8];
            int count = 0;
            int cursor = 1;
            int vertex;
            colors[0] = words[0] & 0xFFFFFFu;
            while (cursor < packet->size && count < 8) {
                uint32_t word;
                if (count > 0 && gouraud) {
                    if (cursor >= packet->size) break;
                    colors[count] = words[cursor++] & 0xFFFFFFu;
                }
                if (cursor >= packet->size) break;
                word = words[cursor++];
                if ((word & 0xF000F000u) == 0x50005000u && count >= 2) break;
                points[count][0] =
                    (int)((word & 0x7FFu) ^ 1024) - 1024 + state->offsetX;
                points[count][1] =
                    (int)(((word >> 16) & 0x7FFu) ^ 1024) - 1024 +
                    state->offsetY;
                if (count > 0 && !gouraud) colors[count] = colors[0];
                count++;
            }
            for (vertex = 0; vertex + 1 < count; vertex++) {
                ModernVertex corners[4];
                int x0 = points[vertex][0], y0 = points[vertex][1];
                int x1 = points[vertex + 1][0], y1 = points[vertex + 1][1];
                int horizontal = abs(x1 - x0) >= abs(y1 - y0);
                int corner;
                for (corner = 0; corner < 4; corner++) {
                    ModernVertex *out = &corners[corner];
                    int end = corner & 1;
                    int side = corner >> 1;
                    float px = (float)(end ? x1 : x0);
                    float py = (float)(end ? y1 : y0);
                    if (horizontal) py += side;
                    else px += side;
                    memset(out, 0, sizeof(*out));
                    ModernOrtho(out, px, py);
                    {
                        uint32_t color = colors[end ? vertex + 1 : vertex];
                        out->color[0] = (uint8_t)(color & 0xFFu);
                        out->color[1] = (uint8_t)((color >> 8) & 0xFFu);
                        out->color[2] = (uint8_t)((color >> 16) & 0xFFu);
                        out->color[3] = (uint8_t)(semi ? 0 : 255);
                    }
                    out->attr = state->tpage | 0x8000u;
                    out->twin = state->twin;
                    out->clut = 0;
                }
                {
                    ModernSpan *span =
                        ModernOverlayBatchesBegin(&s_overlay, MODERN_PIPE_2D,
                                                  &spanState);
                    ModernOverlayBatchesEmitQuad(&s_overlay, span, corners);
                }
            }
        }
    }
}

/* ---- captured PS1 2D -> overlay vertex/span lists ---- */

static void ClassicBuildFrame(const RageSceneSnapshot *snapshot) {
    Modern2DState state;
    ModernOverlayBatchesReset(&s_overlay, 0, MODERN_LAYER_HUD);
    ModernOverlayStateInit(&state, snapshot->displayPageY);
    s_skyPacketCamera = s_skyPresentationCamera = NULL;
    /* `snapshot` is CapturePrevious(). Its sky packets were produced by the
     * previous published render world, not by current->previousCamera: a hard
     * camera cut deliberately snaps that history to the new shot. Reusing the
     * snapped camera here interpreted old packets in the new grid and left a
     * stale/misaligned sky for one presentation. */
    const RenderWorld *previous = GameRenderWorldPrevious();
    if (s_classicSkyWorld && s_classicSkyWorld->hasCamera) {
        s_skyPresentationCamera = &s_classicSkyWorld->camera;
        if (previous && previous->hasCamera &&
            previous->camera.skyAssetKey == s_classicSkyWorld->camera.skyAssetKey &&
            previous->camera.skyCloudRow == s_classicSkyWorld->camera.skyCloudRow)
            s_skyPacketCamera = &previous->camera;
    }
    if (s_config.modernFps != RAGE_MODERN_FPS_LOGIC)
        ClassicMotionPrepare(snapshot, CaptureClassicSources(snapshot),
                             CaptureCurrent(), CaptureClassicSources(CaptureCurrent()));
    else ClassicMotionReset();
    /* Capture walked table 0 then table 1 exactly as DrawOTag consumes them.
     * Keep every state packet, background, subdivision and translucent face
     * in that order. The overlay pipeline is affine and never writes depth. */
    for (int i = 0; i < snapshot->packetCount; ++i) {
        s_classicPacketIndex = i;
        ModernReplay2DPacket(&snapshot->packets[i], &state);
    }
    s_classicPacketIndex = -1;
    if (RuntimeConfigEnabled("diagnostics.classic_trace")) {
        ClassicMotionStats motion = ClassicMotionGetStats();
        static uint32_t motionFrame = UINT32_MAX;
        if (motionFrame != snapshot->frameCounter) {
            motionFrame = snapshot->frameCounter;
            fprintf(stderr, "classic-motion frame=%u scene=%d polygons=%d candidates=%d face_parents=%d moving=%d course_polygons=%d course_moving=%d\n",
                    snapshot->frameCounter, snapshot->sceneId, motion.polygons,
                    motion.candidates, motion.faceParents, motion.moving, motion.coursePolygons,
                    motion.courseMoving);
        }
        uint64_t hash = UINT64_C(14695981039346656037);
        for (int i = 0; i < s_overlay.vertexCount; ++i) {
            const unsigned char *xy = (const unsigned char *)&s_overlay.vertices[i].x;
            for (size_t byte = 0; byte < 2 * sizeof(float); ++byte)
                hash = (hash ^ xy[byte]) * UINT64_C(1099511628211);
        }
        fprintf(stderr, "classic-frame frame=%u t=%.4f packets=%d matched=%d vertices=%d spans=%d xy_hash=%016llx\n",
                snapshot->frameCounter, s_classicFraction, snapshot->packetCount,
                ClassicMotionMatchCount(), s_overlay.vertexCount, s_overlay.spanCount,
                (unsigned long long)hash);
    }
}

/* Set while the overlay is built for the XR HUD panel. */
static int s_xrHudBuild;

/* An untextured quad or rectangle covering (nearly) the whole 320x240
 * screen: the game's fades and flashes. On the XR HUD panel these would turn
 * the floating panel into a solid board, so the panel build leaves them out. */
static int ModernPacketIsScreenFill(const RageCapturePacket *packet,
                                    const Modern2DState *state) {
    const uint32_t *words = packet->words;
    uint32_t command = words[0] >> 24;
    int minX = 4096, minY = 4096, maxX = -4096, maxY = -4096;
    if ((command & 0x04u) != 0) return 0; /* textured */
    if ((command & 0xE0u) == 0x20u && (command & 0x08u) != 0) {
        int gouraud = (command & 0x10u) != 0;
        int cursor = 1, vertex;
        for (vertex = 0; vertex < 4; vertex++) {
            uint32_t xy;
            int px, py;
            if (vertex > 0 && gouraud) cursor++;
            if (cursor >= packet->size) return 0;
            xy = words[cursor++];
            px = (int)((xy & 0x7FFu) ^ 1024) - 1024 + state->offsetX;
            py = (int)(((xy >> 16) & 0x7FFu) ^ 1024) - 1024 + state->offsetY;
            if (px < minX) minX = px;
            if (px > maxX) maxX = px;
            if (py < minY) minY = py;
            if (py > maxY) maxY = py;
        }
    } else if ((command & 0xF8u) == 0x60u && packet->size >= 3) {
        /* Variable-size rectangle: word 1 position, word 2 size. */
        minX = (int)((words[1] & 0x7FFu) ^ 1024) - 1024 + state->offsetX;
        minY = (int)(((words[1] >> 16) & 0x7FFu) ^ 1024) - 1024 +
               state->offsetY;
        maxX = minX + (int)(words[2] & 0x3FFu);
        maxY = minY + (int)((words[2] >> 16) & 0x1FFu);
    } else {
        return 0;
    }
    return minX <= 16 && maxX >= 304 && minY <= 16 && maxY >= 224;
}

static void ModernBuildOverlayFrame(const RageSceneSnapshot *snapshot) {

    Modern2DState state2d;
    int i;

    ModernOverlayBatchesReset(&s_overlay, 0, MODERN_LAYER_HUD);
    s_skyPacketCamera = NULL;
    s_skyPresentationCamera = NULL;
    if (s_config.modernFps != RAGE_MODERN_FPS_LOGIC) {
        const RenderWorld *previous = GameRenderWorldPrevious();
        const RenderWorld *presentation = ModernNativeGpuPreparedWorld();
        if (presentation != NULL && presentation->hasCamera) {
            s_skyPresentationCamera = &presentation->camera;
            if (previous != NULL && previous->hasCamera &&
                previous->camera.skyAssetKey == presentation->camera.skyAssetKey &&
                previous->camera.skyCloudRow == presentation->camera.skyCloudRow)
                s_skyPacketCamera = &previous->camera;
        }
    }
    if (RuntimeConfigEnabled("diagnostics.sky_trace") &&
        s_skyPacketCamera != NULL && s_skyPresentationCamera != NULL) {
        fprintf(stderr,
                "rage-port: sky-present frame=%u source=%.3f,%.3f,%.3f "
                "target=%.3f,%.3f,%.3f\n",
                snapshot->frameCounter,
                s_skyPacketCamera->skyGridOrigin.x,
                s_skyPacketCamera->skyGridOrigin.y,
                s_skyPacketCamera->skyGridColumn.z,
                s_skyPresentationCamera->skyGridOrigin.x,
                s_skyPresentationCamera->skyGridOrigin.y,
                s_skyPresentationCamera->skyGridColumn.z);
    }

    ModernOverlayStateInit(&state2d, snapshot->displayPageY);

    /* DrawSkyBackground's exact screen-space primitives are part of the
     * modern render, not a classic framebuffer fallback. Their source tag is
     * required because far terrain shares the same OT buckets. */
    for (i = 0; i < snapshot->packetCount; i++) {
        const RageCapturePacket *packet = &snapshot->packets[i];
        if (packet->flags & RAGE_CAPTURE_PACKET_3D) continue;
        uint32_t command = packet->words[0] >> 24;
        if (packet->table != 0) continue;
        if ((packet->flags & RAGE_CAPTURE_PACKET_SKY) != 0) {
            s_overlay.currentLayer = MODERN_LAYER_SKY;
            ModernReplay2DPacket(packet, &state2d);
            continue;
        }
        if (packet->bucket >= MODERN_BACKGROUND_BUCKET && command < 0xE0u) {
            continue;
        }
        if (s_xrHudBuild && ModernPacketIsScreenFill(packet, &state2d))
            continue;
        s_overlay.currentLayer = MODERN_LAYER_HUD;
        ModernReplay2DPacket(packet, &state2d);
    }

    /* The native mirror supplies its own world and backdrop. Retain only
     * captured 2D framing/text, seeded with the main table's final GPU state
     * so the original slide-in clipping still applies. */
    s_overlay.currentPass = 1;
    s_overlay.currentLayer = MODERN_LAYER_MIRROR_FOREGROUND;
    for (i = 0; i < snapshot->packetCount; i++) {
        const RageCapturePacket *packet = &snapshot->packets[i];
        if (packet->flags & RAGE_CAPTURE_PACKET_3D) continue;
        uint32_t command = packet->words[0] >> 24;
        if (packet->table != 1) continue;
        if (command < 0xE0u &&
            (!state2d.hasScissor ||
             packet->bucket >= MODERN_BACKGROUND_BUCKET)) {
            continue;
        }
        ModernReplay2DPacket(packet, &state2d);
    }
    s_overlay.currentPass = 0;
    s_overlay.currentLayer = MODERN_LAYER_HUD;
}

/* ---- rendering ---- */

/* The texture the frame chain ends in: composite > fxaa > raw target. */
static SDL_GPUTexture *ModernPresentTexture(void) {
    if (s_config.modernGrading &&
        s_finalTarget != NULL && s_pipeComposite != NULL) {
        return s_finalTarget;
    }
    if (s_config.modernPost != RAGE_MODERN_POST_NONE && s_postTarget != NULL) {
        return s_postTarget;
    }
    return s_target;
}

static void ModernFullscreenPass(SDL_GPUCommandBuffer *cmd,
                                 SDL_GPUGraphicsPipeline *pipeline,
                                 SDL_GPUTexture *target,
                                 SDL_GPUTexture *sources[], int sourceCount) {
    const SDL_GPUColorTargetInfo color = {
        .texture = target,
        .load_op = SDL_GPU_LOADOP_DONT_CARE,
        .store_op = SDL_GPU_STOREOP_STORE,
    };
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color, 1, NULL);
    SDL_GPUTextureSamplerBinding bindings[2];
    int i;
    if (pass == NULL) return;
    for (i = 0; i < sourceCount && i < 2; i++) {
        bindings[i].texture = sources[i];
        bindings[i].sampler = s_samplerLinear;
    }
    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    SDL_BindGPUFragmentSamplers(pass, 0, bindings, (Uint32)sourceCount);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

/* The colour behind the replayed sky tiles. On hardware the sky sits on a
 * frame buffer already filled with sky colour, so any sub-pixel crack between
 * rolled cloud rows or a discarded edge texel is invisible. Clearing to black
 * turned those cracks into horizontal black lines on banked roads (#37). */
static SDL_FColor ModernSkyClearColor(void) {
    SDL_FColor color = {0.0f, 0.0f, 0.0f, 1.0f};
    const RenderCamera *camera = s_skyPresentationCamera;

    if (camera != NULL) {
        color.r = camera->skyColor.x;
        color.g = camera->skyColor.y;
        color.b = camera->skyColor.z;
    }
    return color;
}

static void ModernRenderOverlaySelection(SDL_GPUCommandBuffer *cmd,
                                         SDL_GPUTexture *vram, int passNumber,
                                         uint32_t layerMask, int clearColor) {
    SDL_GPUGraphicsPipeline *pipelines[2] = {s_pipe2d, s_pipe2dSub};
    /* XR draws the HUD alone into its own transparent panel texture. */
    SDL_GPUColorTargetInfo color = {
        .texture = s_overlayTarget != NULL ? s_overlayTarget : s_target,
        .clear_color = s_overlayTarget != NULL
                           ? (SDL_FColor){0.0f, 0.0f, 0.0f, 0.0f}
                       : layerMask == (1u << MODERN_LAYER_SKY)
                           ? ModernSkyClearColor()
                           : (SDL_FColor){0.0f, 0.0f, 0.0f, 1.0f},
        .load_op = clearColor ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD,
        .store_op = SDL_GPU_STOREOP_STORE,
    };
    SDL_GPUDepthStencilTargetInfo depth = {
        .texture = s_depth,
        .clear_depth = 1.0f,
        .load_op = SDL_GPU_LOADOP_CLEAR,
        .store_op = SDL_GPU_STOREOP_DONT_CARE,
    };
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);
    SDL_GPUGraphicsPipeline *bound = NULL;
    SDL_GPUBufferBinding vertex = {.buffer = s_vertexBuffer, .offset = 0};
    SDL_GPUTextureSamplerBinding texture = {.texture = vram,
                                            .sampler = s_sampler};
    int spanIndex;
    if (pass == NULL) return;
    SDL_BindGPUVertexBuffers(pass, 0, &vertex, 1);
    SDL_BindGPUFragmentSamplers(pass, 0, &texture, 1);
    for (spanIndex = 0; spanIndex < s_overlay.spanCount; spanIndex++) {
        const ModernSpan *span = &s_overlay.spans[spanIndex];
        SDL_Rect scissor = {0, 0, s_targetW, s_targetH};
        if (span->pass != passNumber || span->count == 0 ||
            (layerMask & (1u << span->layer)) == 0) continue;
        if (pipelines[span->pipeline] != bound) {
            bound = pipelines[span->pipeline];
            SDL_BindGPUGraphicsPipeline(pass, bound);
        }
        if (span->hasScissor) scissor = span->scissor;
        SDL_SetGPUScissor(pass, &scissor);
        SDL_DrawGPUPrimitives(pass, (Uint32)span->count, 1,
                              (Uint32)span->start, 0);
    }
    if (!s_enabled && (layerMask & (1u << MODERN_LAYER_HUD)) != 0) {
        SDL_Rect full = {0, 0, s_targetW, s_targetH};
        for (spanIndex = 0; spanIndex < s_overlay.spanCount; spanIndex++) {
            const ModernSpan *span = &s_overlay.spans[spanIndex];
            if (span->layer != MODERN_LAYER_PAUSE_FOREGROUND ||
                span->pass != passNumber || span->count == 0)
                continue;
            if (pipelines[span->pipeline] != bound) {
                bound = pipelines[span->pipeline];
                SDL_BindGPUGraphicsPipeline(pass, bound);
            }
            SDL_SetGPUScissor(pass, &full);
            SDL_DrawGPUPrimitives(pass, (Uint32)span->count, 1,
                                  (Uint32)span->start, 0);
        }
    }
    SDL_EndGPURenderPass(pass);
}

static void ModernCompositeNativeMirror(SDL_GPUCommandBuffer *cmd) {
    float panelY = ModernNativeGpuMirrorPanelY();
    float visibleTop = panelY < 0.0f ? 0.0f : panelY;
    float visibleBottom = panelY + 36.0f;
    float sourceTop;
    SDL_GPUBlitInfo blit = {0};

    if (!ModernNativeGpuHasMirrorDraws()) return;
    if (visibleBottom > 240.0f) visibleBottom = 240.0f;
    if (visibleBottom <= visibleTop) return;
    sourceTop = visibleTop - panelY;
    blit.source.texture = s_mirrorTarget;
    blit.source.y = (Uint32)lroundf(
        sourceTop * (float)s_mirrorTargetH / 36.0f);
    blit.source.w = (Uint32)s_mirrorTargetW;
    blit.source.h = (Uint32)lroundf(
        (visibleBottom - visibleTop) * (float)s_mirrorTargetH / 36.0f);
    blit.destination.texture = s_overlayTarget != NULL ? s_overlayTarget
                                                       : s_target;
    blit.destination.x = (Uint32)lroundf(
        (86.0f + s_overscanX) * (float)s_targetW / s_logicalW);
    blit.destination.y = (Uint32)lroundf(
        visibleTop * (float)s_targetH / 240.0f);
    blit.destination.w = (Uint32)lroundf(
        148.0f * (float)s_targetW / s_logicalW);
    blit.destination.h = (Uint32)lroundf(
        (visibleBottom - visibleTop) * (float)s_targetH / 240.0f);
    /* While the panel slides in, an interpolated frame can show a sliver
     * that rounds to no pixels at all; SDL asserts on an empty blit. */
    if (blit.source.w == 0 || blit.source.h == 0 ||
        blit.destination.w == 0 || blit.destination.h == 0) {
        return;
    }
    blit.load_op = SDL_GPU_LOADOP_LOAD;
    /* A physical rear-view mirror swaps left and right. Rendering from a
     * rear camera supplies the scene; flipping at composition supplies the
     * reflection instead of a turn-your-head view. */
    blit.flip_mode = SDL_FLIP_HORIZONTAL;
    blit.filter = s_config.modernTextureFilterLinear
                      ? SDL_GPU_FILTER_LINEAR
                      : SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(cmd, &blit);
}

static SDL_GPUTexture *ModernCaptureVramSnapshot(void *context) {
    (void)context;
    SDL_GPUTexture *source = Psyz_VideoSnapshotVramTexture_SDL3GPU();
    if (!ModernVramSnapshotCopy(s_device, source, &s_ownedSampledVram))
        return NULL;
    return s_ownedSampledVram;
}

static int ModernCompareProfileInterval(const void *a, const void *b) {
    const Uint64 x = *(const Uint64 *)a, y = *(const Uint64 *)b;
    return (x > y) - (x < y);
}

void ModernFrameTexturesReady(void) {
    if (!ModernPresentationActive() || !s_device || !s_resourcesReady) return;
    const RageSceneSnapshot *snapshot = CapturePrevious();
    if (!snapshot->faceCount ||
        (snapshot->displayHeight && snapshot->displayHeight != 240)) return;
    /* Match the existing previous-frame presentation convention. All game
     * transfers have completed here; a subsequent presentation can reuse this
     * private image without sampling a partially updated live VRAM. Resource
     * initialization/recovery can still capture at the normal present hook. */
    SDL_GPUTexture *captured = ModernVramSnapshotForFrame(
        &s_sampledVram, snapshot->frameCounter,
        TrackAssetIdentityRevision(), ModernAssetsGeneration(),
        ModernCaptureVramSnapshot, NULL);
    if (RuntimeConfigEnabled("diagnostics.performance_trace"))
        fprintf(stderr, "presentation-snapshot frame=%u ready=%d\n",
            snapshot->frameCounter, captured != NULL);
}

/* Copies this frame's 2D overlay vertices (sky, HUD, ...) to the GPU.
 * Returns 0 if the upload could not be recorded. */
static int ModernUploadOverlay(SDL_GPUCommandBuffer *cmd) {
    SDL_GPUCopyPass *copy;
    void *mapped;
    if (s_overlay.vertexCount <= 0) return 1;
    mapped = SDL_MapGPUTransferBuffer(s_device, s_vertexTransfer, true);
    if (mapped == NULL) return 0;
    memcpy(mapped, s_overlay.vertices,
           s_overlay.vertexCount * sizeof(ModernVertex));
    SDL_UnmapGPUTransferBuffer(s_device, s_vertexTransfer);
    copy = SDL_BeginGPUCopyPass(cmd);
    if (copy == NULL) return 0;
    {
        const SDL_GPUTransferBufferLocation source = {
            .transfer_buffer = s_vertexTransfer, .offset = 0};
        const SDL_GPUBufferRegion destination = {
            .buffer = s_vertexBuffer,
            .offset = 0,
            .size = (Uint32)(s_overlay.vertexCount * sizeof(ModernVertex))};
        SDL_UploadToGPUBuffer(copy, &source, &destination, true);
    }
    SDL_EndGPUCopyPass(copy);
    return 1;
}

static int ModernRender(const RageSceneSnapshot *snapshot) {
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUTexture *vram;
    static Uint64 profileBuildNs, profileSubmitNs, profilePrepareNs;
    static Uint64 profileFaces, profileVertices, profileSpans;
    static unsigned profileFrames;
    static Uint64 profileWindowStart, profilePrevious, profileIntervals[120];
    static unsigned long long profilePresented;
    static int profile = -1, profileTrace;
    Uint64 profileStart = 0, profileBuilt = 0;
    vram = ModernVramSnapshotForFrame(
        &s_sampledVram, snapshot->frameCounter,
        s_enabled ? ModernNativeGpuTextureRevision() : TrackAssetIdentityRevision(), ModernAssetsGeneration(),
        ModernCaptureVramSnapshot, NULL);
    if (vram == NULL) return 0;
    if (profile < 0) {
        profile = RuntimeConfigEnabled("diagnostics.performance");
        profileTrace = RuntimeConfigEnabled("diagnostics.performance_trace");
    }
    if (profile) profileStart = SDL_GetTicksNS();
    if (s_enabled) ModernBuildOverlayFrame(snapshot);
    else ClassicBuildFrame(snapshot);
    if (profile) profileBuilt = SDL_GetTicksNS();
    cmd = SDL_AcquireGPUCommandBuffer(s_device);
    if (cmd == NULL) return 0;
    if (!ModernUploadOverlay(cmd)) {
        SDL_CancelGPUCommandBuffer(cmd);
        return 0;
    }
    if (s_enabled) {
        static uint64_t reportedIncompleteFrame = UINT64_MAX;
        ModernRenderOverlaySelection(cmd, vram, 0,
                                     1u << MODERN_LAYER_SKY, 1);
        ModernNativeGpuDraw(cmd, s_target, s_depth, 0, 0, s_targetH);
        if (!ModernNativeGpuWorldComplete() &&
            reportedIncompleteFrame != snapshot->frameCounter) {
            reportedIncompleteFrame = snapshot->frameCounter;
            fprintf(stderr,
                    "rage-port: incomplete native world at frame %u; "
                    "legacy 3D fallback is disabled\n",
                    snapshot->frameCounter);
        }
        if (!s_skyOnlyDiagnostic)
            ModernRenderOverlaySelection(cmd, vram, 0,
                                         1u << MODERN_LAYER_HUD, 0);
        if (!s_skyOnlyDiagnostic && ModernNativeGpuHasMirrorDraws()) {
            ModernNativeGpuDrawMirror(cmd, s_mirrorTarget, s_mirrorDepth,
                                      s_mirrorTargetH);
            ModernCompositeNativeMirror(cmd);
        }
        if (!s_skyOnlyDiagnostic)
            ModernRenderOverlaySelection(
                cmd, vram, 1, 1u << MODERN_LAYER_MIRROR_FOREGROUND, 0);
    }
    if (!s_enabled)
        ModernRenderOverlaySelection(cmd, vram, 0, 1u << MODERN_LAYER_HUD, 1);
    {
        SDL_GPUTexture *chain = s_target;

        if (s_config.modernPost != RAGE_MODERN_POST_NONE &&
            s_pipePost != NULL && s_postTarget != NULL) {
            SDL_GPUTexture *sources[1];
            sources[0] = chain;
            ModernFullscreenPass(cmd, s_pipePost, s_postTarget, sources, 1);
            chain = s_postTarget;
        }
        if (s_config.modernGrading &&
            s_pipeComposite != NULL && s_finalTarget != NULL) {
            SDL_GPUTexture *sources[1];
            sources[0] = chain;
            ModernFullscreenPass(cmd, s_pipeComposite, s_finalTarget, sources, 1);
        }
    }
    if (s_ringEnabled && s_ring[s_ringNext] != NULL) {
        const SDL_GPUBlitInfo blit = {
            .source = {.texture = ModernPresentTexture(),
                       .w = (Uint32)s_targetW,
                       .h = (Uint32)s_targetH},
            .destination = {.texture = s_ring[s_ringNext],
                            .w = (Uint32)s_targetW,
                            .h = (Uint32)s_targetH},
            .load_op = SDL_GPU_LOADOP_DONT_CARE,
            .filter = SDL_GPU_FILTER_NEAREST,
        };
        SDL_BlitGPUTexture(cmd, &blit);
        s_ringFrame[s_ringNext] = snapshot->frameCounter;
        if (!RenderWorldSnapshotCopy(&s_ringWorlds[s_ringNext], ModernNativeGpuPreparedWorld())) {
            RenderWorldSnapshotRelease(&s_ringWorlds[s_ringNext]);
            fprintf(stderr, "rage-port: cannot retain render world for history frame %u\n",
                    snapshot->frameCounter);
        }
        TrackTextureGenerationRelease(s_ringGenerations[s_ringNext]);
        s_ringGenerations[s_ringNext] = NativeAssetImporterRetainTextures(
            ModernNativeGpuTextureRevision());
        s_ringT[s_ringNext] = -1.0f;
        if (s_ringScene != NULL) {
            memcpy(&s_ringScene[s_ringNext], snapshot, sizeof(*snapshot));
        }
        s_ringNext = (s_ringNext + 1) % MODERN_RING;
    }
    /* All uploads/cache identities above are speculative until submission.
     * Retire the complete presentation resource generation on failure: a
     * texture-cache hit must never refer to an upload that was discarded. */
    static int injectedSubmitFailure, failureConfigured, failureRequested;
    static unsigned long requestedFrame;
    if (!failureConfigured) {
        const char *failFrame = SDL_getenv("RAGE_PORT_MODERN_FAIL_SUBMIT_FRAME");
        char *failEnd = NULL;
        requestedFrame = failFrame != NULL ? strtoul(failFrame, &failEnd, 10) : 0;
        failureRequested = failFrame != NULL && failEnd != failFrame &&
            *failEnd == '\0';
        failureConfigured = 1;
    }
    int inject = !injectedSubmitFailure && failureRequested &&
                 requestedFrame == snapshot->frameCounter;
    int submitted;
    if (inject) {
        injectedSubmitFailure = 1;
        SDL_CancelGPUCommandBuffer(cmd);
        submitted = 0;
        fprintf(stderr, "rage-port: injected modern submit failure frame=%u\n",
                snapshot->frameCounter);
    } else if (s_presentOffscreen) {
        /* SDL Vulkan normally retires completed command buffers during a
         * swapchain submission. With a claimed window but no swapchain work
         * (offline captures), that cleanup never runs. Explicitly retire the
         * fence so cycled vertex/VRAM allocations cannot grow every frame. */
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        submitted = fence != NULL;
        if (fence != NULL) {
            submitted = SDL_WaitForGPUFences(s_device, true, &fence, 1);
            SDL_ReleaseGPUFence(s_device, fence);
        }
    } else {
        submitted = SDL_SubmitGPUCommandBuffer(cmd);
    }
    if (!submitted) {
        fprintf(stderr, "rage-port: modern submit failed frame=%u; rebuilding presentation resources\n",
                snapshot->frameCounter);
        fprintf(stderr, "rage-port: GPU submission error: %s\n",
                inject ? "injected failure" : SDL_GetError());
        ModernDestroyResources();
        return 0;
    }
    if (s_enabled) ModernNativeGpuSubmitted();
    if (profile) {
        Uint64 finished = SDL_GetTicksNS();
        static int frameTiming = -1;
        if (frameTiming < 0)
            frameTiming = RuntimeConfigEnabled("diagnostics.frame_timing");
        if (frameTiming)
            fprintf(stderr, "benchmark-frame frame=%u scene=%d point=%d end_ns=%llu window_flags=%llx\n",
                    snapshot->frameCounter, snapshot->sceneId,
                    g_PlayerCar.trackPointIndex, (unsigned long long)finished,
                    (unsigned long long)(s_window ? SDL_GetWindowFlags(s_window) : 0));
        if (!profileWindowStart) {
            PsyzVideoStats initialStats = {0};
            Psyz_VideoStats(&initialStats);
            profileWindowStart = profileStart;
            profilePresented = initialStats.presented_frames;
        }
        profileIntervals[profileFrames] = profilePrevious
            ? finished - profilePrevious : finished - profileStart;
        profilePrevious = finished;
        if (profileTrace) {
            const RenderWorld *world = ModernNativeGpuPreparedWorld();
            fprintf(stderr,
                    "modern-frame frame=%u scene=%d point=%d interval_ms=%.3f "
                    "prepare_ms=%.3f build_ms=%.3f submit_ms=%.3f instances=%u end_ns=%llu\n",
                    snapshot->frameCounter, snapshot->sceneId,
                    g_PlayerCar.trackPointIndex,
                    profileIntervals[profileFrames] / 1000000.0,
                    s_profilePrepareNs / 1000000.0,
                    (profileBuilt - profileStart) / 1000000.0,
                    (finished - profileBuilt) / 1000000.0,
                    world != NULL ? (unsigned)world->instanceCount : 0u,
                    (unsigned long long)finished);
        }
        profileBuildNs += profileBuilt - profileStart;
        profilePrepareNs += s_profilePrepareNs;
        profileSubmitNs += finished - profileBuilt;
        profileFaces += (Uint64)snapshot->faceCount;
        profileVertices += (Uint64)s_overlay.vertexCount;
        profileSpans += (Uint64)s_overlay.spanCount;
        profileFrames++;
        if (profileFrames == 120) {
            PsyzVideoStats videoStats = {0};
            Psyz_VideoStats(&videoStats);
            qsort(profileIntervals, 120, sizeof(profileIntervals[0]),
                  ModernCompareProfileInterval);
            fprintf(stderr,
                    "%s-profile frames=%u build_ms=%.3f submit_ms=%.3f "
                    "faces=%.0f vertices=%.0f spans=%.0f "
                    "render_fps=%.2f interval_p95_ms=%.3f interval_max_ms=%.3f "
                    "queued_fps=%.2f prepare_ms=%.3f\n",
                    s_enabled ? "modern" : "classic", profileFrames,
                    (double)profileBuildNs / profileFrames / 1000000.0,
                    (double)profileSubmitNs / profileFrames / 1000000.0,
                    (double)profileFaces / profileFrames,
                    (double)profileVertices / profileFrames,
                    (double)profileSpans / profileFrames,
                    120.0e9 / (double)(finished - profileWindowStart),
                    profileIntervals[113] / 1000000.0,
                    profileIntervals[119] / 1000000.0,
                    (videoStats.presented_frames - profilePresented) * 1.0e9 /
                        (double)(finished-profileWindowStart),
                    (double)profilePrepareNs / profileFrames / 1000000.0);
            profilePresented = videoStats.presented_frames;
            profileWindowStart = finished;
            profileBuildNs = profileSubmitNs = profilePrepareNs = 0;
            profileFaces = profileVertices = profileSpans = 0;
            profileFrames = 0;
        }
    }
    s_haveRenderedFrame = 1;
    return 1;
}

static RageModernDiagnosticFrame ModernDiagnosticFrame(void) {
    RageModernDiagnosticFrame frame = {
        .device = s_device,
        .texture = ModernPresentTexture(),
        .sampledVram = s_sampledVram.valid ? s_sampledVram.texture : NULL,
        .sampledVramFrame = s_sampledVram.frame,
        .width = s_targetW,
        .height = s_targetH,
        .logicalWidth = s_logicalW,
        .fps = s_config.modernFps,
        .ringTextures = s_ring,
        .ringFrames = s_ringFrame,
        .ringInterpolation = s_ringT,
        .ringScenes = s_ringScene,
        .ringWorlds = s_ringWorlds,
        .ringGenerations = s_ringGenerations,
        .ringCount = s_ringEnabled ? MODERN_RING : 0,
        .ringNext = s_ringNext,
    };
    return frame;
}

/* Diagnostic: write the modern target as a binary PPM when
 * RAGE_PORT_MODERN_DUMP names a path and RAGE_PORT_MODERN_DUMP_FRAME (if
 * set) matches the captured frame counter. */
static void ModernMaybeDump(const RageSceneSnapshot *snapshot) {
    if (!s_enabled && s_config.modernFps != RAGE_MODERN_FPS_LOGIC &&
        s_classicFraction < 0.5f &&
        RuntimeConfigEnabled("diagnostics.classic_dump_midpoint")) return;
    RageModernDiagnosticFrame frame = ModernDiagnosticFrame();
    ModernDiagnosticsMaybeDump(snapshot, &frame);
}

/* Debug marker: pressing M writes markers/marker-N-{modern,compat}.ppm,
 * an info.txt with camera/draw/terrain state and the raw scene snapshot,
 * so a moment the player flags can be analysed offline. Works in menus and
 * attract too (compat image only when the scene has no 3D). */
static void ModernMarkerCheck(const RageSceneSnapshot *snapshot,
                              int haveModernImage) {
    RageModernDiagnosticFrame frame = ModernDiagnosticFrame();
    ModernDiagnosticsCheckMarker(snapshot, &frame, haveModernImage);
}

/* ---- hooks ---- */

static void ModernReleaseDevice(void) {
    if (s_device != NULL) SDL_WaitForGPUIdle(s_device);
    ModernDestroyResources();
    s_device = NULL;
    s_window = NULL;
}

static void ModernOverlayDestroy(void) {
    ModernReleaseDevice();
    /* Device destruction/reset is not the end of the content session. */
    if (s_prev_overlay_destroy) s_prev_overlay_destroy();
}

static void ModernOverlayInit(SDL_Window *window, SDL_GPUDevice *device) {
    if (s_device != NULL && s_device != device) {
        ModernReleaseDevice();
    }
    s_window = window;
    s_device = device;
    if (s_prev_overlay_init) {
        s_prev_overlay_init(window, device);
    }
}

static int ModernWindowOccluded(void) {
    return s_window && (SDL_GetWindowFlags(s_window) &
        (SDL_WINDOW_OCCLUDED | SDL_WINDOW_MINIMIZED)) != 0;
}

static int ModernEnsureHudTarget(void) {
    static int hudW, hudH;
    SDL_GPUTextureCreateInfo info = {0};
    if (s_hudTarget != NULL && hudW == s_targetW && hudH == s_targetH)
        return 1;
    if (s_hudTarget != NULL) SDL_ReleaseGPUTexture(s_device, s_hudTarget);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                 SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = (Uint32)s_targetW;
    info.height = (Uint32)s_targetH;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    s_hudTarget = SDL_CreateGPUTexture(s_device, &info);
    hudW = s_hudTarget ? s_targetW : 0;
    hudH = s_hudTarget ? s_targetH : 0;
    return s_hudTarget != NULL;
}

static float ModernXrGradeSetting(const char *text, float minimum,
                                  float maximum) {
    float value = text != NULL ? strtof(text, NULL) : 1.0f;
    if (!(value >= minimum && value <= maximum)) value = 1.0f;
    return value;
}

/* The VR graphics menu's MIRROR ([xr] mirror = off | half | full): 0 hides
 * the in-car mirror, else it is re-rendered every Nth XR frame (half 2, the
 * default; full 1). Re-read only when the settings generation changes. */
static int ModernXrMirrorRate(void) {
    static int rate;
    static unsigned generation;
    static int loaded;
    unsigned now = GraphicsSettingsGeneration();
    if (!loaded || generation != now) {
        const char *mode = XrProfileSetting("xr.mirror");
        loaded = 1;
        generation = now;
        rate = 2;
        if (mode != NULL && SDL_strcasecmp(mode, "off") == 0) rate = 0;
        if (mode != NULL && SDL_strcasecmp(mode, "full") == 0) rate = 1;
    }
    return rate;
}

/* [xr] foveation = off | low | medium | high (default): density falls from full at
 * inner_radius to min_density at outer_radius (UV units from each eye's view
 * centre). The GPU shades 1x1, 2x2 or 4x4 pixel blocks (density 1, 0.5,
 * 0.25), so min 0.5 halves the edge resolution each way. */
typedef struct {
    const char *name;
    float inner, outer, minDensity;
} ModernFoveationLevel;

static const ModernFoveationLevel s_foveationLevels[] = {
    {"low", 0.40f, 0.70f, 0.5f},
    {"medium", 0.30f, 0.60f, 0.5f},
    {"high", 0.25f, 0.55f, 0.25f},
};

/* Development aid: adb shell setprop debug.rr.fov 0..3 (off, low, medium,
 * high) overrides the ini live; the eye targets are rebuilt on a change. */
static int ModernFoveationOverride(void) {
    int value = XrDebugProp("fov", -1);
    return value >= 0 && value <= 3 ? value : -1;
}

/* The xr.foveation setting (VR graphics menu, else ini, else profile),
 * re-read only when the settings generation changes. */
static const ModernFoveationLevel *ModernFoveationSetting(void) {
    const ModernFoveationLevel *levels = s_foveationLevels;
    static const ModernFoveationLevel *level;
    static unsigned generation;
    static int loaded;
    unsigned now = GraphicsSettingsGeneration();
    const char *value;
    size_t i;
    if (loaded && generation == now) return level;
    loaded = 1;
    generation = now;
    value = XrProfileSetting("xr.foveation");
    /* Default high: about 1.3 ms of a ~14 ms canyon frame on a Quest 3
     * (A/B while racing), edges not noticed in play. */
    if (value == NULL) return level = &levels[2];
    for (i = 0; i < SDL_arraysize(s_foveationLevels); i++)
        if (SDL_strcasecmp(value, levels[i].name) == 0)
            return level = &levels[i];
    if (SDL_strcasecmp(value, "off") != 0)
        fprintf(stderr, "rage-port: unknown xr.foveation=%s (off, low, "
                "medium, high)\n", value);
    return level = NULL;
}

static const ModernFoveationLevel *ModernFoveation(void) {
    const ModernFoveationLevel *levels = s_foveationLevels;
    int override = ModernFoveationOverride();
    if (override >= 0) return override > 0 ? &levels[override - 1] : NULL;
    return ModernFoveationSetting();
}

/* What the eye targets and MSAA pipelines are built for: xr.msaa on/off and
 * xr.msaa_samples (0 when off), re-read when the settings generation
 * changes. */
static int ModernXrMsaaWanted(void) {
    static int wanted;
    static unsigned generation;
    static int loaded;
    unsigned now = GraphicsSettingsGeneration();
    if (!loaded || generation != now) {
        loaded = 1;
        generation = now;
        wanted = XrProfileEnabled("xr.msaa")
                     ? (int)ModernNativeGpuEyeSampleCount() + 1
                     : 0;
    }
    return wanted;
}

/* A density map for one eye, centred where the eye looks straight ahead
 * (the views are asymmetric: each eye's centre sits towards the nose).
 * NULL when off or unsupported; the eye then renders at full rate. */
static SDL_GPUTexture *ModernCreateEyeFdm(unsigned w, unsigned h,
                                         const PsyzXrView *view) {
    const ModernFoveationLevel *level = ModernFoveation();
    SDL_GPUTextureCreateInfo info = {0};
    SDL_GPUTexture *fdm;
    float cx = 0.5f, cy = 0.5f;
    if (level == NULL) return NULL;
    if (view->tan_right > view->tan_left)
        cx = -view->tan_left / (view->tan_right - view->tan_left);
    if (view->tan_up > view->tan_down)
        cy = view->tan_up / (view->tan_up - view->tan_down);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
    info.width = w;
    info.height = h;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.props = SDL_CreateProperties();
    SDL_SetBooleanProperty(info.props, SDL_PROP_GPU_TEXTURE_CREATE_VULKAN_FDM_BOOLEAN, true);
    SDL_SetFloatProperty(info.props, SDL_PROP_GPU_TEXTURE_CREATE_VULKAN_FDM_CENTER_X_FLOAT, cx);
    SDL_SetFloatProperty(info.props, SDL_PROP_GPU_TEXTURE_CREATE_VULKAN_FDM_CENTER_Y_FLOAT, cy);
    SDL_SetFloatProperty(info.props, SDL_PROP_GPU_TEXTURE_CREATE_VULKAN_FDM_INNER_RADIUS_FLOAT, level->inner);
    SDL_SetFloatProperty(info.props, SDL_PROP_GPU_TEXTURE_CREATE_VULKAN_FDM_OUTER_RADIUS_FLOAT, level->outer);
    SDL_SetFloatProperty(info.props, SDL_PROP_GPU_TEXTURE_CREATE_VULKAN_FDM_MIN_DENSITY_FLOAT, level->minDensity);
    fdm = SDL_CreateGPUTexture(s_device, &info);
    SDL_DestroyProperties(info.props);
    if (fdm == NULL)
        fprintf(stderr, "rage-port: XR foveation %s unavailable: %s\n",
                level->name, SDL_GetError());
    else
        fprintf(stderr, "rage-port: XR foveation %s centre %.3f,%.3f\n",
                level->name, cx, cy);
    return fdm;
}

/* XR set-up timing: one line for a step that held the main thread for 20 ms
 * or more (a dropped headset frame or two). Each step runs once per build,
 * so this never repeats per frame. */
static double ModernXrMsSince(Uint64 start) {
    return (double)(SDL_GetPerformanceCounter() - start) * 1000.0 /
           (double)SDL_GetPerformanceFrequency();
}

static void ModernXrStepTime(const char *step, Uint64 start) {
    double ms = ModernXrMsSince(start);
    if (ms >= 20.0)
        fprintf(stderr, "rage-port: XR setup %s took %.1f ms\n", step, ms);
}

static int ModernEnsureEyeTargets(unsigned w, unsigned h,
                                  const PsyzXrView *views) {
    int eye;
    Uint64 stepStart, buildStart;
    static const ModernFoveationLevel *foveationBuilt;
    static int foveateFillState = -1, msaaBuilt = -1;
    /* The foveation level (debug.rr.fov override included), MSAA on/off and
     * sample count: the VR graphics menu changes them between races, the
     * debug override at any time. Either way the rebuild happens here,
     * before this frame's passes are recorded; SDL defers destroying what
     * is released until the frames that used it have finished. */
    const ModernFoveationLevel *foveation = ModernFoveation();
    int msaaWanted = ModernXrMsaaWanted();
    /* debug.rr.fovfill 0 leaves the crack-fill pass at full rate. */
    int foveateFill = XrDebugProp("fovfill", 1) != 0;
    if (s_eyeTarget[0] != NULL && s_eyeW == w && s_eyeH == h &&
        foveation == foveationBuilt && msaaWanted == msaaBuilt &&
        foveateFill == foveateFillState)
        return 1;
    foveationBuilt = foveation;
    msaaBuilt = msaaWanted;
    foveateFillState = foveateFill;
    buildStart = stepStart = SDL_GetPerformanceCounter();
    /* xr.msaa renders eyes multisampled (xr.msaa_samples); a sample count
     * change rebuilds the MSAA pipelines inside EyeMsaaReady. */
    s_eyeMsaaOn = msaaWanted != 0 && ModernNativeGpuEyeMsaaReady();
    ModernXrStepTime("MSAA pipelines", stepStart);
    stepStart = SDL_GetPerformanceCounter();
    /* The crack fill reads depth without MSAA and resolved alpha coverage
     * with it: a different shader, so rebuild it when MSAA flips. */
    if (s_pipeCrackFill != NULL && s_pipeCrackFillMsaa != s_eyeMsaaOn) {
        SDL_ReleaseGPUGraphicsPipeline(s_device, s_pipeCrackFill);
        s_pipeCrackFill = NULL;
    }
    if (s_pipeCrackFill == NULL) {
        /* The fullscreen helper wants MSL too; XR stereo only ever runs on
         * Vulkan, so the post shader's MSL stands in unused. */
        /* xr.crack_debug paints every filled pixel magenta. With MSAA the
         * fill reads resolved alpha coverage instead of depth. */
        int debug = RuntimeConfigEnabled("xr.crack_debug");
        const unsigned char *spv;
        size_t spvLen;
        if (s_eyeMsaaOn) {
            spv = debug ? crack_fill_alpha_debug_frag_spv : crack_fill_alpha_frag_spv;
            spvLen = debug ? crack_fill_alpha_debug_frag_spv_len : crack_fill_alpha_frag_spv_len;
        } else {
            spv = debug ? crack_fill_debug_frag_spv : crack_fill_frag_spv;
            spvLen = debug ? crack_fill_debug_frag_spv_len : crack_fill_frag_spv_len;
        }
        s_fragmentUniformBuffers = 1;
        s_pipeCrackFill = ModernCreateFullscreenPipeline(
            post_vert_spv, post_vert_spv_len,
            (const char *)post_vert_msl, post_vert_msl_len, "vs_post",
            spv, spvLen,
            (const char *)post_frag_msl, post_frag_msl_len, "fs_post", 2);
        s_fragmentUniformBuffers = 0;
        s_pipeCrackFillMsaa = s_eyeMsaaOn;
        if (s_pipeCrackFill == NULL)
            fprintf(stderr, "rage-port: XR crack-fill pipeline failed: %s\n",
                    SDL_GetError());
    }
    ModernXrStepTime("crack-fill pipeline", stepStart);
    stepStart = SDL_GetPerformanceCounter();
    for (eye = 0; eye < 2; eye++) {
        SDL_GPUTextureCreateInfo info = {0};
        if (s_eyeTarget[eye]) SDL_ReleaseGPUTexture(s_device, s_eyeTarget[eye]);
        if (s_eyeDepth[eye]) SDL_ReleaseGPUTexture(s_device, s_eyeDepth[eye]);
        if (s_eyeFinal[eye]) SDL_ReleaseGPUTexture(s_device, s_eyeFinal[eye]);
        if (s_eyeMsaa[eye]) SDL_ReleaseGPUTexture(s_device, s_eyeMsaa[eye]);
        if (s_eyeMsaaDepth[eye])
            SDL_ReleaseGPUTexture(s_device, s_eyeMsaaDepth[eye]);
        if (s_eyeFdm[eye]) SDL_ReleaseGPUTexture(s_device, s_eyeFdm[eye]);
        s_eyeTarget[eye] = s_eyeDepth[eye] = s_eyeFinal[eye] = NULL;
        s_eyeMsaa[eye] = s_eyeMsaaDepth[eye] = NULL;
        s_eyeFdm[eye] = ModernCreateEyeFdm(w, h, &views[eye]);
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                     SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = w;
        info.height = h;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        /* The crack fill's output and the scene target (the MSAA one when
         * multisampling) both carry the density map. */
        if (s_eyeFdm[eye] != NULL && foveateFill) {
            info.props = SDL_CreateProperties();
            SDL_SetPointerProperty(info.props, SDL_PROP_GPU_TEXTURE_CREATE_VULKAN_FDM_ATTACH_POINTER, s_eyeFdm[eye]);
        }
        s_eyeFinal[eye] = SDL_CreateGPUTexture(s_device, &info);
        if (info.props != 0) {
            SDL_DestroyProperties(info.props);
            info.props = 0;
        }
        if (s_eyeFdm[eye] != NULL && !s_eyeMsaaOn) {
            info.props = SDL_CreateProperties();
            SDL_SetPointerProperty(info.props, SDL_PROP_GPU_TEXTURE_CREATE_VULKAN_FDM_ATTACH_POINTER, s_eyeFdm[eye]);
        }
        s_eyeTarget[eye] = SDL_CreateGPUTexture(s_device, &info);
        if (info.props != 0) {
            SDL_DestroyProperties(info.props);
            info.props = 0;
        }
        info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        /* Sampled by the crack fill to tell sky from geometry. */
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
                     SDL_GPU_TEXTUREUSAGE_SAMPLER;
        s_eyeDepth[eye] = SDL_CreateGPUTexture(s_device, &info);
        if (s_eyeMsaaOn) {
            /* Tile-memory only on a tiled GPU: never stored or sampled. */
            info.sample_count = ModernNativeGpuEyeSampleCount();
            info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
            s_eyeMsaaDepth[eye] = SDL_CreateGPUTexture(s_device, &info);
            info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
            if (s_eyeFdm[eye] != NULL) {
                info.props = SDL_CreateProperties();
                SDL_SetPointerProperty(info.props, SDL_PROP_GPU_TEXTURE_CREATE_VULKAN_FDM_ATTACH_POINTER, s_eyeFdm[eye]);
            }
            s_eyeMsaa[eye] = SDL_CreateGPUTexture(s_device, &info);
            if (info.props != 0) {
                SDL_DestroyProperties(info.props);
                info.props = 0;
            }
            if (s_eyeMsaa[eye] == NULL || s_eyeMsaaDepth[eye] == NULL) {
                fprintf(stderr, "rage-port: XR MSAA targets failed: %s\n",
                        SDL_GetError());
                s_eyeW = s_eyeH = 0;
                return 0;
            }
        }
        if (s_eyeTarget[eye] == NULL || s_eyeDepth[eye] == NULL ||
            s_eyeFinal[eye] == NULL) {
            fprintf(stderr, "rage-port: XR eye targets %ux%u failed: %s\n",
                    w, h, SDL_GetError());
            s_eyeW = s_eyeH = 0;
            return 0;
        }
    }
    s_eyeW = w;
    s_eyeH = h;
    ModernXrStepTime("eye textures + density maps", stepStart);
    fprintf(stderr, "rage-port: XR stereo eye targets %ux%u msaa=%s "
            "(built in %.1f ms)\n", w, h,
            !s_eyeMsaaOn ? "off"
            : ModernNativeGpuEyeSampleCount() == SDL_GPU_SAMPLECOUNT_2 ? "2x"
            : ModernNativeGpuEyeSampleCount() == SDL_GPU_SAMPLECOUNT_4 ? "4x"
                                                                      : "8x",
            ModernXrMsSince(buildStart));
    return 1;
}

#if RAGE_PC_XR
/* PC VR: build everything the first stereo frame would otherwise create on
 * the spot (modern resources and native GPU pipelines, eye swapchains, eye
 * targets, MSAA and crack-fill pipelines, the HUD texture and swapchain)
 * while the headset shows the flat menu screen. On an integrated GPU that
 * first frame held the main thread long enough to break up the race-start
 * audio. Nothing is drawn or submitted here; a settings change from the VR
 * graphics menu is rebuilt on the next menu frame the same way. Cheap once
 * built: every step returns at once when nothing changed. */
static void ModernXrPrewarm(void) {
    static int logged;
    PsyzXrView views[2];
    unsigned w = 0, h = 0;
    Uint64 start;
    if (!s_enabled || !Psyz_XrPresenting()) return;
    start = SDL_GetPerformanceCounter();
    if (!ModernEnsureResources()) return;
    ModernXrStepTime("modern resources", start);
    if (!Psyz_XrGetViews(views, &w, &h) || !ModernEnsureEyeTargets(w, h, views))
        return;
    if (!ModernEnsureHudTarget()) return;
    if (!Psyz_XrPrepareStereo((unsigned)s_targetW, (unsigned)s_targetH))
        return;
    if (!logged) {
        logged = 1;
        fprintf(stderr, "rage-port: XR stereo pre-warmed on the menu screen "
                "(%.1f ms)\n", ModernXrMsSince(start));
    }
}
#endif

#if RAGE_PC_XR
/* Stereo start-up upload (see ModernPresentXrStereo). */
static int s_xrStereoRunning, s_xrWarmActive, s_xrTimeNextFrame;
static unsigned s_xrWarmFrames, s_xrWarmLoaded;
static Uint64 s_xrWarmStart;
#endif

static unsigned s_xrFpsWindowStart, s_xrFpsFrames;
static int s_xrMeasuredFps;

static void ModernXrCountFrame(void) {
    unsigned now = (unsigned)SDL_GetTicks();
    unsigned elapsed;
    if (s_xrFpsWindowStart == 0) {
        s_xrFpsWindowStart = now ? now : 1;
        s_xrFpsFrames = 0;
        return;
    }
    if (now - s_xrFpsWindowStart > 2500) { /* frames stopped: start over */
        s_xrFpsWindowStart = now;
        s_xrFpsFrames = 0;
        s_xrMeasuredFps = 0;
        return;
    }
    s_xrFpsFrames++;
    elapsed = now - s_xrFpsWindowStart;
    if (elapsed >= 1000) {
        s_xrMeasuredFps = (int)((s_xrFpsFrames * 1000u + elapsed / 2) / elapsed);
        s_xrFpsWindowStart = now;
        s_xrFpsFrames = 0;
    }
}

int ModernXrMeasuredFps(void) { return s_xrMeasuredFps; }

/* XR: draws the race in stereo from the in-car camera, moved and turned by
 * the player's head. Returns 0 to fall back to the flat-screen image (menus,
 * chase and replay cameras, or no XR frame). */
static int ModernPresentXrStereo(const RageSceneSnapshot *snapshot,
                                 int fpsMode,
                                 PsyzPresentSourceInfo *info) {
    static int stereoEnabled = -1;
    static int fullVisibility;
    static int terrainFacing;
    static int hudFades;
    static int stereoChase;
    static int hudEnabled;
    SDL_GPUTexture *vram = NULL;
    int hud = 0;
    static float worldScale;
    static int loggedScale;
    PsyzXrView views[2];
    ModernNativeEyeView eyes[2];
    unsigned w = 0, h = 0;
    const RenderWorld *world;
    SDL_GPUCommandBuffer *cmd;
    int eye;

    if (stereoEnabled < 0) {
        stereoEnabled = RuntimeConfigGet("xr.stereo") == NULL ||
                        RuntimeConfigEnabled("xr.stereo");
        /* World units per metre of head movement. */
        worldScale = (float)RuntimeConfigInt("xr.world_scale", 25, 1, 100000);
        /* Off by default: the authored masks depend on camera position, not
         * heading, and drawing every cell exposes far sections sliced by the
         * draw distance as debris in the sky. */
        fullVisibility = RuntimeConfigEnabled("xr.full_visibility");
        terrainFacing = RuntimeConfigGet("xr.terrain_facing") == NULL ||
                        RuntimeConfigEnabled("xr.terrain_facing");
        hudFades = RuntimeConfigEnabled("xr.hud_fades");
        stereoChase = RuntimeConfigGet("xr.stereo_chase") == NULL ||
                      RuntimeConfigEnabled("xr.stereo_chase");
        hudEnabled = RuntimeConfigGet("xr.hud") == NULL ||
                     RuntimeConfigEnabled("xr.hud");
    }
    /* The player's other race view (behind the car) is the in-car mode with
     * its low bit flipped; it stays in stereo too unless
     * xr.stereo_chase = false. Scripted and replay cameras remain flat. */
    if (!stereoEnabled || !s_enabled ||
        !(g_Camera.mode == CAMERA_VIEW_CAR ||
          (stereoChase && snapshot->sceneId == GAME_SCENE_RACE &&
           g_Camera.mode == (CAMERA_VIEW_CAR ^ 1))) ||
        !Psyz_XrGetViews(views, &w, &h) ||
        !ModernEnsureEyeTargets(w, h, views)) {
        RenderSetOmnidirectionalCulling(0);
        RenderSetOmnidirectionalFacing(0);
        GameRenderWorldSetFullVisibility(0);
#if RAGE_PC_XR
        s_xrStereoRunning = 0;
        if (s_xrWarmActive) {
            s_xrWarmActive = 0;
            Psyz_XrSetBlank(0);
        }
#endif
        return 0;
    }
#if RAGE_PC_XR
    if (!s_xrStereoRunning) {
        /* Stereo (re)starts: the race start, or back from a flat camera.
         * Upload what the first frame needs before drawing it. */
        s_xrStereoRunning = 1;
        s_xrWarmActive = 1;
        s_xrWarmFrames = 0;
        s_xrWarmLoaded = 0;
        s_xrWarmStart = SDL_GetPerformanceCounter();
        /* Black while uploading, not a stale flat image. */
        Psyz_XrSetBlank(1);
    }
#endif
    /* Takes effect from the next published world (one logic frame). */
    GameRenderWorldSetFullVisibility(fullVisibility);
    if (!loggedScale) {
        /* Calibration aid: the in-car camera's height above the car origin
         * in world units; a seated driver's eyes are about 1.1 m up. */
        fprintf(stderr,
                "rage-port: XR stereo active world_scale=%.1f "
                "camera_above_car=%d\n",
                worldScale, (int)(g_PlayerCar.y - g_Camera.view.y));
        loggedScale = 1;
    }
    for (eye = 0; eye < 2; eye++) {
        memcpy(eyes[eye].position, views[eye].position, sizeof(eyes[eye].position));
        memcpy(eyes[eye].orientation, views[eye].orientation,
               sizeof(eyes[eye].orientation));
        eyes[eye].tanLeft = views[eye].tan_left;
        eyes[eye].tanRight = views[eye].tan_right;
        eyes[eye].tanUp = views[eye].tan_up;
        eyes[eye].tanDown = views[eye].tan_down;
    }
    /* Every XR frame needs a fresh image for the new head pose, so render
     * regardless of the flat presentation clock, interpolating the cars. */
    if (fpsMode) {
        const RageSceneSnapshot *target = CaptureCurrent();
        Uint64 now = SDL_GetTicksNS();
        float fraction;
        ModernPresentationClockObserveFrame(&s_presentationClock,
                                            target->frameCounter, now);
        fraction = ModernPresentationClockFraction(&s_presentationClock, now);
#if RAGE_XR_BUILD
        /* The observed clock times each tick from whenever an eye frame
         * first sees it and takes the last gap as the tick length; with
         * uneven XR frames that swings every tick and near scenery judders.
         * The VBlank schedule says exactly where in the tick we are (game
         * speed included). adb shell setprop debug.rr.clock 0 = old clock. */
        if (XrDebugProp("clock", 1)) {
            double tickUs = Psyz_VideoVBlankMicroseconds() *
                            (double)((g_FrameSyncThreshold + 0xFF) / 0x100);
            double into = Psyz_VideoMicrosecondsSinceVSync();
            if (tickUs > 0.0) {
                fraction = (float)(into / tickUs);
                if (fraction < 0.0f) fraction = 0.0f;
                if (fraction > 1.0f) fraction = 1.0f;
            }
        }
#endif
        world = GameRenderWorldPresentation(fraction);
    } else {
        world = GameRenderWorldPrevious();
        if (world == NULL) world = GameRenderWorldCurrent();
    }
    /* The pause menu and the end-of-race captions are drawn on the HUD
     * panel: show it then even if the player has switched the HUD off. */
    Psyz_XrForceHud(g_RacePaused != 0 || g_RacePhase != RACE_PHASE_ACTIVE);
    RenderSetOmnidirectionalCulling(1);
    RenderSetOmnidirectionalFacing(terrainFacing);
    ModernNativeGpuPrepare(ModernSkyOnlyWorld(world), (float)w / (float)h);
#if RAGE_PC_XR
    if (s_xrWarmActive) {
        /* PC VR: the first race frame used to load every visible texture
         * and mesh buffer at once (~230 ms on an integrated GPU), stopping
         * the XR frame stream at the countdown; a streaming runtime's
         * audio broke up with it. Upload them a few ms per XR frame
         * instead, repeating the last image meanwhile (the countdown keeps
         * running underneath; at most ~1.3 s). */
        unsigned loaded = 0;
        int pending;
        cmd = SDL_AcquireGPUCommandBuffer(s_device);
        if (cmd == NULL) return 0;
        pending = ModernNativeGpuWarmStep(cmd, 6000000u, &loaded);
        if (!SDL_SubmitGPUCommandBuffer(cmd)) pending = 0;
        ModernNativeGpuSubmitted();
        s_xrWarmLoaded += loaded;
        s_xrWarmFrames++;
        if (pending && s_xrWarmFrames < 96) {
            info->skip_present = true;
            return 1;
        }
        s_xrWarmActive = 0;
        Psyz_XrSetBlank(0);
        if (s_xrWarmLoaded != 0 || s_xrWarmFrames > 1)
            fprintf(stderr, "rage-port: XR stereo warm-up uploaded %u "
                    "textures over %u frames (%.1f ms)%s\n", s_xrWarmLoaded,
                    s_xrWarmFrames, ModernXrMsSince(s_xrWarmStart),
                    pending ? ", gave up waiting" : "");
        s_xrTimeNextFrame = 1;
    }
    Uint64 xrStepHud = SDL_GetPerformanceCounter();
#endif
    /* The game's 2D HUD (speed, laps, times) is drawn on its own into a
     * transparent texture and shown as a floating panel by psyz. The VRAM
     * snapshot may submit work of its own, so take it before recording. */
    if (hudEnabled) {
        vram = ModernVramSnapshotForFrame(
            &s_sampledVram, snapshot->frameCounter,
            ModernNativeGpuTextureRevision(), ModernAssetsGeneration(),
            ModernCaptureVramSnapshot, NULL);
        if (vram != NULL && ModernEnsureHudTarget()) {
            /* xr.hud_fades = true keeps full-screen fades on the panel. */
            s_xrHudBuild = !hudFades;
            ModernBuildOverlayFrame(snapshot);
            s_xrHudBuild = 0;
            hud = 1;
        }
    }
#if RAGE_PC_XR
    Uint64 xrStepDraw = SDL_GetPerformanceCounter();
#endif
    cmd = SDL_AcquireGPUCommandBuffer(s_device);
    if (cmd == NULL) return 0;
    if (hud && !ModernUploadOverlay(cmd)) hud = 0;
    if (s_eyeMsaaOn)
        ModernNativeGpuDrawStereo(cmd, s_eyeMsaa, s_eyeMsaaDepth, s_eyeTarget,
                                  eyes, worldScale, (int)h);
    else
        ModernNativeGpuDrawStereo(cmd, s_eyeTarget, s_eyeDepth, NULL, eyes,
                                  worldScale, (int)h);
    /* Development aids: debug.rr.crackfill 0 presents the eye targets
     * without the crack-fill/grade pass; debug.rr.grade 0 keeps the pass
     * but neutral (no saturation/contrast/gamma/sharpen). */
    s_xrCrackFillUsed = s_pipeCrackFill != NULL &&
                        XrDebugProp("crackfill", 1) != 0;
    if (s_xrCrackFillUsed) {
        /* [xr] saturation, contrast and gamma grade the headset image in
         * the same pass (1.0 each leaves it as rendered). Saturation and
         * contrast are the VR graphics menu's COLOUR row: re-read whenever
         * the settings generation changes. */
        static float grade[4];
        static int gradeLoaded;
        static unsigned gradeGeneration;
        unsigned generation = GraphicsSettingsGeneration();
        if (!gradeLoaded || gradeGeneration != generation) {
            const char *sharpen = XrProfileSetting("xr.sharpen");
            gradeLoaded = 1;
            gradeGeneration = generation;
            grade[0] = ModernXrGradeSetting(XrProfileSetting("xr.saturation"),
                                            0.0f, 2.5f);
            grade[1] = ModernXrGradeSetting(XrProfileSetting("xr.contrast"),
                                            0.5f, 2.0f);
            grade[2] = ModernXrGradeSetting(RuntimeConfigGet("xr.gamma"),
                                            0.5f, 2.0f);
            /* [xr] sharpen (the menu's SHARPEN): 0 (off, the default)
             * to 1. */
            grade[3] = sharpen != NULL ? strtof(sharpen, NULL) : 0.0f;
            if (!(grade[3] >= 0.0f && grade[3] <= 1.0f)) grade[3] = 0.0f;
            fprintf(stderr,
                    "rage-port: XR colour saturation=%.2f contrast=%.2f "
                    "gamma=%.2f sharpen=%.2f\n",
                    grade[0], grade[1], grade[2], grade[3]);
        }
        if (XrDebugProp("grade", 1) != 0) {
            SDL_PushGPUFragmentUniformData(cmd, 0, grade, sizeof(grade));
        } else {
            static const float neutral[4] = {1.0f, 1.0f, 1.0f, 0.0f};
            SDL_PushGPUFragmentUniformData(cmd, 0, neutral, sizeof(neutral));
        }
        for (eye = 0; eye < 2; eye++) {
            /* Nearest sampling throughout: D32_FLOAT is commonly not
             * linearly filterable on mobile GPUs, and sampling it through
             * a linear sampler is invalid (the depth reads come back
             * wrong, hiding every crack). */
            const SDL_GPUColorTargetInfo color = {
                .texture = s_eyeFinal[eye],
                .load_op = SDL_GPU_LOADOP_DONT_CARE,
                .store_op = SDL_GPU_STOREOP_STORE,
            };
            /* With MSAA the fill reads coverage from alpha; the depth slot
             * is bound but unused (its contents were never stored). */
            const SDL_GPUTextureSamplerBinding bindings[2] = {
                {.texture = s_eyeTarget[eye], .sampler = s_sampler},
                {.texture = s_eyeMsaaOn ? s_eyeTarget[eye] : s_eyeDepth[eye],
                 .sampler = s_sampler},
            };
            SDL_GPURenderPass *pass =
                SDL_BeginGPURenderPass(cmd, &color, 1, NULL);
            if (pass == NULL) continue;
            SDL_BindGPUGraphicsPipeline(pass, s_pipeCrackFill);
            SDL_BindGPUFragmentSamplers(pass, 0, bindings, 2);
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(pass);
        }
    }
    if (hud) {
        s_overlayTarget = s_hudTarget;
        ModernRenderOverlaySelection(cmd, vram, 0, 1u << MODERN_LAYER_HUD, 1);
        /* The rear-view mirror: its frame is part of the HUD, the reflected
         * scene a separate small render composited into it, as in the
         * flat view. */
        /* MIRROR OFF hides it whatever the game's own mirror toggle says
         * (that toggle can still hide it when on). debug.rr.mirror and
         * debug.rr.mirrorrate override the setting when set: -1, their
         * unset value, means "use the setting". */
        const int mirrorRate = ModernXrMirrorRate();
        const int mirrorProp = XrDebugProp("mirror", -1);
        const int mirrorShown =
            mirrorProp >= 0 ? mirrorProp != 0 : mirrorRate != 0;
        if (ModernNativeGpuHasMirrorDraws() && mirrorShown) {
            /* The mirror re-bins the whole scene for a 592x144 view: ~1.5 ms
             * of a Quest 3 frame in GPU traces. Re-render it every Nth XR
             * frame (MIRROR HALF RATE, the default: 2 = 36 Hz at 72; FULL
             * 1) and composite the last image in between. */
            static unsigned mirrorFrame;
            int rate = XrDebugProp("mirrorrate", -1);
            if (rate < 0) rate = mirrorRate > 0 ? mirrorRate : 2;
            if (rate < 1) rate = 1;
            if (mirrorFrame++ % (unsigned)rate == 0)
                ModernNativeGpuDrawMirror(cmd, s_mirrorTarget, s_mirrorDepth,
                                          s_mirrorTargetH);
            ModernCompositeNativeMirror(cmd);
            ModernRenderOverlaySelection(
                cmd, vram, 1, 1u << MODERN_LAYER_MIRROR_FOREGROUND, 0);
        }
        s_overlayTarget = NULL;
    }
    if (XrDebugProp("stats", 0)) {
        /* adb shell setprop debug.rr.stats 1: one line a second or so. */
        static unsigned frames;
        const RenderWorld *prepared = ModernNativeGpuPreparedWorld();
        if (prepared != NULL && ++frames % 120 == 0)
            fprintf(stderr, "rage-port: xr-stats instances=%u spots=%u "
                    "mirror=%d allrivals=%d\n", prepared->instanceCount,
                    prepared->spotLightCount, prepared->mirrorActive,
                    XrDebugProp("allrivals", 1));
    }
    if (!SDL_SubmitGPUCommandBuffer(cmd)) {
        fprintf(stderr, "rage-port: XR stereo submit failed: %s\n",
                SDL_GetError());
        return 0;
    }
    ModernNativeGpuSubmitted();
#if RAGE_PC_XR
    if (s_xrTimeNextFrame) {
        /* Where the first drawn stereo frame's time went. */
        s_xrTimeNextFrame = 0;
        fprintf(stderr, "rage-port: XR first stereo frame: HUD %.1f ms, "
                "draw+submit %.1f ms\n",
                (double)(xrStepDraw - xrStepHud) * 1000.0 /
                    (double)SDL_GetPerformanceFrequency(),
                ModernXrMsSince(xrStepDraw));
    }
#endif
    ModernXrCountFrame();
    info->texture = NULL;
    info->hud = hud ? s_hudTarget : NULL;
    info->hud_w = (Uint32)s_targetW;
    info->hud_h = (Uint32)s_targetH;
    info->stereo[0] = s_xrCrackFillUsed ? s_eyeFinal[0] : s_eyeTarget[0];
    info->stereo[1] = s_xrCrackFillUsed ? s_eyeFinal[1] : s_eyeTarget[1];
    info->stereo_w = w;
    info->stereo_h = h;
    return 1;
}

static void ModernPresentSource(PsyzPresentSourceInfo *info) {
    const RageSceneSnapshot *snapshot;
    int fpsMode;
    const bool *keys;
    int toggleDown;
    if (s_prev_present_source) {
        s_prev_present_source(info);
    }
    /* Opt-in lifecycle exercise at the host callback boundary, after PSY-Z
     * has submitted pending work and before it acquires a swapchain command. */
    {
        static int restartCount = -1, restarted;
        static uint32_t restartFrame;
        if (restartCount < 0) {
            restartCount = RuntimeConfigInt("diagnostics.renderer_restart_count", 0, 0, 10);
            restartFrame = (uint32_t)RuntimeConfigInt(
                "diagnostics.renderer_restart_frame", 500, 1, 1000000);
        }
        if (restarted < restartCount && s_haveRenderedFrame &&
            CaptureCurrent()->frameCounter >= restartFrame) {
            const RenderWorld *world = ModernNativeGpuPreparedWorld();
            RenderMeshInstance probe = {0};
            const RageRuntimeMesh *before = NULL;
            if (world != NULL && world->instanceCount != 0) {
                probe = world->instances[0];
                before = ModernAssetsMeshLookup(NULL, &probe);
            }
            uint32_t meshes = ModernAssetsCachedMeshCount();
            int ok = ModernRestartPresentation(&s_config);
            int retained = before != NULL &&
                ModernAssetsMeshLookup(NULL, &probe) == before &&
                ModernAssetsCachedMeshCount() == meshes;
            ok = ok && retained && s_device != NULL;
            fprintf(stderr,
                "rage-port: modern presentation restart index=%d result=%s meshes=%u retained=%d device=%d\n",
                ++restarted, ok ? "ok" : "failed", meshes, retained, s_device != NULL);
            restartFrame += 60;
            if (!ok) { info->skip_present = true; return; }
        }
    }
    keys = SDL_GetKeyboardState(NULL);
    toggleDown = keys != NULL && keys[s_toggleScancode];
    if (toggleDown && !s_toggleWasDown) ModernToggle();
    s_toggleWasDown = toggleDown;
    /* The mode changes between two complete logic frames. Present the last
     * complete image once while the newly selected renderer builds its first
     * frame; otherwise the two capture paths can be visible in one swap. */
    if (s_waitClassicFrame && s_haveRenderedFrame &&
        CaptureClassicSources(CapturePrevious()) == NULL) {
        info->texture = ModernPresentTexture();
        info->w = (Uint32)s_targetW;
        info->h = (Uint32)s_targetH;
        info->aspect = (4.0f / 3.0f) * (s_logicalW / 320.0f);
        info->filter = s_config.modernTextureFilterLinear
                           ? SDL_GPU_FILTER_LINEAR
                           : SDL_GPU_FILTER_NEAREST;
        return;
    }
    s_waitClassicFrame = 0;
    if (s_markerCaptureEnabled) {
        /* M writes what the modern renderer is showing, with the state that
         * produced it, so a player who can see something wrong can hand over
         * the picture and the place it happened. */
        extern int g_SceneId;
        extern s32 g_SceneTimer;
        static int markWasDown, marks;
        int markDown = keys != NULL && keys[SDL_SCANCODE_M];
        if (markDown && !markWasDown) {
            char directory[1024], path[1200];
            if (!PlatformUserConfigDirectory(directory, sizeof(directory)))
                snprintf(directory, sizeof(directory), ".");
            snprintf(path, sizeof(path), "%s/mark-%02d.ppm", directory, ++marks);
            fprintf(stderr,
                    "rage-port: mark %d scene=%d timer=%d course=%d mirror=%d "
                    "sky_row=%d "
                    "point=%d pos=%d,%d heading=%d camera=%d,%d,%d angle=%d view=%d\n",
                    marks, g_SceneId, g_SceneTimer, g_CourseIndex, g_MirrorMode,
                    g_SkyRowBase,
                    g_PlayerCar.trackPointIndex, g_PlayerCar.x, g_PlayerCar.z,
                    g_PlayerCar.headingAngle, g_Camera.view.x, g_Camera.view.y,
                    g_Camera.view.z, g_Camera.view.angleY, g_Camera.mode);
            if (ModernCaptureFrame(path))
                fprintf(stderr, "rage-port: mark %d written to %s\n", marks, path);
            else
                fprintf(stderr, "rage-port: mark %d could not be written\n", marks);
        }
        markWasDown = markDown;
    }
    if (!ModernPresentationActive() || s_device == NULL) return;
    info->sync_to_display = s_config.modernFps == RAGE_MODERN_FPS_VSYNC;
    int occluded = ModernWindowOccluded();
    if (s_profileTiming) {
        static int previousOccluded = -1;
        if (occluded != previousOccluded) {
            fprintf(stderr, "presentation-window frame=%u occluded=%d\n",
                    CaptureCurrent()->frameCounter, occluded);
            previousOccluded = occluded;
        }
    }
    /* Metal's nominally nonblocking swapchain acquire can still wait in
     * nextDrawable for an occluded window. Keep simulation/input ticking;
     * no display work is useful until the window is visible again. Explicit
     * offline screenshot diagnostics may still render, but never swap. */
    s_presentOffscreen = occluded ||
        (RuntimeConfigGet("diagnostics.modern_dump") != NULL &&
         RuntimeConfigEnabled("diagnostics.modern_dump_offscreen"));
    if (s_presentOffscreen)
        info->skip_present = true;
    /* Skipping swaps also removes the compatibility renderer's queue
     * backpressure. Bound its work even in title/480-line scenes, which
     * return below before the native offscreen submission fence is used. */
    if (s_presentOffscreen) SDL_WaitForGPUIdle(s_device);
    if (occluded && RuntimeConfigGet("diagnostics.modern_dump") == NULL) {
        /* The compat GPU still submits each logic frame while hidden. */
        return;
    }
    /* Both modes render the PREVIOUS logic frame - the one compat is
     * presenting during this tick; fps mode moves its transforms toward
     * the newest frame by the wall-clock fraction of the tick. */
    snapshot = CapturePrevious();
    fpsMode = ModernShouldInterpolate(snapshot);
    /* Scenes with no captured 3D pass through to the compat image, and so
     * do 480-line menu scenes: their double-height buffer follows PS1
     * interlace conventions the compat presenter already handles. */
    /* A scene without a race world (the LOST RACE prompt, for one) can still
     * carry the previous race's captured faces for a frame or two; rendering
     * that through an emptied world drew a black sky and reprojected the
     * old sky through a dead camera (#32). Present the compat image instead. */
    if (snapshot->faceCount == 0 ||
        !GameRenderWorldSceneHas3d((GameSceneId)snapshot->sceneId)) {
        if (s_markerCaptureEnabled) ModernMarkerCheck(snapshot, 0);
        ModernPresentationClockReset(&s_presentationClock);
        /* Menus and other 2D-only scenes use the compatibility framebuffer,
         * but presentation should still honour the modern texture-filter
         * setting. Filtering the completed framebuffer cannot bleed between
         * atlas entries and avoids exposing native texel stair-steps as
         * apparent splits through large glyphs. */
        if (s_config.modernTextureFilterLinear) {
            info->filter = SDL_GPU_FILTER_LINEAR;
        }
#if RAGE_PC_XR
        ModernXrPrewarm();
#endif
        ModernFmvZoomPresent(s_device, s_window, info);
        return;
    }
    if (snapshot->displayHeight != 0 && snapshot->displayHeight != 240) {
        if (s_markerCaptureEnabled) ModernMarkerCheck(snapshot, 0);
        ModernPresentationClockReset(&s_presentationClock);
#if RAGE_PC_XR
        ModernXrPrewarm();
#endif
        return;
    }
    /* A renderer toggle can expose the last modern-only capture. Wait for a
     * complete classic stream instead of displaying its HUD without geometry. */
    if (!s_enabled && CaptureClassicSources(snapshot) == NULL) return;
    if (!ModernEnsureResources()) { info->skip_present = true; return; }
#if RAGE_PC_XR
    {
        /* A stereo frame that held the main thread for 25 ms or more: one
         * line, at most once a second, with how far into stereo it was. */
        static unsigned stereoFrames;
        static Uint64 lastReport;
        Uint64 start = SDL_GetPerformanceCounter();
        if (ModernPresentXrStereo(snapshot, fpsMode, info)) {
            double ms = ModernXrMsSince(start);
            Uint64 now = SDL_GetPerformanceCounter();
            stereoFrames++;
            if (ms >= 25.0 &&
                (lastReport == 0 ||
                 now - lastReport >= SDL_GetPerformanceFrequency())) {
                fprintf(stderr, "rage-port: XR stereo frame %u took %.1f ms "
                        "on the main thread\n", stereoFrames, ms);
                lastReport = now;
            }
            return;
        }
    }
#else
    if (ModernPresentXrStereo(snapshot, fpsMode, info)) return;
#endif

    if (fpsMode) {
        const RageSceneSnapshot *target = CaptureCurrent();
        Uint64 now = SDL_GetTicksNS();
        Uint64 interval = ModernPresentationInterval();
        float t = 1.0f;
        if (!ModernPresentationClockDue(&s_presentationClock, now, interval)) {
            info->skip_present = true;
            return;
        }
        /* PortAfterSceneHandler timestamps the completed logic frame.
         * Starting interpolation when it is first presented instead made
         * the first repeated frame consume part of the next tick. */
        ModernPresentationClockObserveFrame(&s_presentationClock,
                                            target->frameCounter, now);
        t = ModernPresentationClockFraction(&s_presentationClock, now);
        Uint64 prepareStart = s_profileTiming ? SDL_GetTicksNS() : 0;
        if (s_enabled) {
            ModernNativeGpuPrepare(
                ModernSkyOnlyWorld(GameRenderWorldPresentation(t)),
                (float)s_targetW / (float)s_targetH);
        } else {
            s_classicFraction = t;
            s_classicSkyWorld = GameRenderWorldPresentation(t);
        }
        if (s_profileTiming) s_profilePrepareNs = SDL_GetTicksNS() - prepareStart;
        if (!ModernRender(snapshot)) {
            info->skip_present = true;
            return;
        }
        ModernPresentationClockPresented(&s_presentationClock, now, interval);
        if (s_haveRenderedFrame) ModernMaybeDump(snapshot);
    } else if (snapshot->frameCounter != s_lastRenderedFrame) {
        const RenderWorld *world = GameRenderWorldPrevious();
        if (world == NULL) world = GameRenderWorldCurrent();
        Uint64 prepareStart = s_profileTiming ? SDL_GetTicksNS() : 0;
        if (s_enabled)
            ModernNativeGpuPrepare(ModernSkyOnlyWorld(world),
                                   (float)s_targetW / (float)s_targetH);
        else {
            s_classicFraction = 0.0f;
            s_classicSkyWorld = NULL;
        }
        if (s_profileTiming) s_profilePrepareNs = SDL_GetTicksNS() - prepareStart;
        if (!ModernRender(snapshot)) {
            info->skip_present = true;
            return;
        }
        s_lastRenderedFrame = snapshot->frameCounter;
        if (s_haveRenderedFrame) ModernMaybeDump(snapshot);
    }
    /* Capture after this snapshot's render submission. Before rendering, the
     * image, prepared world and sampled VRAM still belonged to the last frame. */
    if (s_markerCaptureEnabled)
        ModernMarkerCheck(snapshot, s_resourcesReady && s_haveRenderedFrame);
    if (!s_haveRenderedFrame) return;
    info->texture = ModernPresentTexture();
    info->w = (Uint32)s_targetW;
    info->h = (Uint32)s_targetH;
    info->aspect = (4.0f / 3.0f) * (s_logicalW / 320.0f);
    info->filter = s_config.modernTextureFilterLinear
                       ? SDL_GPU_FILTER_LINEAR
                       : SDL_GPU_FILTER_NEAREST;
}

/* Called by the main loop's frame-sync wait. When an FPS mode is selected,
 * present additional interpolated frames while race logic (threshold 0x180,
 * one tick per two VBlanks) waits out its interval; menu-rate scenes already
 * present every VBlank. Calling the platform present directly skips the
 * BIOS pad refresh, so input edge semantics are untouched. */
void ModernFrameWaitTick(int frameLimit) {
    if (!ModernPresentationActive() || s_device == NULL) return;
    /* A short bounded sleep yields the CPU without tying emulated VBlank
     * to the monitor. The caller rechecks its original logic deadline. */
    SDL_DelayNS(100000u);
    if (!ModernShouldInterpolate(CaptureCurrent())) return;
    if (ModernWindowOccluded() &&
        RuntimeConfigGet("diagnostics.modern_dump") == NULL) return;
    if (frameLimit < 0x180) return;
    if (CaptureCurrent()->faceCount == 0) return;
    if (!ModernPresentationClockDue(&s_presentationClock, SDL_GetTicksNS(),
                                    ModernPresentationInterval())) return;
    /* An intermediate frame that ends past the game's tick deadline makes
     * that tick late. Skip it when the recent ones say it would not fit;
     * the caller's VSync(0) presents at the deadline anyway. */
    static double s_intermediateUs;
    /* XR frame slots: when the last intermediate frame returned (its slot)
     * and the measured spacing between slots. */
    static Uint64 s_lastSlotNs;
    static double s_slotUs;
#if RAGE_XR_BUILD
    /* debug.rr.guard: 2 = don't start an XR frame whose display slot would
     * fall after this tick's deadline (such a frame is clamped to the end of
     * the tick, so the next one jumps). 1 = the old duration-based guard,
     * 0 (default) = off. Measured GPU bound in the canyon on a Quest 3: the
     * guard left the GPU 11-14% idle (55 fps vs 63 without), and driving
     * felt smoother without it. */
    {
        int guard = XrDebugProp("guard", 0);
        double vblank = Psyz_VideoVBlankMicroseconds();
        double deadline = vblank * (double)((frameLimit + 0xFF) / 0x100);
        double since = Psyz_VideoMicrosecondsSinceVSync();
        if (guard == 1 && since + s_intermediateUs > deadline) return;
        if (guard == 2 && s_lastSlotNs != 0 && s_slotUs > 0.0) {
            double nextSlotIn =
                s_slotUs - (double)(SDL_GetTicksNS() - s_lastSlotNs) / 1000.0;
            if (nextSlotIn < 0.0) nextSlotIn = 0.0;
            if (since + nextSlotIn > deadline) return;
        }
    }
#endif
    Uint64 intermediateStart = SDL_GetTicksNS();
    Uint64 presentStart = s_profileTiming ? SDL_GetTicksNS() : 0;
    SDL_WindowFlags flagsBefore = s_profileTiming && s_window
        ? SDL_GetWindowFlags(s_window) : 0;
    Psyz_VideoPresentIntermediate();
    {
        /* Rising fast, falling slowly: a slow frame is the one to avoid. */
        double took = (double)(SDL_GetTicksNS() - intermediateStart) / 1000.0;
        s_intermediateUs = took > s_intermediateUs
            ? took : s_intermediateUs * 0.9 + took * 0.1;
    }
    {
        /* XR presents return at their display slot; track the spacing. A
         * gap of several slots (a tick boundary) is not a slot period. */
        Uint64 slot = SDL_GetTicksNS();
        if (s_lastSlotNs != 0) {
            double gap = (double)(slot - s_lastSlotNs) / 1000.0;
            if (gap > 2000.0 &&
                gap < (s_slotUs > 0.0 ? s_slotUs * 1.5 : 20000.0))
                s_slotUs = s_slotUs > 0.0 ? s_slotUs * 0.9 + gap * 0.1 : gap;
        }
        s_lastSlotNs = slot;
    }
    if (s_profileTiming) {
        Uint64 elapsed = SDL_GetTicksNS() - presentStart;
        if (elapsed > 20000000u) {
            PsyzVideoStats stats = {0};
            Psyz_VideoStats(&stats);
            fprintf(stderr, "presentation-stall frame=%u point=%d total_ms=%.3f "
                    "platform_frame_ms=%.3f platform_draw_ms=%.3f "
                    "window_before=%llx window_after=%llx\n",
                    CaptureCurrent()->frameCounter, g_PlayerCar.trackPointIndex,
                    elapsed / 1000000.0, stats.last_frame_time_us / 1000.0,
                    stats.last_draw_time_us / 1000.0,
                    (unsigned long long)flagsBefore,
                    (unsigned long long)(s_window ? SDL_GetWindowFlags(s_window) : 0));
        }
    }
}

int ModernInit(const RagePortConfig *config) {
    const char *toggleKey;
    if (s_initialized) {
        return 1;
    }
    if (config == NULL) return 0;
    if (!s_shutdownRegistered) {
        if (atexit(ModernShutdown) != 0) return 0;
        s_shutdownRegistered = 1;
    }
    s_shutdownNeeded = 1;
    s_config = *config;
    const char *classic = RuntimeConfigGet("video.classic_enhancements");
    s_classicEnhancements = classic == NULL || RuntimeConfigEnabled("video.classic_enhancements");
    s_profileTiming = RuntimeConfigEnabled("diagnostics.performance");
    s_skyOnlyDiagnostic = RuntimeConfigEnabled("diagnostics.sky_only");
    if (config->renderer == RAGE_RENDERER_MODERN && !ModernAssetsInit()) {
        fprintf(stderr,
                "rage-port: refusing to start modern renderer without "
                "native assets\n");
        return 0;
    }
    toggleKey = RuntimeConfigGet("video.toggle_renderer_key");
    if (toggleKey != NULL && toggleKey[0] != '\0') {
        SDL_Scancode parsed = SDL_GetScancodeFromName(toggleKey);
        if (parsed != SDL_SCANCODE_UNKNOWN) s_toggleScancode = parsed;
        else fprintf(stderr, "rage-port: unknown renderer toggle key %s; using F10\n",
                     toggleKey);
    }
    s_prev_overlay_init = Psyz_OverlayInit_SDL3GPU(ModernOverlayInit);
    s_prev_present_source = Psyz_PresentSource_SDL3GPU(ModernPresentSource);
    s_prev_overlay_destroy = Psyz_OverlayDestroyCB(ModernOverlayDestroy);
    /* Late attachment must not wait for a device-init event that already
     * happened, nor reinitialize overlays that were attached before us. */
    Psyz_VideoGetPresentationDevice_SDL3GPU(&s_window, &s_device);
    s_initialized = 1;
    ModernPresentationClockReset(&s_presentationClock);
    s_markerCaptureEnabled = RuntimeConfigEnabled("diagnostics.marker_capture");
    s_enabled = config->renderer == RAGE_RENDERER_MODERN;
    fprintf(stderr, "rage-port: renderer toggle=%s; active=%s\n",
            SDL_GetScancodeName(s_toggleScancode),
            s_enabled ? "modern" : "classic");
    return 1;
}

static void ModernDetachPresentation(void) {
    ModernReleaseDevice();
    if (s_initialized) {
        Psyz_OverlayInit_SDL3GPU(s_prev_overlay_init);
        Psyz_PresentSource_SDL3GPU(s_prev_present_source);
        Psyz_OverlayDestroyCB(s_prev_overlay_destroy);
    }
    s_prev_overlay_init = NULL;
    s_prev_present_source = NULL;
    s_prev_overlay_destroy = NULL;
    s_initialized = s_enabled = s_toggleWasDown = 0;
    ModernPresentationClockReset(&s_presentationClock);
}

int ModernRestartPresentation(const RagePortConfig *config) {
    if (config == NULL || !s_initialized) return 0;
    RagePortConfig next = *config;
    ModernDetachPresentation();
    return ModernInit(&next);
}

void ModernShutdown(void) {
    if (!s_shutdownNeeded) return;
    s_shutdownNeeded = 0;
    ModernDetachPresentation();
    ModernAssetsShutdown();
    if (RuntimeConfigEnabled("diagnostics.renderer_lifecycle"))
        fprintf(stderr, "rage-port: modern session shutdown assets_ready=%d meshes=%u\n",
                ModernAssetsReady(), ModernAssetsCachedMeshCount());
}

int ModernPresentationActive(void) {
    return s_initialized && (s_enabled || s_classicEnhancements);
}

int ModernIsEnabled(void) {
    return s_enabled;
}

void ModernToggle(void) {
    if (!s_initialized) return;
    if (!s_enabled && !ModernAssetsReady() && !ModernAssetsInit()) {
        fprintf(stderr,
                "rage-port: renderer switch to modern refused: native "
                "assets are unavailable\n");
        return;
    }
    if (!s_enabled && s_resourcesReady && !s_nativeGpuReady &&
        !ModernNativeGpuInit(s_device, s_config.modernTextureFilterLinear)) {
        fprintf(stderr,
                "rage-port: renderer switch to modern refused: native GPU "
                "setup failed\n");
        return;
    }
    if (!s_enabled) s_nativeGpuReady = 1;
    s_enabled = !s_enabled;
    /* Classic packet sources only start being captured after this switch and
     * reach CapturePrevious two publications later. Retain the modern image
     * until that complete classic frame exists. */
    s_waitClassicFrame = !s_enabled && s_haveRenderedFrame;
    s_config.renderer = s_enabled ? RAGE_RENDERER_MODERN : RAGE_RENDERER_CLASSIC;
    PortConfigSetActive(&s_config);
    /* Both renderers present through these targets.  Keeping them alive also
     * keeps the last completed image available until the newly selected path
     * has rendered, instead of exposing a cleared allocation for one frame. */
    ModernVramSnapshotReset(&s_sampledVram);
    ClassicMotionReset();
    s_lastRenderedFrame = 0xFFFFFFFFu;
    ModernPresentationClockReset(&s_presentationClock);
    fprintf(stderr, "rage-port: renderer switched to %s\n",
            s_enabled ? "modern" : "classic");
}

/* Read back what the modern renderer is presenting. The frame capture the
 * smoke executable uses reads PS1 video memory, which the modern renderer
 * never writes to: it presents a texture of its own. Without this its output
 * cannot be looked at, which is the only way to tell whether the geometry it
 * draws is right. */
int ModernCaptureFrame(const char *path) {
    if (!ModernPresentationActive() || !s_resourcesReady || s_device == NULL || path == NULL ||
        path[0] == '\0') {
        return 0;
    }
    return ModernWriteTexturePpm(s_device, ModernPresentTexture(),
                                     s_targetW, s_targetH, path);
}

int ModernCullMarginX(void) {
    float margin;
    if (!ModernPresentationActive()) return 0;
    margin = (PortAspectLogicalWidth(s_config.modernAspect) - 320.0f) * 0.5f;
    return (int)ceilf(margin);
}

int ModernDepthLimit(void) {
    float multiplier;
    if (!s_enabled) return 0;
    multiplier = s_config.modernDrawDistance;
    if (multiplier <= 1.0f) return 0;
    if (multiplier > (float)MODERN_FAR / 448.0f)
        multiplier = (float)MODERN_FAR / 448.0f;
    return (int)(448.0f * multiplier);
}
