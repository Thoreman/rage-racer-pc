#include <stdio.h>
#include <string.h>

#include "graphics_settings.h"

static int failures;

static void ExpectStr(const char *what, const char *got, const char *want) {
    if (!got || strcmp(got, want)) {
        printf("%s: expected \"%s\", got \"%s\"\n", what, want, got ? got : "(null)");
        failures++;
    }
}

static void ExpectInt(const char *what, int got, int want) {
    if (got != want) {
        printf("%s: expected %d, got %d\n", what, want, got);
        failures++;
    }
}

static void ExpectContains(const char *what, const char *got, const char *want) {
    if (!strstr(got, want)) {
        printf("%s: expected to contain \"%s\", got \"%s\"\n", what, want, got);
        failures++;
    }
}

/* Fake ini-then-profile lookup backed by a small table. */
static const char *gBaseKeys[8];
static const char *gBaseValues[8];
static int gBaseCount;

static void BaseClear(void) { gBaseCount = 0; }
static void BaseAdd(const char *k, const char *v) {
    gBaseKeys[gBaseCount] = k;
    gBaseValues[gBaseCount++] = v;
}
static const char *FakeBase(const char *key) {
    int i;
    for (i = 0; i < gBaseCount; i++)
        if (!strcmp(gBaseKeys[i], key)) return gBaseValues[i];
    return NULL;
}

static void Label(GraphicsRow row, char *out, size_t size) {
    GraphicsRowLabel(row, out, size);
}

static void TestRowParts(void) {
    char name[32];
    char value[32];
    char joined[64];
    int row;

    GraphicsSettingsInit(FakeBase, "quest3");
    GraphicsRowParts(GFX_ROW_PRESET, name, sizeof name, value, sizeof value);
    ExpectStr("preset name", name, "PRESET");
    ExpectStr("preset value", value, "HIGH");
    GraphicsRowParts(GFX_ROW_TEXTURE_DETAIL, name, sizeof name, value, sizeof value);
    ExpectStr("texture name", name, "TEXTURE DETAIL");
    for (row = 0; row < GFX_ROW_COUNT; row++) {
        GraphicsRowParts((GraphicsRow)row, name, sizeof name, value, sizeof value);
        ExpectInt("name present", name[0] != 0, 1);
        ExpectInt("value present", value[0] != 0, 1);
        GraphicsRowLabel((GraphicsRow)row, joined, sizeof joined);
        ExpectContains("label holds name", joined, name);
        ExpectContains("label holds value", joined, value);
    }
    GraphicsRowParts(GFX_ROW_COUNT, name, sizeof name, value, sizeof value);
    ExpectStr("bad row name", name, "");
    ExpectStr("bad row value", value, "");
}

static void TestDefaultsFollowProfile(void) {
    char text[64];
    BaseClear();
    GraphicsSettingsInit(FakeBase, "quest2");
    ExpectStr("quest2 lamp lighting", GraphicsSettingGet("xr.lamp_lighting"), "false");
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("quest2 preset", text, "LOW");
    GraphicsSettingsInit(FakeBase, "quest3");
    ExpectStr("quest3 lamp lighting", GraphicsSettingGet("xr.lamp_lighting"), "false");
    ExpectStr("quest3 msaa", GraphicsSettingGet("xr.msaa"), "true");
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("quest3 preset", text, "HIGH");
    GraphicsSettingsInit(FakeBase, "unknown");
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("unknown preset", text, "HIGH");
    GraphicsSettingsInit(FakeBase, "questpro");
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("questpro preset", text, "MEDIUM");
    ExpectInt("not owned", GraphicsSettingGet("xr.nothing") == NULL, 1);
}

static void TestIniBeatsDefaultPreset(void) {
    char text[64];
    BaseClear();
    BaseAdd("xr.foveation", "medium");
    GraphicsSettingsInit(FakeBase, "quest3");
    ExpectStr("ini foveation", GraphicsSettingGet("xr.foveation"), "medium");
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("ini makes custom", text, "CUSTOM");
}

