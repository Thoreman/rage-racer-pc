#include <psyz.h>
#include <psyz/video.h>
#ifdef __ANDROID__
/* Renames main() to SDL_main(), the entry point SDLActivity calls. */
#include <SDL3/SDL_main.h>
#include "android_bootstrap.h"
#endif
#include <psyz/audio.h>
#include <libetc.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "input_config.h"
#include "content_options.h"
#include "car_catalog.h"
#include "car_catalog_profile.h"
#include "mod_assets.h"
#include "diagnostic_log.h"
#include "host_storage.h"
#include "host_disc.h"
#include "runtime_config.h"
#include "xr_profile.h"
#include "timing_control.h"
#include "modern/modern_renderer.h"
#include "modern/modern_assets.h"
#include "native_asset_importer.h"
#include "track_export.h"
#include "car_export.h"
#include "platform_paths.h"
#include "menu_music_runtime.h"
#include "game/car.h"
#include "game/render.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

void MainLoop(void);
int InitNativeGameData(void);

typedef enum ArchiveDumpResult {
    ARCHIVE_DUMP_NOT_REQUESTED,
    ARCHIVE_DUMP_COMPLETE,
    ARCHIVE_DUMP_FAILED,
} ArchiveDumpResult;

static void LoadInputConfig(RageInputConfig *config, const char *argv0) {
    char path[PATH_MAX];
    if (PlatformFindConfigFile(argv0, "rage-input.cfg", path,
                                   sizeof(path)))
        InputConfigLoad(config, path);
}

static ArchiveDumpResult DumpRequestedArchive(void) {
    const char *path = RuntimeConfigGet("tools.dump_archive");
    char fingerprint[72];

    if (path == NULL || path[0] == '\0') {
        return ARCHIVE_DUMP_NOT_REQUESTED;
    }
    if (!HostInitDisc()) {
        return ARCHIVE_DUMP_FAILED;
    }
    if (!HostDumpArchive(path,fingerprint)) {
        fprintf(stderr, "rage-port: cannot write %s\n", path);
        return ARCHIVE_DUMP_FAILED;
    }
    fprintf(stderr, "rage-port: archive written to %s\n", path);
    if (RuntimeConfigEnabled("tools.launcher_report")) {
        /* Machine-readable completion, only after the archive is fully written.
         * Region is one of the fixed strings supplied by DiscIdentify. */
        printf("{\"region\":\"%s\",\"fingerprint\":\"%s\"}\n", HostDiscRegion(),fingerprint);
        fflush(stdout);
    }
    return ARCHIVE_DUMP_COMPLETE;
}

int main(int argc, char **argv) {
    RageInputConfig inputConfig;
    RagePortConfig portConfig;
    ArchiveDumpResult archiveDumpResult;
    int inputIndex;
    char logPath[PATH_MAX];
    char carCatalogPath[PATH_MAX];
    char carCatalogError[256] = {0};

#ifdef __ANDROID__
    AndroidBootstrap();
#endif
    if (!RuntimeConfigInit(argc, argv)) return EXIT_FAILURE;
#ifdef __ANDROID__
    /* The XR platform layer starts before the renderer and reads these from
     * the environment: [xr] eye_scale (fraction of the runtime's recommended
     * eye resolution) and refresh_rate (Hz). */
    /* SDL_getenv reads SDL's own environment snapshot, so set them there. */
    if (PortXrAvailable()) XrProfileGraphicsStart();
    /* Unset values come from the headset's profile (xr_profile.c). */
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "PSYZ_XR_EYE_SCALE",
                               XrProfileSetting("xr.eye_scale"), true);
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "PSYZ_XR_REFRESH_RATE",
                               XrProfileSetting("xr.refresh_rate"), true);
    /* Game logic keeps real time even when an XR frame is slow to present:
     * VSync(1) counts from an absolute VBlank schedule (psyz sdl3_common.h).
     * [xr] catchup = false restores the old present-relative pacing. */
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "PSYZ_CATCHUP",
                               RuntimeConfigGet("xr.catchup") == NULL ||
                                       RuntimeConfigEnabled("xr.catchup")
                                   ? "1" : "0",
                               true);
    fprintf(stderr,
            "rage-port: XR profile=%s refresh_rate=%s eye_scale=%s msaa=%s\n",
            XrProfileName(), XrProfileSetting("xr.refresh_rate"),
            XrProfileSetting("xr.eye_scale"), XrProfileSetting("xr.msaa"));
    /* HUD panel placement in metres: hud_width, hud_distance, hud_drop. */
    {
        static const char *const hudKeys[][2] = {
            {"xr.hud_width", "PSYZ_XR_HUD_WIDTH"},
            {"xr.hud_distance", "PSYZ_XR_HUD_DISTANCE"},
            {"xr.hud_drop", "PSYZ_XR_HUD_DROP"},
        };
        size_t i;
        for (i = 0; i < sizeof(hudKeys) / sizeof(hudKeys[0]); i++)
            if (RuntimeConfigGet(hudKeys[i][0]))
                SDL_SetEnvironmentVariable(SDL_GetEnvironment(), hudKeys[i][1],
                                           RuntimeConfigGet(hudKeys[i][0]),
                                           true);
    }
