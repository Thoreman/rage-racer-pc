#include "graphics_settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    K_FOVEATION, K_MSAA, K_SAMPLES, K_EYE_SCALE, K_LAMP_LIGHTING,
    K_LAMP_GLOWS, K_SHADOWS, K_REFRESH, K_SATURATION, K_CONTRAST,
    K_LAMPS, K_MIRROR, K_TEXTURE, K_SHARPEN, K_FPS, K_COUNT
};

static const char *const kKeyNames[K_COUNT] = {
    "xr.foveation", "xr.msaa", "xr.msaa_samples", "xr.eye_scale",
    "xr.lamp_lighting", "xr.lamp_glows", "xr.shadows", "xr.refresh_rate",
    "xr.saturation", "xr.contrast", "xr.max_spot_lights", "xr.mirror",
    "xr.texture_detail", "xr.sharpen", "xr.fps_display"
};

enum { PRESET_LOW, PRESET_MEDIUM, PRESET_HIGH, PRESET_ULTRA, PRESET_COUNT };
#define PRESET_CUSTOM (-1)

static const char *const kPresetNames[PRESET_COUNT] = {
    "LOW", "MEDIUM", "HIGH", "ULTRA"
};

typedef struct PresetValues {
    const char *foveation;
    int antialias; /* 0 off, 2, 4 */
    int lampLighting, lampGlows, shadows;
    int lamps;
    const char *mirror, *texture;
} PresetValues;

static const PresetValues kPresets[PRESET_COUNT] = {
    {"high", 0, 0, 0, 0, 2, "half", "standard"},
    {"high", 0, 1, 0, 1, 4, "half", "standard"},
    {"high", 2, 0, 0, 0, 4, "half", "standard"},
    {"medium", 2, 1, 1, 1, 8, "full", "high"},
};

static const char *const kFoveation[] = {"off", "low", "medium", "high"};
static const int kRefresh[] = {72, 80, 90, 120};
static const char *const kMirror[] = {"off", "half", "full"};
static const char *const kMirrorLabel[] = {"OFF", "HALF RATE", "FULL"};
static const char *const kTexture[] = {"standard", "high"};
static const char *const kTextureLabel[] = {"STANDARD", "HIGH"};
static const int kLamps[] = {2, 4, 6, 8, 72}; /* 72 = ALL, the renderer's ceiling */
static const double kSharpen[] = {0.0, 0.35, 0.70};
static const char *const kSharpenLabel[] = {"OFF", "LOW", "HIGH"};

/* PC VR: GPUs with headroom get 8X anti-aliasing and supersampling to
 * 150% eye resolution. The Quest keeps 4X and 100%. */
#if defined(RAGE_PC_XR)
#define GFX_MAX_AA_INDEX 3
#define GFX_MAX_PCT 150
#else
#define GFX_MAX_AA_INDEX 2
#define GFX_MAX_PCT 100
#endif

#define VIVID_SATURATION 1.25
#define VIVID_CONTRAST 1.08

typedef char SnapshotFitsKeys[(K_COUNT <= 16) ? 1 : -1];
static GraphicsBaseLookup gBase;
static int gDefaultPreset = PRESET_HIGH;
static char gMenu[K_COUNT][16];
static int gMenuSet[K_COUNT];
static unsigned gGeneration;
static int gForceCustom; /* a row was changed by hand since the last preset */
static int gPresetChosen; /* the player picked (or the file named) a preset */
static int gVividDefault; /* Quest 3/3S: VIVID colour and HIGH sharpen by default */

static int StrEqI(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return *a == *b;
}

static const char *Bool(int v) { return v ? "true" : "false"; }

static int KeyIndex(const char *key) {
    int i;
    if (!key) return -1;
    for (i = 0; i < K_COUNT; i++)
        if (!strcmp(key, kKeyNames[i])) return i;
    return -1;
}

static void MenuSet(int k, const char *value) {
    snprintf(gMenu[k], sizeof gMenu[k], "%s", value);
    gMenuSet[k] = 1;
}

