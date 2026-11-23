#include "common.h"
#include "game/frontend_internal.h"
#include "game/menu.h"
#include "game/menu_internal.h"
#include "game/render_internal.h"
#include "game/render_state.h"
#include "graphics_settings.h"
#include "menu_label_font.h"

#include <stdio.h>
#include <string.h>

s32 g_GameMode;
static GameSceneId s_exitScene = GAME_SCENE_BOOT_LOGO;
static s32 s_returnSelection = -1;
void StartOptionMenuExit(GameSceneId scene) { s_exitScene = scene; }
void SetTitleMenuReturnSelection(TitleMenuItem item) {
    s_returnSelection = item;
}
u16 g_PadPressed;
GameRenderState g_RenderState;
static GameFrameContext s_frame;
GameFrameContext *g_DrawBuffer = &s_frame;

/* Font stub: on, every text is a 32x16 block; off, the page falls back to 8x8. */
static int s_fontOn;
static int s_fontCount;
static char s_fontText[64][48];
static int s_fontSelected[64];
static int s_emitCount;
static s32 s_emitY[64];
static s32 s_emitScale[64];
static uint8_t s_pixels[32 * 16 * 3];

const uint8_t *MenuFontTextPixels(const char *text, int selected, int *width,
                                  int *height) {
    if (!s_fontOn) return NULL;
    if (s_fontCount < 64) {
        snprintf(s_fontText[s_fontCount], sizeof(s_fontText[0]), "%s", text);
        s_fontSelected[s_fontCount++] = selected;
    }
    *width = 32;
    *height = MENU_LABEL_HEIGHT;
    return s_pixels;
}
static int s_gapCount;
u8 *AddTilePrim(GameOrderingTableEntry *ot, u8 *packet, s32 x, s32 y,
                s32 width, s32 height, s32 red, s32 green, s32 blue) {
    (void)ot;
    (void)x;
    (void)y;
    (void)red;
    (void)green;
    (void)blue;
    if (width > 0 && height == 2 * MENU_LABEL_HEIGHT) s_gapCount++;
    return packet;
}
u8 *EmitFontPixelsScaled(GameOrderingTableEntry *ot, u8 *packet, s32 x, s32 y,
                         s32 width, s32 height, const uint8_t *pixels,
                         s32 rowScale) {
    (void)ot;
    (void)x;
    (void)width;
    (void)height;
    (void)pixels;
    if (s_emitCount < 64) {
        s_emitY[s_emitCount] = y;
        s_emitScale[s_emitCount++] = rowScale;
    }
    return packet;
}

static s32 s_lastCue;
static s32 s_arrowY;
static s32 s_textCount;
static char s_text[16][64];
static s32 s_textY[16];
static const char *s_savePath;

void PlaySoundCue(s32 cue) { s_lastCue = cue; }
void DrawMenuCursorArrow(s32 x, s32 y) {
    (void)x;
    s_arrowY = y;
}
void DrawText8x8(s32 x, s32 y, const char *text, s32 clutIndex) {
    (void)x;
    (void)clutIndex;
    if (s_textCount < 16) {
        snprintf(s_text[s_textCount], sizeof(s_text[0]), "%s", text);
        s_textY[s_textCount++] = y;
    }
}
int PlatformUserConfigPath(const char *name, char *path, size_t pathSize) {
    (void)name;
    snprintf(path, pathSize, "%s", s_savePath);
    return 1;
}

static const char *NoBase(const char *key) {
    (void)key;
    return NULL;
}

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "check failed at line %d: %s\n", __LINE__,         \
                    #condition);                                               \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static void Press(u16 pad) {
    g_PadPressed = pad;
    UpdateGraphicsOptionMenu();
    g_PadPressed = 0;
}

