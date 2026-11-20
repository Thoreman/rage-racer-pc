#include "rage/render_world_game.h"
#include "modern/modern_renderer.h"
#include "modern/scene_capture.h"
#include "fmv_audio.h"
#include "mod_assets.h"
#include "timing_control.h"
#include "game/asset.h"
#include "game/race.h"
#include "game/state.h"
#include "xr_profile.h"
#include "runtime_config.h"
#include "platform_paths.h"
#include "rage/compat.h"
#include "game/scene.h"
#include <SDL3/SDL.h>
#include <psyz/video.h>
#include <stdio.h>
#include <stdlib.h>
#include <libgte.h>

extern int32_t g_FrameCounter;
static unsigned s_extraCourseGeneration;

static void SyncExtraCourseCatalog(void) {
    unsigned generation = ModAssetsGeneration();
    u32 mask = 0;
    s32 i;

    if (s_extraCourseGeneration == generation) return;
    s_extraCourseGeneration = generation;
    for (i = 0; i < EXTRA_COURSE_SLOT_COUNT; i++) {
        s32 slot = EXTRA_COURSE_SLOT_BASE + i;
        s32 firstAsset = TrackCourseAssetIndex(ASSET_TRACK_1ST_BASE, 0, slot);
        s32 secondAsset = TrackCourseAssetIndex(ASSET_TRACK_2ND_BASE, 0, slot);
        if (firstAsset >= 0 && secondAsset >= 0 &&
            ModAssetExists(firstAsset) && ModAssetExists(secondAsset)) {
            mask |= (1u << (u32)i);
        }
        ModAssetCourseLabel(i,
                            g_RaceSession.extraCourseNames[i],
                            sizeof(g_RaceSession.extraCourseNames[i]),
                            g_RaceSession.extraCourseLengths[i],
                            sizeof(g_RaceSession.extraCourseLengths[i]));
    }
    g_RaceSession.extraCourseMask = mask;
}

/*
 * Fallbacks for the two hooks the smoke and scenario builds provide. They are
 * weak so a build that carries the real one wins, which every build of the
 * game does. The Windows linker does not honour weak definitions, and takes
 * two of the same symbol as an error rather than a preference, so there it
 * gets the declarations only and the real definitions do the work.
 */
void PortSmokeBeforeSceneHandler(void);
void PortScenarioBeforeSceneHandler(void);
#ifndef _MSC_VER
void PortSmokeBeforeSceneHandler(void) __attribute__((weak));
void PortSmokeBeforeSceneHandler(void) {}
void PortScenarioBeforeSceneHandler(void) __attribute__((weak));
void PortScenarioBeforeSceneHandler(void) {}
#else
/* The scenario hook is in every build of the game, so it needs no stand-in
 * here; the smoke hook is only in the smoke build, so it does, except in the
 * smoke build itself. */
#ifndef RAGE_SMOKE_TARGET
void PortSmokeBeforeSceneHandler(void) {}
#endif
#endif

void PortBeforeSceneHandler(void) {
    SyncExtraCourseCatalog();
    HostFmvAudioTick();
    TimingApply();
#ifdef RAGE_SMOKE_TARGET
    PortSmokeBeforeSceneHandler();
    /* Scenario input is synthesized after physical/test input sampling so it
     * cannot be cleared by the pad edge update in the same frame. */
    PortScenarioBeforeSceneHandler();
#else
    PortScenarioBeforeSceneHandler();
    PortSmokeBeforeSceneHandler();
#endif
    GameRenderWorldBeginFrame((uint64_t)g_FrameCounter);
    CaptureFrameBegin();
}

/* Development aid (adb shell setprop debug.rr.stats 1): logic ticks and
 * game-clock units per real second, to check the game runs at true speed.
 * NTSC race: 30 ticks and 90 clock units; PAL race: 25 and 75. */
static void PortLogGameSpeed(void) {
    static Uint64 windowStart;
    static s32 ticks, clockStart;
    Uint64 now = SDL_GetTicksNS();
    if (!XrDebugProp("stats", 0)) return;
    {
        /* How long each race phase lasts, in real seconds and in ticks, to
         * compare the countdown with the console (ticks / 30 s on NTSC). */
        static s16 lastPhase = -1;
        static s32 lastScene = -1;
        static Uint64 phaseStart;
        static s32 phaseTicks;
        if (g_RacePhase != lastPhase || g_SceneId != lastScene) {
            if (phaseStart != 0 && lastScene == GAME_SCENE_RACE)
                fprintf(stderr, "rage-port: race phase %d lasted %.2f s, "
                        "%d ticks\n", lastPhase,
                        (double)(now - phaseStart) / 1e9, phaseTicks);
            lastPhase = g_RacePhase;
            lastScene = g_SceneId;
            phaseStart = now;
            phaseTicks = 0;
        }
        phaseTicks++;
    }
    if (windowStart == 0) {
        windowStart = now;
        clockStart = g_GameClock;
        ticks = 0;
        return;
    }
    ticks++;
    if (now - windowStart >= 2000000000u) {
        double seconds = (double)(now - windowStart) / 1e9;
        fprintf(stderr, "rage-port: speed scene=%d phase=%d ticks/s=%.1f "
                "clock/s=%.1f\n", g_SceneId, g_RacePhase, ticks / seconds,
                (double)(g_GameClock - clockStart) / seconds);
        windowStart = now;
        clockStart = g_GameClock;
        ticks = 0;
    }
}