static void MenuSetF(int k, double v) {
    snprintf(gMenu[k], sizeof gMenu[k], "%.2f", v);
    gMenuSet[k] = 1;
}

static char gPresetNumber[16];

/* The default preset's value for a key, or NULL for keys it does not set. */
static const char *PresetValue(int preset, int k) {
    const PresetValues *p = &kPresets[preset];
    switch (k) {
    case K_FOVEATION: return p->foveation;
    case K_MSAA: return Bool(p->antialias != 0);
    case K_SAMPLES: return p->antialias ? (p->antialias == 4 ? "4" : "2") : NULL;
    case K_LAMP_LIGHTING: return Bool(p->lampLighting);
    case K_LAMP_GLOWS: return Bool(p->lampGlows);
    case K_SHADOWS: return Bool(p->shadows);
    case K_LAMPS:
        snprintf(gPresetNumber, sizeof gPresetNumber, "%d", p->lamps);
        return gPresetNumber;
    case K_MIRROR: return p->mirror;
    case K_TEXTURE: return p->texture;
    case K_SHARPEN: return gVividDefault ? "0.70" : "0.00";
    case K_SATURATION: return gVividDefault ? "1.25" : NULL;
    case K_CONTRAST: return gVividDefault ? "1.08" : NULL;
    case K_FPS: return "false";
    default: return NULL;
    }
}

static const char *Effective(int k) {
    const char *v;
    if (gMenuSet[k]) return gMenu[k];
    if (gBase && (v = gBase(kKeyNames[k])) != NULL) return v;
    return PresetValue(gDefaultPreset, k);
}

static int EffectiveBool(int k) {
    const char *v = Effective(k);
    return v && (StrEqI(v, "true") || !strcmp(v, "1") || StrEqI(v, "on"));
}

static double EffectiveNumber(int k, double fallback) {
    const char *v = Effective(k);
    char *end;
    double d;
    if (!v) return fallback;
    d = strtod(v, &end);
    return end == v ? fallback : d;
}

static int FoveationIndex(void) {
    const char *v = Effective(K_FOVEATION);
    int i;
    for (i = 0; v && i < 4; i++)
        if (StrEqI(v, kFoveation[i])) return i;
    return -1;
}

static int NameIndex(int k, const char *const *names, int count) {
    const char *v = Effective(k);
    int i;
    for (i = 0; v && i < count; i++)
        if (StrEqI(v, names[i])) return i;
    return -1;
}

static int LampsValue(void) {
    double d = EffectiveNumber(K_LAMPS, 6);
    if (d != d) d = atoi(PresetValue(gDefaultPreset, K_LAMPS)); /* NaN: the default */
    if (d < 0) d = 0;
    if (d > 72) d = 72;
    return (int)d;
}

static int SharpenIndex(void) {
    double v = EffectiveNumber(K_SHARPEN, 0.0);
    int i;
    for (i = 0; i < 3; i++)
        if (fabs(v - kSharpen[i]) < 0.005) return i;
    return -1;
}

static int AntialiasValue(void) {
    if (!EffectiveBool(K_MSAA)) return 0;
    { /* matches the renderer */
        int n = (int)EffectiveNumber(K_SAMPLES, 2);
        return n <= 2 ? 2 : (n <= 4 || GFX_MAX_AA_INDEX < 3) ? 4 : 8;
    }
}

static int CurrentPreset(void) {
    if (gForceCustom) return PRESET_CUSTOM;
    int p, fov = FoveationIndex();
    for (p = 0; p < PRESET_COUNT; p++) {
        const PresetValues *v = &kPresets[p];
        if (fov >= 0 && !strcmp(kFoveation[fov], v->foveation) &&
            AntialiasValue() == v->antialias &&
            EffectiveBool(K_LAMP_LIGHTING) == v->lampLighting &&
            EffectiveBool(K_LAMP_GLOWS) == v->lampGlows &&
            EffectiveBool(K_SHADOWS) == v->shadows &&
            LampsValue() == v->lamps &&
            NameIndex(K_MIRROR, kMirror, 3) >= 0 &&
            !strcmp(kMirror[NameIndex(K_MIRROR, kMirror, 3)], v->mirror) &&
            NameIndex(K_TEXTURE, kTexture, 2) >= 0 &&
            !strcmp(kTexture[NameIndex(K_TEXTURE, kTexture, 2)], v->texture))
            return p;
    }
    return PRESET_CUSTOM;
}