static void TestPresetWritesAllKeys(void) {
    char text[64];
    const char *path = "graphics_settings_test_file.txt";
    BaseClear();
    BaseAdd("xr.lamp_glows", "true");
    GraphicsSettingsInit(FakeBase, "quest3");
    /* The base ini makes the starting point CUSTOM, which sits past ULTRA. */
    GraphicsRowAdjust(GFX_ROW_PRESET, -1);
    ExpectStr("ultra foveation", GraphicsSettingGet("xr.foveation"), "medium");
    ExpectStr("ultra msaa", GraphicsSettingGet("xr.msaa"), "true");
    ExpectStr("ultra samples", GraphicsSettingGet("xr.msaa_samples"), "2");
    GraphicsRowAdjust(GFX_ROW_PRESET, -1);
    GraphicsRowAdjust(GFX_ROW_PRESET, -1);
    GraphicsRowAdjust(GFX_ROW_PRESET, -1);
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("walked to low", text, "LOW");
    ExpectStr("low msaa beats base", GraphicsSettingGet("xr.msaa"), "false");
    ExpectInt("save", GraphicsSettingsSave(path), 1);
    GraphicsSettingsInit(FakeBase, "quest3");
    ExpectInt("load", GraphicsSettingsLoad(path), 1);
    ExpectStr("reload msaa", GraphicsSettingGet("xr.msaa"), "false");
    ExpectStr("reload lighting", GraphicsSettingGet("xr.lamp_lighting"), "false");
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("reload preset", text, "LOW");
    remove(path);
}

static void TestRowChangeMakesCustom(void) {
    char text[64];
    BaseClear();
    GraphicsSettingsInit(FakeBase, "quest3");
    GraphicsRowAdjust(GFX_ROW_LAMP_GLOWS, +1);
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("custom after row change", text, "CUSTOM");
}

static void TestOddValueShownAndSnaps(void) {
    char text[64];
    BaseClear();
    BaseAdd("xr.eye_scale", "0.72");
    GraphicsSettingsInit(FakeBase, "quest3");
    Label(GFX_ROW_RESOLUTION, text, sizeof text);
    ExpectContains("odd resolution", text, "72%");
    GraphicsRowAdjust(GFX_ROW_RESOLUTION, +1);
    ExpectStr("snap up", GraphicsSettingGet("xr.eye_scale"), "0.75");
    GraphicsSettingsInit(FakeBase, "quest3");
    GraphicsRowAdjust(GFX_ROW_RESOLUTION, -1);
    ExpectStr("snap down", GraphicsSettingGet("xr.eye_scale"), "0.70");
}

static void TestColourRow(void) {
    BaseClear();
    GraphicsSettingsInit(FakeBase, "quest3");
    GraphicsRowAdjust(GFX_ROW_COLOUR, +1);
    ExpectStr("vivid saturation", GraphicsSettingGet("xr.saturation"), "1.25");
    ExpectStr("vivid contrast", GraphicsSettingGet("xr.contrast"), "1.08");
    GraphicsRowAdjust(GFX_ROW_COLOUR, -1);
    ExpectStr("standard saturation", GraphicsSettingGet("xr.saturation"), "1.00");
    ExpectStr("standard contrast", GraphicsSettingGet("xr.contrast"), "1.00");
}

static void TestNotes(void) {
    ExpectStr("resolution note", GraphicsRowNote(GFX_ROW_RESOLUTION), "RESTART TO APPLY");
    ExpectStr("refresh note", GraphicsRowNote(GFX_ROW_REFRESH), "RESTART TO APPLY");
    ExpectStr("foveation note", GraphicsRowNote(GFX_ROW_FOVEATION), "APPLIES FROM NEXT RACE");
}

static void TestClampsAtEnds(void) {
    BaseClear();
    BaseAdd("xr.eye_scale", "1.00");
    GraphicsSettingsInit(FakeBase, "quest3");
    GraphicsRowAdjust(GFX_ROW_RESOLUTION, +1);
    ExpectStr("resolution top", GraphicsSettingGet("xr.eye_scale"), "1.00");
    GraphicsRowAdjust(GFX_ROW_FOVEATION, -1);
    GraphicsRowAdjust(GFX_ROW_FOVEATION, -1);
    GraphicsRowAdjust(GFX_ROW_FOVEATION, -1);
    GraphicsRowAdjust(GFX_ROW_FOVEATION, -1);
    GraphicsRowAdjust(GFX_ROW_FOVEATION, -1);
    ExpectStr("foveation floor", GraphicsSettingGet("xr.foveation"), "off");
}

