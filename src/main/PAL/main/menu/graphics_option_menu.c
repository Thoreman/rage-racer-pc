#include "game/audio.h"
#include "game/frontend_internal.h"
#include "game/menu.h"
#include "game/menu_internal.h"
#include "game/render.h"
#include "game/render_internal.h"
#include "game/prim.h"
#include "game/state.h"
#include "graphics_settings.h"
#include "menu_label_font.h"
#include "platform_paths.h"

#include <stdio.h>
#include <string.h>

/* OPTION_MODE_GRAPHICS: the VR graphics page, reached from the title menu only
 * when PortXrAvailable() (the Options root has no GRAPHICS row).
 *
 * The page is a 480-line scene whose pixels are half as tall as they are wide,
 * so the title buttons' face (16 source rows) is drawn two screen lines per
 * row: 32 lines tall, the same look as the title's 16-line buttons on its
 * 240-line screen. Eight of the fourteen rows (the settings plus EXIT) scroll
 * in a window (name left, value right, the
 * selected row in the selected style), an arrow above or below says there are
 * more, and the selected row's note sits in the footer:
 *
 *   y  20..52   up arrow   | rows y = 56 + 40 * k, k = 0..7 (32 tall, 8 gap)
 *   y 372..404  down arrow | last row ends at y 360
 *   y 416..448  footer     | 32 lines of margin left under it
 *
 * If the face cannot be read, every row is drawn as plain 8x8 text instead. */
enum {
    GRAPHICS_TEXT_X = 0x30,
    GRAPHICS_TOP = 0x94,
    GRAPHICS_ROW_HEIGHT = 0x12,
    GRAPHICS_NOTE_Y = GRAPHICS_TOP + (GFX_ROW_COUNT + 1) * GRAPHICS_ROW_HEIGHT + 8,
    GRAPHICS_CURSOR_X = 0x20,
    GRAPHICS_CURSOR_Y_OFFSET = -6,
    GRAPHICS_CLUT = 0x78CC,

    FONT_SCREEN_WIDTH = 320,
    FONT_CENTRE = -1,
    FONT_ROW_SCALE = 2,
    FONT_GLYPH_LINES = MENU_LABEL_HEIGHT * FONT_ROW_SCALE,
    FONT_ROW_X = 24,
    FONT_ROW_WIDTH = FONT_SCREEN_WIDTH - 2 * FONT_ROW_X,
    FONT_ROW_TOP = 56,
    FONT_ROW_PITCH = FONT_GLYPH_LINES + 8,
    FONT_UP_ARROW_Y = FONT_ROW_TOP - FONT_GLYPH_LINES - 4,
    FONT_DOWN_ARROW_Y = FONT_ROW_TOP + GRAPHICS_PAGE_WINDOW_ROWS * FONT_ROW_PITCH - 8 + 4,
    FONT_FOOTER_Y = FONT_DOWN_ARROW_Y + FONT_GLYPH_LINES + 12,
};

static s32 s_cursor;
static s32 s_first;
static int s_editing;
static GraphicsSettingsSnapshot s_snapshot;

#define GRAPHICS_PAGE_ROW_COUNT (GFX_ROW_COUNT + 1)

void EnterGraphicsOptionMenu(void) {
    s_cursor = GFX_ROW_PRESET;
    s_first = 0;
    s_editing = 0;
}

int GraphicsPageFirstVisibleRow(int cursor, int previousFirst, int windowRows,
                                int rowCount) {
    int first = previousFirst;
    int last = rowCount - windowRows;

    if (last < 0) last = 0;
    if (cursor < first) {
        first = cursor;
    } else if (cursor > first + windowRows - 1) {
        first = cursor - windowRows + 1;
    }
    if (first > last) first = last;
    if (first < 0) first = 0;
    return first;
}

/* Draws text at x, y and returns the plate's width (0 when it is not drawn).
 * rightEdge, if set, right-aligns it there; x == FONT_CENTRE centres it.
 * pixelsOut, if set, receives the text image (NULL when it is not drawn). */