static void ApplyPreset(int p) {
    const PresetValues *v = &kPresets[p];
    MenuSet(K_FOVEATION, v->foveation);
    MenuSet(K_MSAA, Bool(v->antialias != 0));
    if (v->antialias) MenuSet(K_SAMPLES, v->antialias == 4 ? "4" : "2");
    MenuSet(K_LAMP_LIGHTING, Bool(v->lampLighting));
    MenuSet(K_LAMP_GLOWS, Bool(v->lampGlows));
    MenuSet(K_SHADOWS, Bool(v->shadows));
    snprintf(gMenu[K_LAMPS], sizeof gMenu[K_LAMPS], "%d", v->lamps);
    gMenuSet[K_LAMPS] = 1;
    MenuSet(K_MIRROR, v->mirror);
    MenuSet(K_TEXTURE, v->texture);
}

static int PresetFromName(const char *name) {
    int p;
    for (p = 0; p < PRESET_COUNT; p++)
        if (StrEqI(name, kPresetNames[p])) return p;
    return PRESET_CUSTOM;
}

void GraphicsSettingsInit(GraphicsBaseLookup base, const char *profileName) {
    gBase = base;
    memset(gMenu, 0, sizeof gMenu);
    memset(gMenuSet, 0, sizeof gMenuSet);
    gDefaultPreset = PRESET_HIGH;
    gForceCustom = 0;
    gPresetChosen = 0;
    gVividDefault = 0;
    if (profileName) {
        if (!strcmp(profileName, "quest3") || !strcmp(profileName, "quest3s"))
            gVividDefault = 1;
        if (!strcmp(profileName, "quest2")) gDefaultPreset = PRESET_LOW;
        else if (!strcmp(profileName, "questpro")) gDefaultPreset = PRESET_MEDIUM;
    }
}

/* Validates a file value for a key and stores it normalised. */
static void StoreChecked(int k, const char *value) {
    char *end;
    double d;
    int i, r;
    switch (k) {
    case K_FOVEATION:
        for (i = 0; i < 4; i++)
            if (StrEqI(value, kFoveation[i])) { MenuSet(k, kFoveation[i]); return; }
        return;
    case K_MSAA: case K_LAMP_LIGHTING: case K_LAMP_GLOWS: case K_SHADOWS: case K_FPS:
        if (StrEqI(value, "true")) MenuSet(k, "true");
        else if (StrEqI(value, "false")) MenuSet(k, "false");
        return;
    case K_SAMPLES:
        if (!strcmp(value, "2") || !strcmp(value, "4") ||
            (GFX_MAX_AA_INDEX >= 3 && !strcmp(value, "8")))
            MenuSet(k, value);
        return;
    case K_REFRESH:
        i = atoi(value);
        for (r = 0; r < 4; r++)
            if (kRefresh[r] == i) {
                snprintf(gMenu[k], sizeof gMenu[k], "%d", i);
                gMenuSet[k] = 1;
                return;
            }
        return;
    case K_LAMPS:
        i = atoi(value);
        if (i >= 1 && i <= 72) {
            snprintf(gMenu[k], sizeof gMenu[k], "%d", i);
            gMenuSet[k] = 1;
        }
        return;
    case K_MIRROR:
        for (i = 0; i < 3; i++)
            if (StrEqI(value, kMirror[i])) { MenuSet(k, kMirror[i]); return; }
        return;
    case K_TEXTURE:
        for (i = 0; i < 2; i++)
            if (StrEqI(value, kTexture[i])) { MenuSet(k, kTexture[i]); return; }
        return;
    case K_SHARPEN:
        d = strtod(value, &end);
        if (end == value || *end || !isfinite(d) || d < 0.0 || d > 1.0) return;
        MenuSetF(k, d);
        return;
    case K_EYE_SCALE: case K_SATURATION: case K_CONTRAST:
        d = strtod(value, &end);
        if (end == value || *end || !isfinite(d)) return;
        if (k == K_EYE_SCALE ? (d < 0.5 || d > GFX_MAX_PCT / 100.0)
                             : (d < 0.5 || d > 2.0))
            return;
        MenuSetF(k, d);
        return;
    }
}