static void WriteText(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
}

static void TestCorruptFileLoads(void) {
    const char *path = "graphics_settings_corrupt_file.txt";
    BaseClear();
    GraphicsSettingsInit(FakeBase, "quest3");
    WriteText(path, "preset = ULTRA\nxr.foveation = banana\nnonsense\nxr.shadows = false");
    ExpectInt("corrupt load", GraphicsSettingsLoad(path), 1);
    ExpectStr("shadows kept", GraphicsSettingGet("xr.shadows"), "false");
    ExpectStr("foveation falls back to ultra", GraphicsSettingGet("xr.foveation"), "medium");
    remove(path);
    GraphicsSettingsInit(FakeBase, "quest3");
    ExpectInt("missing file", GraphicsSettingsLoad("graphics_settings_no_such_file.txt"), 1);
}

static void TestSaveFailureKeepsState(void) {
    BaseClear();
    GraphicsSettingsInit(FakeBase, "quest3");
    GraphicsRowAdjust(GFX_ROW_SHADOWS, -1);
    ExpectInt("save fails", GraphicsSettingsSave("no_such_dir_for_gfx/file.txt"), 0);
    ExpectStr("shadows unchanged", GraphicsSettingGet("xr.shadows"), "false");
}

static void TestGenerationBumps(void) {
    const char *path = "graphics_settings_generation_file.txt";
    unsigned g;
    BaseClear();
    GraphicsSettingsInit(FakeBase, "quest3");
    g = GraphicsSettingsGeneration();
    GraphicsRowAdjust(GFX_ROW_SHADOWS, -1);
    ExpectInt("adjust bumps", GraphicsSettingsGeneration() > g, 1);
    g = GraphicsSettingsGeneration();
    GraphicsSettingsSave(path);
    GraphicsSettingsLoad(path);
    ExpectInt("load bumps", GraphicsSettingsGeneration() > g, 1);
    remove(path);
}

static void TestCustomEntersPresetList(void) {
    char text[64];
    BaseClear();
    GraphicsSettingsInit(FakeBase, "quest3");
    GraphicsRowAdjust(GFX_ROW_LAMP_GLOWS, +1);
    GraphicsRowAdjust(GFX_ROW_PRESET, +1);
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("custom right enters at low", text, "LOW");
    GraphicsRowAdjust(GFX_ROW_FOVEATION, -1); /* LOW already has glows off */
    GraphicsRowAdjust(GFX_ROW_PRESET, -1);
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("custom left enters at ultra", text, "ULTRA");
}

static void TestNonPresetRowsKeepPreset(void) {
    char text[64];
    BaseClear();
    GraphicsSettingsInit(FakeBase, "quest3");
    GraphicsRowAdjust(GFX_ROW_COLOUR, +1);
    GraphicsRowAdjust(GFX_ROW_RESOLUTION, -1);
    GraphicsRowAdjust(GFX_ROW_REFRESH, +1);
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("colour/resolution/refresh keep preset", text, "HIGH");
}

static void TestUntouchedPresetNotPinned(void) {
    const char *path = "graphics_settings_nopin_file.txt";
    BaseClear();
    GraphicsSettingsInit(FakeBase, "quest3");
    GraphicsRowAdjust(GFX_ROW_COLOUR, +1);
    GraphicsSettingsSave(path);
    BaseClear();
    BaseAdd("xr.foveation", "medium");
    GraphicsSettingsInit(FakeBase, "quest3");
    GraphicsSettingsLoad(path);
    ExpectStr("ini foveation survives load", GraphicsSettingGet("xr.foveation"), "medium");
    remove(path);
}

