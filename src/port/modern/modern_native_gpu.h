#ifndef RAGE_MODERN_NATIVE_GPU_H
#define RAGE_MODERN_NATIVE_GPU_H

#include <SDL3/SDL.h>
#include <stdio.h>

#include "render/render_world.h"

int ModernNativeGpuInit(SDL_GPUDevice *device, int linearTextureFilter);
void ModernNativeGpuShutdown(void);
/* world->frame is a presentation revision, not necessarily a simulation tick.
 * Callers must advance it when camera/instance/environment contents change.
 * Preparation may reuse an unchanged revision and aspect within the same
 * asset generation; aspect changes are detected independently. The game
 * interpolation adapter issues a fresh revision for each presentation. */
void ModernNativeGpuPrepare(const RenderWorld *world, float aspect);
/* Call after successfully submitting the command buffer used by Draw and
 * DrawMirror. On cancellation/submission failure, shut down this renderer
 * before reuse: cached textures and geometry may refer to discarded uploads. */
void ModernNativeGpuSubmitted(void);
/* Backend-owned immutable values, borrowed until the next preparation or
 * shutdown. Mesh/material IDs still refer to external asset generations. */
const RenderWorld *ModernNativeGpuPreparedWorld(void);
uint64_t ModernNativeGpuTextureRevision(void);
int ModernNativeGpuWriteDrawDump(FILE *file);
/* Diagnostic CPU-only measurement of the resident world; restores its revision.
 * Invoke between submitted frames, with performance/asset tracing disabled. */
int ModernNativeGpuBenchmarkPrepare(FILE *file, unsigned repeats);
int ModernNativeGpuWriteProbe(FILE *file, int x, int y,
                              int width, int height);
int ModernNativeGpuHasDraws(void);
int ModernNativeGpuWorldComplete(void);
int ModernNativeGpuHasMirrorDraws(void);
float ModernNativeGpuMirrorPanelY(void);
void ModernNativeGpuDraw(SDL_GPUCommandBuffer *command,
                         SDL_GPUTexture *colorTarget,
                         SDL_GPUTexture *depthTarget,
                         int clearColor, int drawSky, int targetHeight);

/* One XR eye relative to the game camera: pose in metres in the game
 * camera's own view space (+X right, +Y up, -Z forward) plus the eye's
 * asymmetric field-of-view tangents (left/down negative). */
typedef struct ModernNativeEyeView {
    float position[3];
    float orientation[4]; /* quaternion x, y, z, w */
    float tanLeft, tanRight, tanUp, tanDown;
} ModernNativeEyeView;

/* Draws the prepared world once per eye, each with its own sky. worldScale
 * converts metres of head movement into world units. */
/* With resolveTargets, colour/depth are 4x multisampled and colour resolves
 * into resolveTargets (alpha = geometry coverage); requires
 * ModernNativeGpuEyeMsaaReady(). Pass NULL for single-sampled targets. */
void ModernNativeGpuDrawStereo(SDL_GPUCommandBuffer *command,
                               SDL_GPUTexture *const colorTargets[2],
                               SDL_GPUTexture *const depthTargets[2],
                               SDL_GPUTexture *const resolveTargets[2],
                               const ModernNativeEyeView eyes[2],
                               float worldScale, int targetHeight);
/* Uploads textures and resident geometry the prepared world (main and
 * mirror views) needs but does not have yet, stopping after budgetNs or
 * when the upload queue is full. Returns 1 while some remain. Submit the
 * command buffer and call ModernNativeGpuSubmitted before the next step. */
int ModernNativeGpuWarmStep(SDL_GPUCommandBuffer *command, Uint64 budgetNs,
                            unsigned *loaded);
/* Builds the 4x MSAA eye pipelines on first call; 0 if unavailable. */
int ModernNativeGpuEyeMsaaReady(void);
/* Sample count of the XR MSAA eye targets and pipelines ([xr] msaa_samples). */
SDL_GPUSampleCount ModernNativeGpuEyeSampleCount(void);
void ModernNativeGpuDrawMirror(SDL_GPUCommandBuffer *command,
                               SDL_GPUTexture *colorTarget,
                               SDL_GPUTexture *depthTarget, int targetHeight);

#endif
