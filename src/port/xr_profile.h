#ifndef RAGE_XR_PROFILE_H
#define RAGE_XR_PROFILE_H

/* The ini value for an "xr." key, else the profile's default, else NULL.
 * Layering below the menu file; GraphicsSettingsInit takes this as its base. */
const char *XrProfileBaseSetting(const char *key);

/* The effective value: the VR graphics menu's value for the keys it owns
 * (once XrProfileGraphicsStart has run), else XrProfileBaseSetting(key). */
const char *XrProfileSetting(const char *key);

/* Initialises the VR graphics settings over XrProfileBaseSetting and loads
 * the user's saved "graphics" file. Call once at startup, VR builds only. */
void XrProfileGraphicsStart(void);

/* 1 when this build can run in a headset (Android, or RAGE_PC_XR). */
int PortXrAvailable(void);

/* Per-headset defaults for the [xr] performance settings. The profile is
 * picked from the device ([xr] profile = auto, the default) or named
 * explicitly: quest2, questpro, quest3, quest3s. A value written in the ini
 * always wins over the profile. */

/* 1 unless the resolved setting is present and spells false. */
int XrProfileEnabled(const char *key);

/* The profile in use, for the log. */
const char *XrProfileName(void);

/* Development aid: the integer in Android system property
 * "debug.rr.<name>", re-read about once a second so it can be flipped with
 * `adb shell setprop` while the game runs; fallback when unset (and always
 * on other platforms). Header-only so tools that link the renderer without
 * the rest of the port still build; each file keeps its own small cache. */
#ifdef __ANDROID__
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#elif defined(RAGE_PC_XR)
#include <stdlib.h>
#include <string.h>
#endif

/* Compile-time: this build carries the VR code paths (Quest, or PC VR).
 * At run time, PortXrAvailable() says whether a headset is in use. */
#if defined(__ANDROID__) || defined(RAGE_PC_XR)
#define RAGE_XR_BUILD 1
#else
#define RAGE_XR_BUILD 0
#endif

static inline int XrDebugProp(const char *name, int fallback) {
#ifdef __ANDROID__
    enum { SLOTS = 16 };
    static struct {
        const char *name;
        int value, ticks;
    } cache[SLOTS];
    int i;
    for (i = 0; i < SLOTS; i++) {
        if (cache[i].name == NULL) cache[i].name = name;
        if (cache[i].name != name) continue;
        if (cache[i].ticks-- <= 0) {
            char key[PROP_NAME_MAX], value[PROP_VALUE_MAX] = {0};
            strcpy(key, "debug.rr.");
            strncat(key, name, sizeof(key) - strlen(key) - 1);
            cache[i].value = __system_property_get(key, value) > 0
                ? atoi(value) : fallback;
            cache[i].ticks = 100;
        }
        return cache[i].value;
    }
#elif defined(RAGE_PC_XR)
    /* PC VR: environment variable RAGE_DEBUG_<NAME> (upper-cased), read
     * once per name. */
    enum { SLOTS = 16 };
    static struct {
        const char *name;
        int value;
    } cache[SLOTS];
    int i;
    for (i = 0; i < SLOTS; i++) {
        if (cache[i].name == NULL) {
            char key[64] = "RAGE_DEBUG_";
            size_t at = strlen(key);
            const char *p;
            const char *text;
            for (p = name; *p && at + 1 < sizeof(key); p++)
                key[at++] = (char)((*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
            key[at] = '\0';
            text = getenv(key);
            cache[i].name = name;
            cache[i].value = text != NULL && *text ? atoi(text) : fallback;
        }
        if (cache[i].name == name) return cache[i].value;
    }
#endif
    (void)name;
    return fallback;
}

#endif