/* Race game speed in percent, 50 to 100 in steps of 10 (Quest only; the PC
 * build always runs at 100). Races run at that fraction of the console's
 * speed; menus, movies and their sound stay at full speed. The default is
 * [xr] game_speed (a fraction; 0.6 on Quest if unset); the pause menu's SPEED row
 * changes it and it is remembered in the user config file "game-speed". */
enum {
    GAME_SPEED_MIN = 50,
    GAME_SPEED_MAX = 100,
    GAME_SPEED_STEP = 10,
    GAME_SPEED_DEFAULT_XR = 60,
};
static int s_gameSpeedPercent = -1;

static int ClampGameSpeed(int percent) {
    percent = (percent + GAME_SPEED_STEP / 2) / GAME_SPEED_STEP *
              GAME_SPEED_STEP;
    if (percent < GAME_SPEED_MIN) return GAME_SPEED_MIN;
    if (percent > GAME_SPEED_MAX) return GAME_SPEED_MAX;
    return percent;
}

static void LoadGameSpeed(void) {
    char path[4096];
    FILE *file;
    const char *value;
    int saved;

    if (s_gameSpeedPercent >= 0) return;
    s_gameSpeedPercent = GAME_SPEED_MAX;
#ifdef __ANDROID__
    /* Console speed feels much faster in VR than on a TV (life-size scenery,
     * no frame); 60% was the comfortable default in play-testing. */
    s_gameSpeedPercent = GAME_SPEED_DEFAULT_XR;
    value = RuntimeConfigGet("xr.game_speed");
    if (value != NULL)
        s_gameSpeedPercent = ClampGameSpeed((int)(strtod(value, NULL) * 100.0 + 0.5));
    if (PlatformUserConfigPath("game-speed", path, sizeof(path)) &&
        (file = fopen(path, "r")) != NULL) {
        if (fscanf(file, "%d", &saved) == 1)
            s_gameSpeedPercent = ClampGameSpeed(saved);
        fclose(file);
    }
    fprintf(stderr, "rage-port: race game speed %d%%\n", s_gameSpeedPercent);
#else
    (void)path; (void)file; (void)value; (void)saved;
#endif
}

static void SaveGameSpeed(void) {
    char directory[4096];
    char path[4096];
    FILE *file;

    if (!PlatformUserConfigDirectory(directory, sizeof(directory)) ||
        !PlatformEnsureDirectory(directory) ||
        !PlatformUserConfigPath("game-speed", path, sizeof(path)))
        return;
    file = fopen(path, "w");
    if (file == NULL) return;
    fprintf(file, "%d\n", s_gameSpeedPercent);
    fclose(file);
}

int PortAdjustGameSpeed(int direction) {
    int next;

    LoadGameSpeed();
#ifndef __ANDROID__
    return 0;
#endif
    if (direction == 0) return 0;
    next = ClampGameSpeed(s_gameSpeedPercent +
                          (direction < 0 ? -GAME_SPEED_STEP : GAME_SPEED_STEP));
    if (next == s_gameSpeedPercent) return 0;
    s_gameSpeedPercent = next;
    SaveGameSpeed();
    return 1;
}

void PortCycleGameSpeed(void) {
    LoadGameSpeed();
#ifdef __ANDROID__
    s_gameSpeedPercent = s_gameSpeedPercent >= GAME_SPEED_MAX
        ? GAME_SPEED_MIN : s_gameSpeedPercent + GAME_SPEED_STEP;
    SaveGameSpeed();
#endif
}

void PortGameSpeedLabel(char *text, size_t size) {
    if (text == NULL || size == 0) return;
    LoadGameSpeed();
    snprintf(text, size, "SPEED %d%%", s_gameSpeedPercent);
}

#ifdef __ANDROID__
/* Set again on every tick so only races are slowed. */
static void PortApplyGameSpeed(void) {
    LoadGameSpeed();
    Psyz_VideoSetGameSpeed(g_SceneId == GAME_SCENE_RACE
                               ? s_gameSpeedPercent / 100.0 : 1.0);
    /* XR frames come from the wait between ticks; one more at the end of a
     * tick lands before the next tick's logic and judders. debug.rr.tickskip
     * 0 restores it. */
    Psyz_VideoSetSkipTickPresent(XrDebugProp("tickskip", 1));
}
#endif

void PortAfterSceneHandler(void) {
#ifdef __ANDROID__
    PortApplyGameSpeed();
#endif
    PortLogGameSpeed();
    GameRenderWorldPublishCurrentCamera();
    if (ModernIsEnabled()) {
        GameRenderWorldPublishCourseObjects();
        GameRenderWorldPublishTerrainGrid();
        GameRenderWorldPublishRaceCars();
        GameRenderWorldDiscardLegacyMirror();
    }
    CaptureFrameEnd();
    GameRenderWorldEndFrame();
    ModernLogicFrameReady((uint32_t)g_FrameCounter);
}

void PortDuringFrameWait(int frameLimit) {
    ModernFrameWaitTick(frameLimit);
}

int PortModernRendererEnabled(void) {
    return ModernIsEnabled();
}

void PortToggleRenderer(void) {
    ModernToggle();
}

void PortAfterFrameTransfers(void) {
    ModernFrameTexturesReady();
}

long SpuTransferStatus(void *address, long mode) {
    (void)address;
    (void)mode;
    return 0;
}