int GraphicsSettingsLoad(const char *path) {
    FILE *f = fopen(path, "rb");
    char line[128];
    int preset = PRESET_CUSTOM;
    int savedCustom = 0;
    char pending[K_COUNT][32];
    int havePending[K_COUNT] = {0};
    int k;

    if (!f) {
        gGeneration++;
        return 1;
    }
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *key = line, *val, *e;
        if (!eq) continue;
        *eq = 0;
        val = eq + 1;
        while (*key == ' ' || *key == '\t') key++;
        e = key + strlen(key);
        while (e > key && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        while (*val == ' ' || *val == '\t') val++;
        e = val + strlen(val);
        while (e > val && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                           e[-1] == '\n')) *--e = 0;
        if (!strcmp(key, "preset")) {
            preset = PresetFromName(val);
            savedCustom = StrEqI(val, "CUSTOM");
        } else if ((k = KeyIndex(key)) >= 0) {
            snprintf(pending[k], sizeof pending[k], "%s", val);
            havePending[k] = 1;
        }
    }
    fclose(f);

    memset(gMenu, 0, sizeof gMenu);
    memset(gMenuSet, 0, sizeof gMenuSet);
    /* The saved preset is the fallback for any key whose value is missing or
     * unreadable. */
    gForceCustom = 0;
    gPresetChosen = preset != PRESET_CUSTOM;
    if (preset != PRESET_CUSTOM) ApplyPreset(preset);
    for (k = 0; k < K_COUNT; k++)
        if (havePending[k]) StoreChecked(k, pending[k]);
    gForceCustom = savedCustom;
    gGeneration++;
    return 1;
}

int GraphicsSettingsSave(const char *path) {
    FILE *f = fopen(path, "wb");
    int k, p = CurrentPreset();
    if (!f) return 0;
    /* Only a preset the player chose is written; otherwise the line is
     * absent so the ini still decides the preset's keys. */
    if (gForceCustom || (gPresetChosen && p == PRESET_CUSTOM))
        fprintf(f, "preset = CUSTOM\n");
    else if (gPresetChosen)
        fprintf(f, "preset = %s\n", kPresetNames[p]);
    for (k = 0; k < K_COUNT; k++)
        if (gMenuSet[k]) fprintf(f, "%s = %s\n", kKeyNames[k], gMenu[k]);
    return fclose(f) == 0;
}

const char *GraphicsSettingGet(const char *key) {
    int k = KeyIndex(key);
    return k < 0 ? NULL : Effective(k);
}

static const char *const kRowNames[GFX_ROW_COUNT] = {
    "PRESET", "FOVEATION", "ANTI-ALIAS", "RESOLUTION", "TEXTURE DETAIL",
    "CAR LAMPS", "LAMP LIGHTING", "LAMP GLOWS", "SHADOWS", "MIRROR", "SHARPEN",
    "COLOUR", "FPS COUNTER", "REFRESH"
};

#define LABEL_WIDTH 24

static int Pct(void) { return (int)floor(EffectiveNumber(K_EYE_SCALE, 1.0) * 100.0 + 0.5); }

static int ColourIndex(void) {
    double s = EffectiveNumber(K_SATURATION, 1.0), c = EffectiveNumber(K_CONTRAST, 1.0);
    if (fabs(s - 1.0) < 0.005 && fabs(c - 1.0) < 0.005) return 0;
    if (fabs(s - VIVID_SATURATION) < 0.005 && fabs(c - VIVID_CONTRAST) < 0.005) return 1;
    return -1;
}