static void TestSnapshotRestore(void) {
    char text[64];
    GraphicsSettingsSnapshot snap;
    unsigned gen;
    BaseClear();
    GraphicsSettingsInit(FakeBase, "quest3");
    GraphicsSettingsSnapshotTake(&snap);
    gen = GraphicsSettingsGeneration();
    GraphicsSettingsSnapshotRestore(&snap);
    ExpectInt("restore of unchanged state keeps generation",
           GraphicsSettingsGeneration() == gen, 1);
    GraphicsRowAdjust(GFX_ROW_SHADOWS, +1);
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("hand change reads custom", text, "CUSTOM");
    gen = GraphicsSettingsGeneration();
    GraphicsSettingsSnapshotRestore(&snap);
    ExpectInt("restore of a change bumps generation",
           GraphicsSettingsGeneration() != gen, 1);
    ExpectStr("shadows back", GraphicsSettingGet("xr.shadows"), "false");
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("preset back", text, "HIGH");
    GraphicsRowAdjust(GFX_ROW_PRESET, -1);
    GraphicsSettingsSnapshotTake(&snap);
    GraphicsRowAdjust(GFX_ROW_COLOUR, -1);
    GraphicsSettingsSnapshotRestore(&snap);
    Label(GFX_ROW_COLOUR, text, sizeof text);
    ExpectContains("colour back", text, "VIVID");
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("chosen preset kept", text, "MEDIUM");
}

static void TestClampedPressKeepsPreset(void) {
    char text[64];
    BaseClear();
    GraphicsSettingsInit(FakeBase, "quest3");
    GraphicsRowAdjust(GFX_ROW_PRESET, -1);
    GraphicsRowAdjust(GFX_ROW_PRESET, -1); /* HIGH -> LOW */
    GraphicsRowAdjust(GFX_ROW_LAMP_GLOWS, -1); /* already off: no change */
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("no-op press keeps preset", text, "LOW");
}

static void TestProfileMsaaDoesNotOutrankPreset(void) {
    /* The caller's base lookup carries no msaa; the default preset supplies it. */
    BaseClear();
    GraphicsSettingsInit(FakeBase, "quest3");
    ExpectStr("preset msaa", GraphicsSettingGet("xr.msaa"), "true");
    ExpectStr("preset samples", GraphicsSettingGet("xr.msaa_samples"), "2");
}

static void TestSamplesThreeShowsFourX(void) {
    char text[64];
    BaseClear();
    BaseAdd("xr.msaa", "true");
    BaseAdd("xr.msaa_samples", "3");
    GraphicsSettingsInit(FakeBase, "quest3");
    Label(GFX_ROW_ANTIALIAS, text, sizeof text);
    ExpectContains("3 samples shows 4X", text, "4X");
}

static void TestNewRowDefaults(void) {
    BaseClear();
    GraphicsSettingsInit(FakeBase, "quest3");
    ExpectStr("q3 lamps", GraphicsSettingGet("xr.max_spot_lights"), "4");
    ExpectStr("q3 mirror", GraphicsSettingGet("xr.mirror"), "half");
    ExpectStr("q3 texture", GraphicsSettingGet("xr.texture_detail"), "standard");
    ExpectStr("q3 sharpen", GraphicsSettingGet("xr.sharpen"), "0.70");
    ExpectStr("q3 saturation", GraphicsSettingGet("xr.saturation"), "1.25");
    ExpectStr("q3 contrast", GraphicsSettingGet("xr.contrast"), "1.08");
    ExpectStr("q3 fps", GraphicsSettingGet("xr.fps_display"), "false");
    GraphicsSettingsInit(FakeBase, "quest2");
    ExpectStr("q2 lamps", GraphicsSettingGet("xr.max_spot_lights"), "2");
    ExpectStr("q2 mirror", GraphicsSettingGet("xr.mirror"), "half");
    ExpectStr("q2 texture", GraphicsSettingGet("xr.texture_detail"), "standard");
    ExpectStr("q2 sharpen", GraphicsSettingGet("xr.sharpen"), "0.00");
    ExpectInt("q2 saturation unset", GraphicsSettingGet("xr.saturation") == NULL, 1);
}

static void TestUltraSetsNewRows(void) {
    char text[64];
    BaseClear();
    GraphicsSettingsInit(FakeBase, "quest3");
    GraphicsRowAdjust(GFX_ROW_PRESET, +1);
    ExpectStr("ultra lamps", GraphicsSettingGet("xr.max_spot_lights"), "8");
    ExpectStr("ultra mirror", GraphicsSettingGet("xr.mirror"), "full");
    ExpectStr("ultra texture", GraphicsSettingGet("xr.texture_detail"), "high");
    Label(GFX_ROW_MIRROR, text, sizeof text);
    ExpectContains("mirror label", text, "FULL");
    Label(GFX_ROW_CAR_LAMPS, text, sizeof text);
    ExpectContains("lamps label", text, "8");
}