static s32 DrawFontText(u8 **packet, GameOrderingTableEntry *ot, s32 x, s32 y,
                        const char *text, int selected, s32 rightEdge,
                        const uint8_t **pixelsOut) {
    int width = 0;
    int height = 0;
    const uint8_t *pixels = MenuFontTextPixels(text, selected, &width, &height);

    if (pixelsOut != NULL) *pixelsOut = pixels;
    if (pixels == NULL) return 0;
    if (rightEdge) x = rightEdge - width;
    else if (x == FONT_CENTRE) x = (FONT_SCREEN_WIDTH - width) / 2;
    *packet = EmitFontPixelsScaled(ot, *packet, x, y, width, height, pixels,
                                   FONT_ROW_SCALE);
    return width;
}

/* The page's rows are the settings plus a last EXIT row. */
static const char kExitName[] = "EXIT";
static const char kExitNote[] = "SAVE AND GO BACK";

static void PageRowParts(s32 row, char *name, size_t nameSize, char *value,
                         size_t valueSize) {
    if (row >= GFX_ROW_COUNT) {
        snprintf(name, nameSize, "%s", kExitName);
        value[0] = '\0';
        return;
    }
    GraphicsRowParts((GraphicsRow)row, name, nameSize, value, valueSize);
    /* While editing, the row shows arrows around its value. */
    if (s_editing && row == s_cursor && value[0] != '\0') {
        char arrowed[24];

        snprintf(arrowed, sizeof(arrowed), "< %s >", value);
        snprintf(value, valueSize, "%s", arrowed);
    }
}

static const char *PageRowNote(s32 row) {
    return row >= GFX_ROW_COUNT ? kExitNote : GraphicsRowNote((GraphicsRow)row);
}

static int DrawGraphicsFontScreen(void) {
    GameOrderingTableEntry *ot = GamePrimaryOrderingTable(0);
    u8 *packet = RENDER_PRIM_CURSOR_AS(u8);
    char name[24];
    char value[32];
    s32 slot;

    s_first = GraphicsPageFirstVisibleRow(s_cursor, s_first,
                                          GRAPHICS_PAGE_WINDOW_ROWS,
                                          GRAPHICS_PAGE_ROW_COUNT);
    for (slot = 0; slot < GRAPHICS_PAGE_WINDOW_ROWS; slot++) {
        s32 row = s_first + slot;
        s32 y = FONT_ROW_TOP + slot * FONT_ROW_PITCH;
        int selected = row == s_cursor;
        const uint8_t *bar = NULL;
        s32 nameWidth;
        s32 valueWidth;

        if (row >= GRAPHICS_PAGE_ROW_COUNT) break;
        PageRowParts(row, name, sizeof(name), value, sizeof(value));
        nameWidth = DrawFontText(&packet, ot, FONT_ROW_X, y, name, selected, 0,
                                 &bar);
        /* No face: nothing has been queued yet, so the caller can fall back. */
        if (bar == NULL && slot == 0) return 0;
        valueWidth = DrawFontText(&packet, ot, FONT_ROW_X, y, value, selected,
                                  FONT_ROW_X + FONT_ROW_WIDTH, NULL);
        /* Join the two plates into one bar; queued last, so drawn first. */
        if (bar != NULL && valueWidth > 0 &&
            nameWidth + valueWidth < FONT_ROW_WIDTH) {
            packet = AddTilePrim(ot, packet, FONT_ROW_X + nameWidth, y,
                                 FONT_ROW_WIDTH - nameWidth - valueWidth,
                                 FONT_GLYPH_LINES, bar[6], bar[7], bar[8]);
        }
    }
    if (s_first > 0) {
        DrawFontText(&packet, ot, FONT_CENTRE, FONT_UP_ARROW_Y,
                     "^", 0, 0, NULL);
    }
    if (s_first + GRAPHICS_PAGE_WINDOW_ROWS < GRAPHICS_PAGE_ROW_COUNT) {
        DrawFontText(&packet, ot, FONT_CENTRE,
                     FONT_DOWN_ARROW_Y, "v", 0, 0, NULL);
    }
    DrawFontText(&packet, ot, FONT_ROW_X, FONT_FOOTER_Y,
                 PageRowNote(s_cursor), 0, 0, NULL);
    g_RenderState.draw.packetCursor = packet;
    return 1;
}