void GraphicsRowParts(GraphicsRow row, char *name, size_t nameSize,
                      char *valueText, size_t valueSize) {
    char value[24] = "";
    int i;
    switch (row) {
    case GFX_ROW_PRESET:
        i = CurrentPreset();
        snprintf(value, sizeof value, "%s", i < 0 ? "CUSTOM" : kPresetNames[i]);
        break;
    case GFX_ROW_FOVEATION:
        i = FoveationIndex();
        if (i < 0) snprintf(value, sizeof value, "CUSTOM");
        else {
            const char *s = kFoveation[i];
            int n;
            for (n = 0; s[n] && n < 15; n++)
                value[n] = (char)(s[n] - 32);
            value[n] = 0;
        }
        break;
    case GFX_ROW_ANTIALIAS:
        i = AntialiasValue();
        snprintf(value, sizeof value, "%s",
                 i == 0 ? "OFF" : i == 2 ? "2X" : i == 4 ? "4X" : "8X");
        break;
    case GFX_ROW_RESOLUTION:
        snprintf(value, sizeof value, "%d%%", Pct());
        break;
    case GFX_ROW_TEXTURE_DETAIL:
        i = NameIndex(K_TEXTURE, kTexture, 2);
        snprintf(value, sizeof value, "%s", i < 0 ? "CUSTOM" : kTextureLabel[i]);
        break;
    case GFX_ROW_CAR_LAMPS:
        i = LampsValue();
        if (i >= 72) snprintf(value, sizeof value, "ALL");
        else snprintf(value, sizeof value, "%d", i);
        break;
    case GFX_ROW_MIRROR:
        i = NameIndex(K_MIRROR, kMirror, 3);
        snprintf(value, sizeof value, "%s", i < 0 ? "CUSTOM" : kMirrorLabel[i]);
        break;
    case GFX_ROW_SHARPEN:
        i = SharpenIndex();
        snprintf(value, sizeof value, "%s", i < 0 ? "CUSTOM" : kSharpenLabel[i]);
        break;
    case GFX_ROW_LAMP_LIGHTING:
        snprintf(value, sizeof value, "%s", EffectiveBool(K_LAMP_LIGHTING) ? "ON" : "OFF");
        break;
    case GFX_ROW_LAMP_GLOWS:
        snprintf(value, sizeof value, "%s", EffectiveBool(K_LAMP_GLOWS) ? "ON" : "OFF");
        break;
    case GFX_ROW_SHADOWS:
        snprintf(value, sizeof value, "%s", EffectiveBool(K_SHADOWS) ? "ON" : "OFF");
        break;
    case GFX_ROW_FPS_COUNTER:
        snprintf(value, sizeof value, "%s", EffectiveBool(K_FPS) ? "ON" : "OFF");
        break;
    case GFX_ROW_REFRESH:
        snprintf(value, sizeof value, "%d HZ", (int)EffectiveNumber(K_REFRESH, 72));
        break;
    case GFX_ROW_COLOUR:
        i = ColourIndex();
        snprintf(value, sizeof value, "%s", i == 0 ? "STANDARD" : i == 1 ? "VIVID" : "CUSTOM");
        break;
    default:
        break;
    }
    if (row < 0 || row >= GFX_ROW_COUNT) {
        if (nameSize) name[0] = 0;
        if (valueSize) valueText[0] = 0;
        return;
    }
    snprintf(name, nameSize, "%s", kRowNames[row]);
    snprintf(valueText, valueSize, "%s", value);
}

void GraphicsRowLabel(GraphicsRow row, char *text, size_t size) {
    char name[24];
    char value[24];

    GraphicsRowParts(row, name, sizeof name, value, sizeof value);
    if (row < 0 || row >= GFX_ROW_COUNT) {
        if (size) text[0] = 0;
        return;
    }
    snprintf(text, size, "%-*s%*s", (int)(LABEL_WIDTH - strlen(value)), name,
             (int)strlen(value), value);
}