static void TestCarLampsAll(void) {
    char text[64];
    BaseClear();
    BaseAdd("xr.max_spot_lights", "8");
    GraphicsSettingsInit(FakeBase, "quest3");
    GraphicsRowAdjust(GFX_ROW_CAR_LAMPS, +1);
    ExpectStr("all lamps", GraphicsSettingGet("xr.max_spot_lights"), "72");
    Label(GFX_ROW_CAR_LAMPS, text, sizeof text);
    ExpectContains("all label", text, "ALL");
}

static void TestNewPresetRowsMakeCustom(void) {
    char text[64];
    BaseClear();
    GraphicsSettingsInit(FakeBase, "quest3");
    GraphicsRowAdjust(GFX_ROW_MIRROR, +1);
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("mirror makes custom", text, "CUSTOM");
    GraphicsSettingsInit(FakeBase, "quest3");
    GraphicsRowAdjust(GFX_ROW_SHARPEN, -1);
    ExpectStr("sharpen low", GraphicsSettingGet("xr.sharpen"), "0.35");
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("sharpen keeps preset", text, "HIGH");
}

static void TestOddLampCount(void) {
    char text[64];
    BaseClear();
    BaseAdd("xr.max_spot_lights", "5");
    GraphicsSettingsInit(FakeBase, "quest3");
    Label(GFX_ROW_CAR_LAMPS, text, sizeof text);
    ExpectContains("odd lamps", text, "5");
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("odd lamps custom", text, "CUSTOM");
}

static void TestLampsOutOfRange(void) {
    char text[64];
    BaseClear();
    BaseAdd("xr.max_spot_lights", "1e30");
    GraphicsSettingsInit(FakeBase, "quest3");
    Label(GFX_ROW_CAR_LAMPS, text, sizeof text);
    ExpectContains("huge lamps label", text, "ALL");
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("huge lamps custom", text, "CUSTOM");
    BaseClear();
    BaseAdd("xr.max_spot_lights", "nan");
    GraphicsSettingsInit(FakeBase, "quest3");
    Label(GFX_ROW_CAR_LAMPS, text, sizeof text);
    ExpectContains("nan lamps default", text, "4");
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("nan lamps preset", text, "HIGH");
    BaseClear();
    BaseAdd("xr.max_spot_lights", "-5");
    GraphicsSettingsInit(FakeBase, "quest3");
    Label(GFX_ROW_CAR_LAMPS, text, sizeof text);
    ExpectContains("negative lamps", text, "0");
}

static void TestRoundOneFileLoads(void) {
    char text[64];
    const char *path = "graphics_settings_round1_file.txt";
    BaseClear();
    GraphicsSettingsInit(FakeBase, "quest3");
    WriteText(path, "preset = HIGH\nxr.foveation = high\nxr.msaa = false\n"
                    "xr.lamp_lighting = true\nxr.lamp_glows = true\nxr.shadows = true\n");
    ExpectInt("round one load", GraphicsSettingsLoad(path), 1);
    ExpectStr("r1 mirror", GraphicsSettingGet("xr.mirror"), "half");
    ExpectStr("r1 lamps", GraphicsSettingGet("xr.max_spot_lights"), "4");
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("r1 preset", text, "CUSTOM");
    remove(path);
}

static void TestNewNotes(void) {
    ExpectStr("lamps note", GraphicsRowNote(GFX_ROW_CAR_LAMPS), "APPLIES FROM NEXT RACE");
    ExpectStr("mirror note", GraphicsRowNote(GFX_ROW_MIRROR), "APPLIES FROM NEXT RACE");
    ExpectStr("texture note", GraphicsRowNote(GFX_ROW_TEXTURE_DETAIL), "APPLIES FROM NEXT RACE");
    ExpectStr("sharpen note", GraphicsRowNote(GFX_ROW_SHARPEN), "APPLIES FROM NEXT RACE");
}


