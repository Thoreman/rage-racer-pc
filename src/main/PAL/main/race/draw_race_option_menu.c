#include "game/prim.h"
#include "game/race.h"
#include "game/race_scene_internal.h"
#include "game/save_internal.h"
#include "game/render.h"
#include "game/render_internal.h"
#include "game/race_hud_internal.h"
#include "game/state.h"
#include "rage/compat.h"

#include <string.h>

enum {
    RACE_OPTION_SELECTION_TOP = 0x68,
    RACE_OPTION_SELECTION_ROW_HEIGHT = 8,
    RACE_OPTION_SELECTION_HEIGHT = 9,
    RACE_OPTION_DIM_PASSES = 2,
    RACE_OPTION_HINT_TOP = 0xA4,
    RACE_OPTION_HINT_ROW_HEIGHT = 10,
    RACE_OPTION_HINT_WIDTH = 0xC0,
    RACE_OPTION_RETRY_DIGIT_COUNT = 6,
};

typedef struct RaceOptionMarqueeText {
    const char *first;
    const char *second;
} RaceOptionMarqueeText;

typedef struct RaceOptionAnimation {
    s32 pulseAngle;
    s16 firstScroll;
    s16 secondScroll;
} RaceOptionAnimation;

static RaceOptionAnimation s_Animation;

static const RaceOptionMarqueeText s_MarqueeText[] = {
    {"  RAGE RACER GE", "TS YOU GOING!  "},
    {"  RAGE RACER GE", "TS YOU GOING!  "},
    {"   KICK BACK AN", "D CHILL OUT!   "},
    {"     SLASH THOS", "E RECORDS!     "},
};

void ResetRaceOptionMenuAnimation(void) {
    s_Animation = (RaceOptionAnimation){0, -240, 240};
}

static s32 ClampRaceOptionCursor(s32 cursor, s32 grandPrixMode) {
    s32 lastOption = LastRacePauseOption(grandPrixMode);

    if (cursor < 0) {
        return 0;
    }
    return cursor < lastOption ? cursor : lastOption;
}

static s32 RaceOptionRetryDigit(void) {
    s32 retries =
        g_CourseProgress != NULL ? g_CourseProgress->retriesRemaining : 0;

    if (retries < 0) {
        return 0;
    }
    return retries < RACE_OPTION_RETRY_DIGIT_COUNT
               ? retries
               : RACE_OPTION_RETRY_DIGIT_COUNT - 1;
}