static int Step(int direction) { return direction < 0 ? -1 : 1; }

static void AdjustIndexed(int k, const char *const *names, int count, int cur,
                          int direction) {
    int next = cur < 0 ? 0 : cur + Step(direction);
    if (next < 0) next = 0;
    if (next >= count) next = count - 1;
    MenuSet(k, names[next]);
}

static void AdjustBool(int k, int direction) {
    MenuSet(k, Bool(direction > 0));
}

/* The effective values a preset covers, packed for a before/after compare. */
static unsigned PresetRowsSignature(void) {
    return (unsigned)(FoveationIndex() + 1) | (unsigned)AntialiasValue() << 4 |
           (unsigned)EffectiveBool(K_LAMP_LIGHTING) << 8 |
           (unsigned)EffectiveBool(K_LAMP_GLOWS) << 9 |
           (unsigned)EffectiveBool(K_SHADOWS) << 10 |
           (unsigned)LampsValue() << 11 |
           (unsigned)(NameIndex(K_MIRROR, kMirror, 3) + 1) << 19 |
           (unsigned)(NameIndex(K_TEXTURE, kTexture, 2) + 1) << 21;
}

void GraphicsRowAdjust(GraphicsRow row, int direction) {
    int i;
    unsigned before = PresetRowsSignature();
    switch (row) {
    case GFX_ROW_PRESET:
        i = CurrentPreset();
        /* CUSTOM enters the list from either end: right to LOW, left to ULTRA. */
        if (i < 0) i = direction < 0 ? PRESET_ULTRA : PRESET_LOW;
        else i += Step(direction);
        if (i < 0) i = 0;
        if (i >= PRESET_COUNT) i = PRESET_COUNT - 1;
        ApplyPreset(i);
        gForceCustom = 0;
        gPresetChosen = 1;
        break;
    case GFX_ROW_FOVEATION:
        AdjustIndexed(K_FOVEATION, kFoveation, 4, FoveationIndex(), direction);
        break;
    case GFX_ROW_ANTIALIAS:
        i = AntialiasValue();
        i = i == 0 ? 0 : i == 2 ? 1 : i == 4 ? 2 : 3;
        i += Step(direction);
        if (i < 0) i = 0;
        if (i > GFX_MAX_AA_INDEX) i = GFX_MAX_AA_INDEX;
        MenuSet(K_MSAA, Bool(i != 0));
        if (i) MenuSet(K_SAMPLES, i == 1 ? "2" : i == 2 ? "4" : "8"); /* 1 = 2X, 2 = 4X, 3 = 8X */
        break;
    case GFX_ROW_RESOLUTION: {
        int pct = Pct();
        if (direction > 0) pct = (pct / 5 + 1) * 5;
        else pct = pct % 5 ? (pct / 5) * 5 : pct - 5;
        if (pct < 70) pct = 70;
        if (pct > GFX_MAX_PCT) pct = GFX_MAX_PCT;
        MenuSetF(K_EYE_SCALE, pct / 100.0);
        break;
    }
    case GFX_ROW_TEXTURE_DETAIL:
        AdjustIndexed(K_TEXTURE, kTexture, 2, NameIndex(K_TEXTURE, kTexture, 2), direction);
        break;
    case GFX_ROW_MIRROR:
        AdjustIndexed(K_MIRROR, kMirror, 3, NameIndex(K_MIRROR, kMirror, 3), direction);
        break;
    case GFX_ROW_CAR_LAMPS: {
        int n = LampsValue(), pick = -1;
        if (direction > 0) {
            for (i = 4; i >= 0; i--) if (kLamps[i] > n) pick = i;
            if (pick < 0) pick = 4;
        } else {
            for (i = 0; i < 5; i++) if (kLamps[i] < n) pick = i;
            if (pick < 0) pick = 0;
        }
        snprintf(gMenu[K_LAMPS], sizeof gMenu[K_LAMPS], "%d", kLamps[pick]);
        gMenuSet[K_LAMPS] = 1;
        break;
    }
    case GFX_ROW_SHARPEN: {
        double v = EffectiveNumber(K_SHARPEN, 0.0), pick = -1;
        if (direction > 0) {
            for (i = 2; i >= 0; i--) if (kSharpen[i] > v + 0.005) pick = i;
            if (pick < 0) pick = 2;
        } else {
            for (i = 0; i < 3; i++) if (kSharpen[i] < v - 0.005) pick = i;
            if (pick < 0) pick = 0;
        }
        MenuSetF(K_SHARPEN, kSharpen[(int)pick]);
        break;
    }
    case GFX_ROW_LAMP_LIGHTING: AdjustBool(K_LAMP_LIGHTING, direction); break;
    case GFX_ROW_LAMP_GLOWS: AdjustBool(K_LAMP_GLOWS, direction); break;
    case GFX_ROW_SHADOWS: AdjustBool(K_SHADOWS, direction); break;
    case GFX_ROW_FPS_COUNTER: AdjustBool(K_FPS, direction); break;
    case GFX_ROW_REFRESH: {
        int hz = (int)EffectiveNumber(K_REFRESH, 72), n = -1;
        if (direction > 0) {
            for (i = 3; i >= 0; i--) if (kRefresh[i] > hz) n = i;
            if (n < 0) n = 3;
        } else {
            for (i = 0; i < 4; i++) if (kRefresh[i] < hz) n = i;
            if (n < 0) n = 0;
        }
        snprintf(gMenu[K_REFRESH], sizeof gMenu[K_REFRESH], "%d", kRefresh[n]);
        gMenuSet[K_REFRESH] = 1;
        break;
    }
    case GFX_ROW_COLOUR:
        if (direction > 0) {
            MenuSetF(K_SATURATION, VIVID_SATURATION);
            MenuSetF(K_CONTRAST, VIVID_CONTRAST);
        } else {
            MenuSetF(K_SATURATION, 1.0);
            MenuSetF(K_CONTRAST, 1.0);
        }
        break;
    default:
        return;
    }
    if (row == GFX_ROW_FOVEATION || row == GFX_ROW_ANTIALIAS ||
        row == GFX_ROW_LAMP_LIGHTING || row == GFX_ROW_LAMP_GLOWS ||
        row == GFX_ROW_SHADOWS || row == GFX_ROW_TEXTURE_DETAIL ||
        row == GFX_ROW_CAR_LAMPS || row == GFX_ROW_MIRROR) {
        gPresetChosen = 0;
        if (PresetRowsSignature() != before) gForceCustom = 1;
    }
    gGeneration++;
}