int main(int argc, char **argv) {
    char saved[512] = "";
    FILE *file;
    static u8 packets[256];

    s_savePath = argc > 1 ? argv[1] : "graphics_option_menu_test.out";
    remove(s_savePath);
    g_RenderState.draw.packetCursor = packets;
    GraphicsSettingsInit(NoBase, "quest3");
    g_GameMode = OPTION_MODE_GRAPHICS;
    EnterGraphicsOptionMenu();

    /* Every row, EXIT and the footer note are drawn. */
    UpdateGraphicsOptionMenu();
    CHECK(s_textCount == GFX_ROW_COUNT + 2);
    CHECK(strncmp(s_text[0], "PRESET", 6) == 0);
    CHECK(strncmp(s_text[GFX_ROW_COUNT], "EXIT", 4) == 0);
    CHECK(strcmp(s_text[GFX_ROW_COUNT + 1], GraphicsRowNote(GFX_ROW_PRESET)) == 0);
    CHECK(strchr(s_text[0], '<') == NULL);

    Press(PAD_DOWN);
    CHECK(s_lastCue == 1);
    Press(PAD_DOWN);
    s_textCount = 0;
    UpdateGraphicsOptionMenu();
    CHECK(strcmp(s_text[GFX_ROW_COUNT + 1], GraphicsRowNote(GFX_ROW_ANTIALIAS)) == 0);
    CHECK(s_arrowY == s_textY[GFX_ROW_ANTIALIAS] - 6);

    /* List mode: left/right do nothing; confirm enters edit, changing nothing. */
    CHECK(strcmp(GraphicsSettingGet("xr.msaa"), "false") == 0);
    Press(PAD_RIGHT);
    CHECK(strcmp(GraphicsSettingGet("xr.msaa"), "false") == 0);
    s_lastCue = 0;
    Press(PAD_CONFIRM);
    CHECK(s_lastCue == 2);
    CHECK(strcmp(GraphicsSettingGet("xr.msaa"), "false") == 0);
    CHECK(s_exitScene == GAME_SCENE_BOOT_LOGO);
    /* The edited row shows arrows; the others do not. */
    s_textCount = 0;
    UpdateGraphicsOptionMenu();
    CHECK(strstr(s_text[GFX_ROW_ANTIALIAS], "< OFF >") != NULL);
    CHECK(strchr(s_text[GFX_ROW_FOVEATION], '<') == NULL);

    /* Edit mode ignores up/down. */
    s_lastCue = 0;
    Press(PAD_DOWN);
    Press(PAD_UP);
    CHECK(s_lastCue == 0);
    s_textCount = 0;
    UpdateGraphicsOptionMenu();
    CHECK(s_arrowY == s_textY[GFX_ROW_ANTIALIAS] - 6);

    /* Left/right change the value (cue 1); a clamped press is silent. */
    Press(PAD_RIGHT);
    CHECK(s_lastCue == 1);
    CHECK(strcmp(GraphicsSettingGet("xr.msaa"), "true") == 0);
    Press(PAD_LEFT);
    CHECK(strcmp(GraphicsSettingGet("xr.msaa"), "false") == 0);
    s_lastCue = 0;
    Press(PAD_LEFT);
    CHECK(s_lastCue == 0);
    Press(PAD_RIGHT);
    /* Confirm keeps the change and returns to list mode (arrows gone). */
    Press(PAD_CONFIRM);
    CHECK(s_lastCue == 2);
    CHECK(strcmp(GraphicsSettingGet("xr.msaa"), "true") == 0);
    s_textCount = 0;
    UpdateGraphicsOptionMenu();
    CHECK(strchr(s_text[GFX_ROW_ANTIALIAS], '<') == NULL);
    s_lastCue = 0;
    Press(PAD_RIGHT);
    CHECK(s_lastCue == 0);
    CHECK(s_exitScene == GAME_SCENE_BOOT_LOGO);

    /* Cancel in edit mode restores the value and the PRESET label. */
    {
        char before[64];
        char after[64];

        GraphicsRowLabel(GFX_ROW_PRESET, before, sizeof(before));
        CHECK(strstr(before, "CUSTOM") != NULL); /* 2X is a hand change */
        Press(PAD_UP);
        Press(PAD_UP);
        Press(PAD_CONFIRM);
        Press(PAD_RIGHT);
        CHECK(strcmp(GraphicsSettingGet("xr.msaa"), "false") == 0);
        GraphicsRowLabel(GFX_ROW_PRESET, after, sizeof(after));
        CHECK(strcmp(before, after) != 0);
        Press(PAD_CANCEL);
        CHECK(s_lastCue == 3);
        CHECK(s_exitScene == GAME_SCENE_BOOT_LOGO);
        CHECK(strcmp(GraphicsSettingGet("xr.msaa"), "true") == 0);
        GraphicsRowLabel(GFX_ROW_PRESET, after, sizeof(after));
        CHECK(strcmp(before, after) == 0);
        /* Back in list mode: up from PRESET wraps to EXIT. */
        Press(PAD_UP);
        s_textCount = 0;
        UpdateGraphicsOptionMenu();
        CHECK(s_arrowY == s_textY[GFX_ROW_COUNT] - 6);
        CHECK(strcmp(s_text[GFX_ROW_COUNT + 1], "SAVE AND GO BACK") == 0);
    }

    /* Confirm on EXIT leaves (cue 2), saving, title returns to GRAPHICS. */
    Press(PAD_CONFIRM);
    CHECK(s_lastCue == 2);
    CHECK(s_exitScene == GAME_SCENE_ENTER_FRONTEND);
    CHECK(s_returnSelection == TITLE_MENU_GRAPHICS);
    file = fopen(s_savePath, "rb");
    CHECK(file != NULL);
    fclose(file);
    remove(s_savePath);

    /* Cancel in list mode leaves too (cue 3), saving. */
    s_exitScene = GAME_SCENE_BOOT_LOGO;
    s_returnSelection = -1;
    Press(PAD_DOWN);
    Press(PAD_CANCEL);
    CHECK(s_lastCue == 3);
    /* Opened from the title: leave the Options scene, title lands on GRAPHICS. */
    CHECK(s_exitScene == GAME_SCENE_ENTER_FRONTEND);
    CHECK(s_returnSelection == TITLE_MENU_GRAPHICS);
    file = fopen(s_savePath, "rb");
    CHECK(file != NULL);
    fread(saved, 1, sizeof(saved) - 1, file);
    fclose(file);
    CHECK(strstr(saved, "xr.msaa = true") != NULL);
    remove(s_savePath);

    /* The scrolling window keeps the cursor inside it. */
    CHECK(GraphicsPageFirstVisibleRow(0, 0, 8, 14) == 0);
    CHECK(GraphicsPageFirstVisibleRow(7, 0, 8, 14) == 0);
    CHECK(GraphicsPageFirstVisibleRow(8, 0, 8, 14) == 1);
    CHECK(GraphicsPageFirstVisibleRow(12, 0, 8, 14) == 5);
    CHECK(GraphicsPageFirstVisibleRow(13, 0, 8, 14) == 6);
    CHECK(GraphicsPageFirstVisibleRow(4, 5, 8, 14) == 4);
    CHECK(GraphicsPageFirstVisibleRow(0, 5, 8, 14) == 0);
    CHECK(GraphicsPageFirstVisibleRow(9, 3, 8, 14) == 3);
    CHECK(GraphicsPageFirstVisibleRow(3, 3, 8, 14) == 3);
    CHECK(GraphicsPageFirstVisibleRow(2, 99, 8, 14) == 2);
    CHECK(GraphicsPageFirstVisibleRow(12, -4, 8, 14) == 5);
    CHECK(GraphicsPageFirstVisibleRow(0, 3, 20, 14) == 0);

    /* Font page: a window of rows, arrows for what is hidden, the footer. */
    s_fontOn = 1;
    GraphicsSettingsInit(NoBase, "quest3");
    EnterGraphicsOptionMenu();
    s_fontCount = 0;
    s_emitCount = 0;
    s_textCount = 0;
    UpdateGraphicsOptionMenu();
    CHECK(s_textCount == 0);
    {
        int i;
        int sawPreset = 0, sawRefresh = 0, sawUp = 0, sawDown = 0, sawNote = 0;
        int selectedTexts = 0;

        for (i = 0; i < s_fontCount; i++) {
            sawPreset |= strcmp(s_fontText[i], "PRESET") == 0;
            sawRefresh |= strcmp(s_fontText[i], "REFRESH") == 0;
            sawUp |= strcmp(s_fontText[i], "^") == 0;
            sawDown |= strcmp(s_fontText[i], "v") == 0;
            sawNote |= strcmp(s_fontText[i], GraphicsRowNote(GFX_ROW_PRESET)) == 0;
            selectedTexts += s_fontSelected[i];
        }
        CHECK(sawPreset && !sawRefresh);
        CHECK(!sawUp && sawDown && sawNote);
        CHECK(selectedTexts == 2);
        /* 8 rows x (name, value) + down arrow + footer. */
        CHECK(s_emitCount == GRAPHICS_PAGE_WINDOW_ROWS * 2 + 2);
        /* Each row's name and value plates are joined by a bar. */
        CHECK(s_gapCount == GRAPHICS_PAGE_WINDOW_ROWS);
        for (i = 0; i < s_emitCount; i++) {
            CHECK(s_emitScale[i] == 2);
            CHECK(s_emitY[i] >= 0 && s_emitY[i] + 2 * MENU_LABEL_HEIGHT <= 480);
        }
    }
    /* Wrap UP from PRESET: the window scrolls so REFRESH is visible. */
    Press(PAD_UP);
    s_fontCount = 0;
    s_emitCount = 0;
    UpdateGraphicsOptionMenu();
    {
        int i;
        int sawPreset = 0, sawRefresh = 0, sawUp = 0, sawDown = 0, sawExit = 0;

        for (i = 0; i < s_fontCount; i++) {
            sawPreset |= strcmp(s_fontText[i], "PRESET") == 0;
            sawRefresh |= strcmp(s_fontText[i], "REFRESH") == 0;
            sawUp |= strcmp(s_fontText[i], "^") == 0;
            sawDown |= strcmp(s_fontText[i], "v") == 0;
            sawExit |= strcmp(s_fontText[i], "EXIT") == 0;
        }
        CHECK(sawRefresh && !sawPreset && sawExit);
        CHECK(sawUp && !sawDown);
    }
    /* Editing PRESET in the font page: its value is drawn selected, in arrows. */
    Press(PAD_DOWN);
    Press(PAD_CONFIRM);
    s_fontCount = 0;
    UpdateGraphicsOptionMenu();
    {
        int i;
        int sawArrows = 0;

        for (i = 0; i < s_fontCount; i++) {
            if (strcmp(s_fontText[i], "< HIGH >") == 0 && s_fontSelected[i]) {
                sawArrows = 1;
            }
        }
        CHECK(sawArrows);
    }
    s_fontOn = 0;

    puts("graphics option menu navigates, adjusts and saves");
    return 0;
}