#endif
    if(RuntimeConfigGet("tools.car_raw"))
        return ExportRawCar(RuntimeConfigGet("tools.car_raw"),RuntimeConfigInt("tools.car_bank",10,0,255),RuntimeConfigEnabled("tools.car_rival"),RuntimeConfigInt("tools.car_part",0,0,255),RuntimeConfigGet("tools.car_output"))?EXIT_SUCCESS:EXIT_FAILURE;
    if(RuntimeConfigGet("tools.track_export"))
        return ExportTrack(RuntimeConfigGet("tools.track_export"),
            RuntimeConfigInt("tools.track_bank",88,88,198),RuntimeConfigGet("tools.track_output"))?EXIT_SUCCESS:EXIT_FAILURE;
    if (!DiagnosticLogOpen(logPath, sizeof(logPath))) {
        fprintf(stderr, "rage-port: could not open diagnostic log\n");
    }

    Psyz_SetTitle("Rage Racer");
    if (!HostInitStorage()) {
        fprintf(stderr, "failed to initialize user save storage\n");
        return EXIT_FAILURE;
    }
    Psyz_VideoSetAspectMode(PSYZ_ASPECT_SQUARE);
    PortConfigDefaults(&portConfig);
    PortConfigApplyRuntime(&portConfig);
    PortConfigSetActive(&portConfig);
    fprintf(stderr,
            "rage-port: renderer=%s scale=%.2f aspect=%d fps=%d draw_distance=%.2f post=%d\n",
            portConfig.renderer == RAGE_RENDERER_MODERN ? "modern" : "classic",
            portConfig.modernInternalScale, portConfig.modernAspect,
            portConfig.modernFps, portConfig.modernDrawDistance,
            portConfig.modernPost);
    /* Initialize SDL input before configurable names are resolved to
     * scancodes. GameInitPad later attaches the game's BIOS buffers. */
    PadInit(0);
    InputConfigDefaults(&inputConfig);
    LoadInputConfig(&inputConfig, argc > 0 ? argv[0] : NULL);
    InputConfigApplyRuntime(&inputConfig);
    for (inputIndex = 0; inputIndex < RAGE_INPUT_BUTTON_COUNT; inputIndex++) {
        Psyz_SetKeyboardKey(inputIndex, inputConfig.keys[inputIndex]);
    }
    archiveDumpResult = DumpRequestedArchive();
    if (archiveDumpResult != ARCHIVE_DUMP_NOT_REQUESTED) {
        return archiveDumpResult == ARCHIVE_DUMP_COMPLETE
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    }
    if (!HostInitDisc()) {
        fprintf(stderr, "failed to initialize disc image\n");
        return EXIT_FAILURE;
    }
    if(ModAssetsNeedsSource()) {
        char fingerprint[72];
        if(HostDiscFingerprint(fingerprint))ModAssetsSetSource(HostDiscRegion(),fingerprint);
    }
    if (RuntimeConfigEnabled("tools.car_catalog"))
        return ModernAssetsCarCatalog() ? EXIT_SUCCESS : EXIT_FAILURE;
    if (RuntimeConfigGet("tools.car_export"))
        return ModernAssetsExportCar(RuntimeConfigGet("tools.car_export"),
                                     RuntimeConfigGet("tools.car_output"))
                   ? EXIT_SUCCESS : EXIT_FAILURE;
    TimingInit();
    if (!NativeAssetImporterInit() || !ModernInit(&portConfig))
        return EXIT_FAILURE;
    if (!InitNativeGameData()) {
        fprintf(stderr, "failed to initialize retail game data\n");
        return EXIT_FAILURE;
    }
    ContentOptionsApply();
    g_DuelEnabled = RuntimeConfigEnabled("duel.enabled");
    g_DuelRivalCar = RuntimeConfigInt("duel.rival_slot", 0, 0,
                                      RACE_CAR_SLOT_COUNT - 1);
    g_CpuChaseMode = RuntimeConfigInt("cpu_chase.mode", 0, 0, 2);
    g_CpuChaseRivalCar = RuntimeConfigInt("cpu_chase.rival_slot", 0, 0,
                                         RACE_CAR_SLOT_COUNT - 1);
    g_CpuChaseNearDistance = RuntimeConfigInt("cpu_chase.near_distance", 8,
                                              -999, 999);
    g_CpuChaseFarDistance = RuntimeConfigInt("cpu_chase.far_distance", 50,
                                             1, 1000);
    g_CpuChaseSpeedPercent = RuntimeConfigInt("cpu_chase.speed_percent", 115,
                                              100, 300);
    CarCatalogClearOverrides();
    const char *configuredProfile = RuntimeConfigGet("cars.profile");
    if (!CarCatalogPrepareProfile(argc > 0 ? argv[0] : NULL,
                                  HostDiscRegion(),
                                  configuredProfile && *configuredProfile
                                      ? configuredProfile : ModAssetsCarProfile(),
                                  carCatalogPath,
                                  sizeof(carCatalogPath)) ||
        !CarCatalogLoadFile(carCatalogPath, carCatalogError,
                            sizeof(carCatalogError))) {
        /* The retail tables loaded from rage.bin are the baseline. A typo in
         * an optional mod file must never prevent the player reaching the menu. */
        fprintf(stderr, "rage-port: ignoring car catalog override: %s\n",
                carCatalogError[0] != '\0' ? carCatalogError : "cannot copy profile");
        CarCatalogClearOverrides();
    } else {
        fprintf(stderr, "rage-port: car catalog=%s\n", carCatalogPath);
    }
    CarCatalogSetRetailManualOnly(RuntimeConfigEnabled("cars.retail_manual_only"));
    CarCatalogApplyMetadata();
    if (!MenuMusicPrepare()) {
        Psyz_AudioDestroy();
        ModernShutdown();
        return EXIT_FAILURE;
    }
    MainLoop();
    Psyz_PcmMusicUnload();
    Psyz_AudioDestroy();
    ModernShutdown();
    return EXIT_SUCCESS;
}