const char *GraphicsRowNote(GraphicsRow row) {
    if (row == GFX_ROW_FPS_COUNTER) return "APPLIES NOW";
    return row == GFX_ROW_RESOLUTION || row == GFX_ROW_REFRESH
               ? "RESTART TO APPLY" : "APPLIES FROM NEXT RACE";
}

unsigned GraphicsSettingsGeneration(void) { return gGeneration; }

void GraphicsSettingsSnapshotTake(GraphicsSettingsSnapshot *out) {
    memcpy(out->menu, gMenu, sizeof gMenu);
    memcpy(out->menuSet, gMenuSet, sizeof gMenuSet);
    out->forceCustom = gForceCustom;
    out->presetChosen = gPresetChosen;
}

void GraphicsSettingsSnapshotRestore(const GraphicsSettingsSnapshot *in) {
    int changed = memcmp(gMenu, in->menu, sizeof gMenu) != 0 ||
                  memcmp(gMenuSet, in->menuSet, sizeof gMenuSet) != 0 ||
                  gForceCustom != in->forceCustom ||
                  gPresetChosen != in->presetChosen;
    memcpy(gMenu, in->menu, sizeof gMenu);
    memcpy(gMenuSet, in->menuSet, sizeof gMenuSet);
    gForceCustom = in->forceCustom;
    gPresetChosen = in->presetChosen;
    if (changed) gGeneration++;
}