static void TestFpsRow(void) {
    char text[64];
    const char *path = "graphics_settings_fps_file.txt";
    GraphicsSettingsSnapshot snap;
    unsigned gen;

    BaseClear();
    GraphicsSettingsInit(FakeBase, "quest3");
    ExpectInt("fps row before refresh", GFX_ROW_FPS_COUNTER + 1, GFX_ROW_REFRESH);
    ExpectInt("colour before fps", GFX_ROW_COLOUR + 1, GFX_ROW_FPS_COUNTER);
    ExpectInt("refresh last", GFX_ROW_REFRESH + 1, GFX_ROW_COUNT);
    Label(GFX_ROW_FPS_COUNTER, text, sizeof text);
    ExpectContains("fps name", text, "FPS COUNTER");
    ExpectContains("fps default off", text, "OFF");
    ExpectInt("fps default value not on", GraphicsSettingGet("xr.fps_display") == NULL ||
              !strcmp(GraphicsSettingGet("xr.fps_display"), "false"), 1);
    ExpectStr("fps note", GraphicsRowNote(GFX_ROW_FPS_COUNTER), "APPLIES NOW");
    ExpectStr("refresh note unchanged", GraphicsRowNote(GFX_ROW_REFRESH), "RESTART TO APPLY");

    GraphicsSettingsSnapshotTake(&snap);
    gen = GraphicsSettingsGeneration();
    GraphicsRowAdjust(GFX_ROW_FPS_COUNTER, +1);
    ExpectInt("generation bumps", GraphicsSettingsGeneration() != gen, 1);
    Label(GFX_ROW_FPS_COUNTER, text, sizeof text);
    ExpectContains("fps on", text, "ON");
    ExpectStr("fps key true", GraphicsSettingGet("xr.fps_display"), "true");
    Label(GFX_ROW_PRESET, text, sizeof text);
    ExpectContains("fps keeps preset", text, "HIGH");

    GraphicsSettingsSave(path);
    GraphicsSettingsInit(FakeBase, "quest3");
    ExpectInt("reset off", GraphicsSettingGet("xr.fps_display") == NULL ||
              !strcmp(GraphicsSettingGet("xr.fps_display"), "false"), 1);
    GraphicsSettingsLoad(path);
    ExpectStr("fps survives load", GraphicsSettingGet("xr.fps_display"), "true");
    remove(path);

    GraphicsRowAdjust(GFX_ROW_FPS_COUNTER, -1);
    ExpectStr("fps back off", GraphicsSettingGet("xr.fps_display"), "false");
    GraphicsRowAdjust(GFX_ROW_FPS_COUNTER, +1);
    GraphicsSettingsSnapshotRestore(&snap);
    ExpectInt("snapshot restores fps", GraphicsSettingGet("xr.fps_display") == NULL ||
              !strcmp(GraphicsSettingGet("xr.fps_display"), "false"), 1);
}

int main(void) {
    TestRowParts();
    TestDefaultsFollowProfile();
    TestIniBeatsDefaultPreset();
    TestPresetWritesAllKeys();
    TestRowChangeMakesCustom();
    TestOddValueShownAndSnaps();
    TestColourRow();
    TestNotes();
    TestClampsAtEnds();
    TestCorruptFileLoads();
    TestSaveFailureKeepsState();
    TestGenerationBumps();
    TestCustomEntersPresetList();
    TestNonPresetRowsKeepPreset();
    TestUntouchedPresetNotPinned();
    TestSnapshotRestore();
    TestClampedPressKeepsPreset();
    TestProfileMsaaDoesNotOutrankPreset();
    TestSamplesThreeShowsFourX();
    TestNewRowDefaults();
    TestUltraSetsNewRows();
    TestCarLampsAll();
    TestNewPresetRowsMakeCustom();
    TestOddLampCount();
    TestLampsOutOfRange();
    TestRoundOneFileLoads();
    TestNewNotes();
    TestFpsRow();
    if (failures) {
        printf("%d graphics settings assertion(s) failed\n", failures);
        return 1;
    }
    printf("graphics settings rows, presets, precedence and file behave\n");
    return 0;
}