static void DrawGraphicsOptionScreen(void) {
    char text[64];
    char name[24];
    char value[32];
    s32 row;

    if (DrawGraphicsFontScreen()) return;
    for (row = 0; row < GRAPHICS_PAGE_ROW_COUNT; row++) {
        if (row >= GFX_ROW_COUNT || (s_editing && row == s_cursor)) {
            PageRowParts(row, name, sizeof(name), value, sizeof(value));
            snprintf(text, sizeof(text), "%-18s%s", name, value);
        } else {
            GraphicsRowLabel((GraphicsRow)row, text, sizeof(text));
        }
        DrawText8x8(GRAPHICS_TEXT_X, GRAPHICS_TOP + row * GRAPHICS_ROW_HEIGHT,
                    text, GRAPHICS_CLUT);
    }
    DrawText8x8(GRAPHICS_TEXT_X, GRAPHICS_NOTE_Y, PageRowNote(s_cursor),
                GRAPHICS_CLUT);
    DrawMenuCursorArrow(GRAPHICS_CURSOR_X,
                        GRAPHICS_TOP + s_cursor * GRAPHICS_ROW_HEIGHT +
                            GRAPHICS_CURSOR_Y_OFFSET);
}

/* Save, then back to the title with GRAPHICS selected. */
static void LeaveGraphicsPage(s32 cue) {
    char path[512];

    PlaySoundCue(cue);
    if (PlatformUserConfigPath("graphics", path, sizeof(path))) {
        GraphicsSettingsSave(path);
    }
    SetTitleMenuReturnSelection(TITLE_MENU_GRAPHICS);
    StartOptionMenuExit(GAME_SCENE_ENTER_FRONTEND);
}

/* Edit mode, as on the Sound page: left/right change the row, confirm keeps it,
 * cancel puts everything back. Up/down do nothing. */
static void UpdateGraphicsEdit(void) {
    GraphicsRow row = (GraphicsRow)s_cursor;
    char name[24];
    char before[24];
    char after[24];

    if (g_PadPressed & PAD_CANCEL) {
        GraphicsSettingsSnapshotRestore(&s_snapshot);
        s_editing = 0;
        PlaySoundCue(3);
    } else if (g_PadPressed & PAD_CONFIRM) {
        s_editing = 0;
        PlaySoundCue(2);
    } else if (g_PadPressed & (PAD_LEFT | PAD_RIGHT)) {
        GraphicsRowParts(row, name, sizeof(name), before, sizeof(before));
        GraphicsRowAdjust(row, (g_PadPressed & PAD_LEFT) ? -1 : 1);
        GraphicsRowParts(row, name, sizeof(name), after, sizeof(after));
        if (strcmp(before, after) != 0) PlaySoundCue(1);
    }
}

void UpdateGraphicsOptionMenu(void) {
    s32 oldCursor;

    s_cursor = AddClampedMenuValue(s_cursor, 0, 0, GRAPHICS_PAGE_ROW_COUNT - 1);
    DrawGraphicsOptionScreen();
    if (s_editing) {
        UpdateGraphicsEdit();
        return;
    }
    oldCursor = s_cursor;
    if (g_PadPressed & PAD_UP) {
        s_cursor = WrapMenuIndex(s_cursor, -1, GRAPHICS_PAGE_ROW_COUNT);
    } else if (g_PadPressed & PAD_DOWN) {
        s_cursor = WrapMenuIndex(s_cursor, 1, GRAPHICS_PAGE_ROW_COUNT);
    }
    if (oldCursor != s_cursor) {
        PlaySoundCue(1);
    }

    if (g_PadPressed & PAD_CANCEL) {
        LeaveGraphicsPage(3);
    } else if (g_PadPressed & PAD_CONFIRM) {
        if (s_cursor >= GFX_ROW_COUNT) {
            LeaveGraphicsPage(2);
        } else {
            GraphicsSettingsSnapshotTake(&s_snapshot);
            s_editing = 1;
            PlaySoundCue(2);
        }
    }
}