void DrawRaceOptionMenu(s32 cursorRow) {
    GameOrderingTableEntry *ot;
    u8 *next;
    u8 *packet;
    s32 selectionY;
    POLY_FT4 *quad;
    SPRT *marquee;
    RaceOptionMarqueeState marqueeState;
    RaceOptionPulseState pulseState;
    s32 pass;
    s32 feedbackWidth;
    s32 speedWidth;
    s32 hintCount;
    const char *const *hints;
    s32 selectionWidth;
    s32 selectionX;

    cursorRow = ClampRaceOptionCursor(cursorRow, g_GrandPrixMode);
    ot = GamePrimaryOrderingTable(0);
    marquee = RENDER_PRIM_CURSOR_AS(SPRT);
    SetSprt(marquee);
    SetShadeTex(marquee, 0);
    marquee->x0 = 0x8C;
    marquee->y0 = 0x5A;
    marquee->w = 0x28;
    marquee->h = 8;
    marquee->u0 = 0xD8;
    marquee->v0 = 0x38;
    marquee->clut = 0x7893;
    marqueeState = AdvanceRaceOptionMarquee(
        s_Animation.firstScroll, s_Animation.secondScroll, g_SceneTimer);
    marquee->r0 = marqueeState.brightness;
    marquee->g0 = marqueeState.brightness;
    marquee->b0 = marqueeState.brightness;
    AddPrim(ot, marquee);

    s_Animation.firstScroll = marqueeState.firstScroll;
    s_Animation.secondScroll = marqueeState.secondScroll;

    next = QueueDrawAreaPrim(ot, (DrawPacket *)(marquee + 1),
                             0, 0, 0x140, 0xF0);
    g_RenderState.draw.packetCursor = next;
    /* The feedback row sits under the renderer line, so the marquee moves
     * down with it. */
    DrawText8x8((s_Animation.firstScroll >> 2) + 0xA0, 0x92,
                s_MarqueeText[marqueeState.textFrame].first, 0x7811);
    DrawText8x8((s_Animation.secondScroll >> 2) + 0xA0, 0x92,
                s_MarqueeText[marqueeState.textFrame].second, 0x7811);

    next = QueueDrawAreaPrim(ot, RENDER_PRIM_CURSOR_AS(DrawPacket),
                             0x72, 0x92, 0x5C, 0xC);
    packet = GameQueueSprite(
        ot, next, 0x88, 0x6A, 0x30, 8, 0xD0, 0x10, 0x7893);
    if (g_GrandPrixMode != 0) {
        packet = GameQueueSprite(
            ot, packet, 0x88, 0x72, 0x30, 8, 0xA0, 0x28, 0x7893);
        packet = GameQueueSprite(
            ot, packet, 0x84, 0x8A, 0x30, 8, 0xD0, 0x28, 0x7893);
        packet = GameQueueSprite(
            ot, packet, 0xB8, 0x8A, 8, 8,
            RaceOptionRetryDigit() * 8, 0, 0x78CC);
        packet = GameQueueSprite(
            ot, packet, 0x78, 0x8A, 8, 8, 0xD8, 8, 0x78CC);
        packet = GameQueueSprite(
            ot, packet, 0xC0, 0x8A, 8, 8, 0xE8, 8, 0x78CC);
    } else {
        packet = GameQueueSprite(
            ot, packet, 0x85, 0x72, 0x38, 8, 0xA0, 0x40, 0x7893);
        packet = GameQueueSprite(
            ot, packet, 0x90, 0x7A, 0x28, 8, 0xD8, 0x40, 0x7893);
    }

    g_RenderState.draw.packetCursor = packet;
    if (RACE_PAUSE_RENDERER_ROW) {
        DrawText8x8(PortModernRendererEnabled() ? 0x88 : 0x84,
                    g_GrandPrixMode != 0 ? 0x7A : 0x82,
                    PortModernRendererEnabled() ? "MODERN" : "CLASSIC",
                    0x78CC);
    }
    speedWidth = 0;
    if (RACE_PAUSE_SPEED_ROW) {
        char speed[16];

        /* The Quest's SPEED row takes the renderer row's place. */
        PortGameSpeedLabel(speed, sizeof(speed));
        speedWidth = (s32)strlen(speed) * 8;
        DrawText8x8(0xA0 - speedWidth / 2,
                    g_GrandPrixMode != 0 ? 0x7A : 0x82, speed, 0x78CC);
    }
    {
        char feedback[16];

        PortForceFeedbackLabel(feedback, sizeof(feedback));
        /* Centred like the rows above; "FFB nnn" lands on the old 0x84. */
        feedbackWidth = (s32)strlen(feedback) * 8;
        /* Without the renderer (or speed) row it takes that row's place. */
        DrawText8x8(0xA0 - feedbackWidth / 2,
                    (g_GrandPrixMode != 0 ? 0x82 : 0x8A) -
                        (RACE_PAUSE_RENDERER_ROW || RACE_PAUSE_SPEED_ROW
                             ? 0 : 8),
                    feedback, 0x78CC);
    }
    /* Port: control reminders in their own dimmed block under the menu. */
    hintCount = PortPauseHints(&hints);
    for (pass = 0; pass < hintCount; pass++) {
        DrawText8x8(0xA0 - (s32)strlen(hints[pass]) * 4,
                    RACE_OPTION_HINT_TOP + 4 +
                        pass * RACE_OPTION_HINT_ROW_HEIGHT,
                    hints[pass], 0x78CC);
    }
    packet = RENDER_PRIM_CURSOR_AS(u8);

    selectionY = cursorRow * RACE_OPTION_SELECTION_ROW_HEIGHT +
                 RACE_OPTION_SELECTION_TOP;
    /* A feedback label wider than the retail box stretches it on its row. */
    selectionWidth = 0x40;
    if (cursorRow == LastRacePauseOption(g_GrandPrixMode) &&
        feedbackWidth + 8 > selectionWidth) {
        selectionWidth = feedbackWidth + 8;
    }
    if (RACE_PAUSE_SPEED_ROW &&
        cursorRow == LastRacePauseOption(g_GrandPrixMode) - 1 &&
        speedWidth + 8 > selectionWidth) {
        selectionWidth = speedWidth + 8;
    }
    selectionX = 0xA0 - selectionWidth / 2;
    packet = AddTilePrim(
        ot, packet, selectionX, selectionY, selectionWidth, 1,
        0xFF, 0xFF, 0);
    packet = AddTilePrim(
        ot, packet, selectionX, selectionY + RACE_OPTION_SELECTION_HEIGHT,
        selectionWidth, 1, 0xFF, 0xFF, 0);
    packet = AddTilePrim(
        ot, packet, selectionX, selectionY, 1, RACE_OPTION_SELECTION_HEIGHT,
        0xFF, 0xFF, 0);
    packet = AddTilePrim(
        ot, packet, selectionX + selectionWidth - 1, selectionY, 1,
        RACE_OPTION_SELECTION_HEIGHT, 0xFF, 0xFF, 0);

    /* The original overlay deliberately applies the same translucent tile
     * twice to make the paused race dark enough behind the menu. */
    for (pass = 0; pass < RACE_OPTION_DIM_PASSES; pass++) {
        packet = GameQueueTileTrans(
            ot, packet, 0x70, 0x50, 0x60, 0x50, 8, 8, 8);
        if (hintCount > 0) {
            packet = GameQueueTileTrans(
                ot, packet, 0xA0 - RACE_OPTION_HINT_WIDTH / 2,
                RACE_OPTION_HINT_TOP, RACE_OPTION_HINT_WIDTH,
                hintCount * RACE_OPTION_HINT_ROW_HEIGHT + 6, 8, 8, 8);
        }
    }
    quad = (POLY_FT4 *)packet;

    pulseState = AdvanceRaceOptionPulse(s_Animation.pulseAngle);
    s_Animation.pulseAngle = pulseState.angle;

    SetPolyFT4(quad);
    quad->r0 = 0x60;
    quad->g0 = 0x60;
    quad->b0 = 0x60;
    quad->x0 = 0xA0 - pulseState.halfWidth;
    quad->x2 = 0xA0 - pulseState.halfWidth;
    quad->x1 = 0xA0 + pulseState.halfWidth;
    quad->x3 = 0xA0 + pulseState.halfWidth;
    quad->y0 = 0x58;
    quad->y1 = 0x58;
    quad->y2 = 0x9C;
    quad->y3 = 0x9C;
    quad->u0 = 0xA8;
    quad->v0 = 0xA8;
    quad->u1 = 0xFF;
    quad->v1 = 0xA8;
    quad->u2 = 0xA8;
    quad->v2 = 0xE0;
    quad->u3 = 0xFF;
    quad->v3 = 0xE0;
    quad->clut = 0x784B;
    quad->tpage = 9;
    AddPrim(ot, quad);

    g_RenderState.draw.packetCursor = QueueDrawModePrim(ot, (u8 *)(quad + 1), 9);
}
