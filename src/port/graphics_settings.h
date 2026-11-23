#ifndef GRAPHICS_SETTINGS_H
#define GRAPHICS_SETTINGS_H

#include <stddef.h>

/* The VR graphics menu's settings: rows, presets, and the saved file. Pure
 * data, no GPU or game dependencies. GraphicsSettingGet is the effective
 * value for the keys the menu owns: menu value, then the caller's base lookup
 * (ini, then headset profile), then the default preset. */

typedef enum GraphicsRow {
    GFX_ROW_PRESET,
    GFX_ROW_FOVEATION,
    GFX_ROW_ANTIALIAS,
    GFX_ROW_RESOLUTION,
    GFX_ROW_TEXTURE_DETAIL,
    GFX_ROW_CAR_LAMPS,
    GFX_ROW_LAMP_LIGHTING,
    GFX_ROW_LAMP_GLOWS,
    GFX_ROW_SHADOWS,
    GFX_ROW_MIRROR,
    GFX_ROW_SHARPEN,
    GFX_ROW_COLOUR,
    GFX_ROW_REFRESH,
    GFX_ROW_COUNT
} GraphicsRow;

/* The ini-then-profile lookup, supplied by the caller. May return NULL. */
typedef const char *(*GraphicsBaseLookup)(const char *key);

/* Clears the menu values and picks the default preset from the profile name. */
void GraphicsSettingsInit(GraphicsBaseLookup base, const char *profileName);

/* 1 on success, 0 on failure. A missing file on load is success with nothing
 * loaded. Bad lines and bad values are ignored. */
int GraphicsSettingsLoad(const char *path);
int GraphicsSettingsSave(const char *path);

/* Effective value, or NULL for a key the menu does not own or has no value
 * for. Valid until the next call that changes settings. */
const char *GraphicsSettingGet(const char *key);

/* Row text such as "FOVEATION          HIGH": name left, value right-aligned. */
void GraphicsRowLabel(GraphicsRow row, char *text, size_t size);

/* The same row split into its name and its value, for the font page. Both are
 * empty for a row outside the page. */
void GraphicsRowParts(GraphicsRow row, char *name, size_t nameSize,
                      char *value, size_t valueSize);

/* Steps a row by direction (+1/-1), clamping at the ends. PRESET applies the
 * chosen preset; changing a preset row (FOVEATION, ANTI-ALIAS, TEXTURE DETAIL,
 * CAR LAMPS, LAMP LIGHTING, LAMP GLOWS, SHADOWS, MIRROR) by hand makes the
 * PRESET CUSTOM (SHARPEN, RESOLUTION, COLOUR and REFRESH do not). */
void GraphicsRowAdjust(GraphicsRow row, int direction);

const char *GraphicsRowNote(GraphicsRow row);

/* Increments on every adjust and load. */
unsigned GraphicsSettingsGeneration(void);

/* All the menu's editable state, so the page can undo an edit. Take it before
 * editing a row; Restore puts everything back (values, preset, CUSTOM flag) and
 * bumps the generation only if something differed. */
typedef struct GraphicsSettingsSnapshot {
    char menu[16][16];
    int menuSet[16];
    int forceCustom;
    int presetChosen;
} GraphicsSettingsSnapshot;

void GraphicsSettingsSnapshotTake(GraphicsSettingsSnapshot *out);
void GraphicsSettingsSnapshotRestore(const GraphicsSettingsSnapshot *in);

#endif
