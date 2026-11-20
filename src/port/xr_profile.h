#ifndef RAGE_XR_PROFILE_H
#define RAGE_XR_PROFILE_H

/* Per-headset defaults for the [xr] performance settings. The profile is
 * picked from the device ([xr] profile = auto, the default) or named
 * explicitly: quest2, questpro, quest3, quest3s. A value written in the ini
 * always wins over the profile. */

/* The ini value for an "xr." key, else the profile's default, else NULL. */
const char *XrProfileSetting(const char *key);

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
#endif

static inline int XrDebugProp(const char *name, int fallback) {
#ifdef __ANDROID__
    enum { SLOTS = 8 };
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
#endif
    (void)name;
    return fallback;
}

#endif
