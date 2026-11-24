#include "xr_profile.h"

#include "runtime_config.h"
#include "graphics_settings.h"
#include "platform_paths.h"

#include <stdlib.h>
#include <string.h>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#ifdef RAGE_PC_XR
#include <psyz/xr.h>
#include <stdio.h>
#endif

typedef struct XrProfile {
    const char *name;
    const char *device; /* ro.product.device */
    const char *refreshRate;
    const char *eyeScale;
    const char *msaa;
} XrProfile;

/* Quest 3 values are measured at stock clocks (GPU level 5 via the
 * manifest's CPU-for-GPU trade). The eye passes are fragment bound: the
 * heaviest view measured (a rock cutting) took ~22 ms with 2x MSAA and
 * ~13 ms without, so MSAA is off by default ([xr] msaa = true turns it on,
 * msaa_samples picks 2 or 4). Quest 3S has the same processor and fewer
 * pixels. Quest 2 and Quest Pro have roughly half the GPU: untested
 * estimates. */
static const XrProfile s_profiles[] = {
    {"quest3", "eureka", "72", "1.0", "false"},
    {"quest3s", "panther", "72", "1.0", "false"},
    {"quest2", "hollywood", "72", "0.7", "false"},
    {"questpro", "seacliff", "72", "0.75", "false"},
};

static const XrProfile s_fallback = {"default", "", "72", "1.0", "false"};

#ifdef RAGE_PC_XR
/* PC VR: the headset is identified from the OpenXR system name (lower-case,
 * letters and digits only, matched as a substring; first match wins, so
 * quest3s precedes quest3). Rendering runs on the PC's GPU, so every PC
 * profile defaults to the HIGH preset (graphics_settings.c gives HIGH to
 * any profile not named quest2/questpro) and asks for 90 Hz (the highest
 * rate at or below it, when the runtime exposes XR_FB_display_refresh_rate;
 * otherwise the runtime's own rate). Untested estimates. */
static const XrProfile s_pcProfiles[] = {
    {"pc-quest3s", "quest3s", "90", "1.0", "false"},
    {"pc-quest3", "quest3", "90", "1.0", "false"},
    {"pc-questpro", "questpro", "90", "1.0", "false"},
    {"pc-quest2", "quest2", "90", "1.0", "false"},
};
static const XrProfile s_pcFallback = {"pc-default", "", "90", "1.0", "false"};

static int s_pcProbe = -1; /* -1 not run, 0 no headset, 1 headset */
static char s_pcSystemName[256];

static int PcXrProbe(void) {
    if (s_pcProbe < 0) {
        s_pcProbe = Psyz_XrProbeSystemName(s_pcSystemName,
                                           sizeof(s_pcSystemName));
        fprintf(stderr, "rage-port: PC VR probe: %s%s%s\n",
                s_pcProbe ? "headset \"" : "no headset",
                s_pcProbe ? s_pcSystemName : "", s_pcProbe ? "\"" : "");
    }
    return s_pcProbe;
}

static const XrProfile *ResolvePc(void) {
    char norm[256];
    size_t i, n = 0;
    const char *p;

    for (p = s_pcSystemName; *p && n + 1 < sizeof(norm); p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) norm[n++] = c;
    }
    norm[n] = '\0';
    for (i = 0; i < sizeof(s_pcProfiles) / sizeof(s_pcProfiles[0]); i++)
        if (strstr(norm, s_pcProfiles[i].device) != NULL)
            return &s_pcProfiles[i];
    return &s_pcFallback;
}
#endif

static const XrProfile *Resolve(void) {
    static const XrProfile *resolved;
    const char *wanted;
    size_t i;

    if (resolved != NULL) return resolved;
    resolved = &s_fallback;
#ifdef RAGE_PC_XR
    resolved = &s_pcFallback;
#endif
    wanted = RuntimeConfigGet("xr.profile");
    if (wanted != NULL && strcmp(wanted, "auto") != 0) {
        for (i = 0; i < sizeof(s_profiles) / sizeof(s_profiles[0]); i++)
            if (strcmp(wanted, s_profiles[i].name) == 0)
                resolved = &s_profiles[i];
#ifdef RAGE_PC_XR
        for (i = 0; i < sizeof(s_pcProfiles) / sizeof(s_pcProfiles[0]); i++)
            if (strcmp(wanted, s_pcProfiles[i].name) == 0)
                resolved = &s_pcProfiles[i];
#endif
        return resolved;
    }
#ifdef RAGE_PC_XR
    PcXrProbe();
    resolved = ResolvePc();
#endif
#ifdef __ANDROID__
    {
        char device[PROP_VALUE_MAX] = {0};
        __system_property_get("ro.product.device", device);
        for (i = 0; i < sizeof(s_profiles) / sizeof(s_profiles[0]); i++)
            if (strcmp(device, s_profiles[i].device) == 0)
                resolved = &s_profiles[i];
    }
#endif
    return resolved;
}

const char *XrProfileName(void) { return Resolve()->name; }

const char *XrProfileBaseSetting(const char *key) {
    const char *value = RuntimeConfigGet(key);
    const XrProfile *profile;

    if (value != NULL || key == NULL) return value;
    profile = Resolve();
    if (strcmp(key, "xr.refresh_rate") == 0) return profile->refreshRate;
    if (strcmp(key, "xr.eye_scale") == 0) return profile->eyeScale;
    if (strcmp(key, "xr.msaa") == 0) return profile->msaa;
    return NULL;
}

static int s_graphicsReady;

/* The graphics layer's base: ini, then the profile table, except msaa, which
 * the default preset owns (ini -> default preset -> profile). */
static const char *GraphicsBase(const char *key) {
    if (key != NULL && strcmp(key, "xr.msaa") == 0) return RuntimeConfigGet(key);
    return XrProfileBaseSetting(key);
}

const char *XrProfileSetting(const char *key) {
    const char *value = s_graphicsReady && key != NULL ? GraphicsSettingGet(key) : NULL;

    return value != NULL ? value : XrProfileBaseSetting(key);
}

void XrProfileGraphicsStart(void) {
    char path[4096];

    GraphicsSettingsInit(GraphicsBase, XrProfileName());
    s_graphicsReady = 1;
    if (PlatformUserConfigPath("graphics", path, sizeof(path)))
        GraphicsSettingsLoad(path);
}

int XrProfileEnabled(const char *key) {
    const char *value = XrProfileSetting(key);

    if (value == NULL) return 1;
    return !(strcmp(value, "false") == 0 || strcmp(value, "0") == 0 ||
             strcmp(value, "off") == 0 || strcmp(value, "no") == 0);
}

int PortXrAvailable(void) {
#if defined(RAGE_PC_XR)
    /* PC VR: a headset answered the startup probe. (Spike limitation: if
     * the OpenXR session then fails, psyz falls back to a window but the
     * VR menus/pacing stay on.) */
    return PcXrProbe();
#elif defined(__ANDROID__)
    return 1;
#else
    return 0;
#endif
}
