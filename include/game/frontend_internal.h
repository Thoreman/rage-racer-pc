#ifndef GAME_FRONTEND_INTERNAL_H
#define GAME_FRONTEND_INTERNAL_H

#include "common.h"

typedef struct Frontend {
    s32 state;
    u32 idleTimer;
    s32 attractCycle;
    s32 attractTimer;
    s32 exitTimer;
    s32 pulse;
    s32 fade;
    s32 menuSlide;
    s32 selection;
} Frontend;

Frontend *MenuFrontend(void);

typedef enum TitleMenuItem {
    TITLE_MENU_GRAND_PRIX,
    TITLE_MENU_EXTRA_GRAND_PRIX,
    TITLE_MENU_TIME_ATTACK,
    TITLE_MENU_CUSTOM,
    TITLE_MENU_LOAD_SAVE,
    TITLE_MENU_OPTIONS,
    /* VR build only: opens the graphics page; hidden unless PortXrAvailable(). */
    TITLE_MENU_GRAPHICS,
    TITLE_MENU_ITEM_COUNT,
} TitleMenuItem;

/* Rows shown on the title menu: all of them on the VR build, one fewer on
 * the flat game. */
s32 TitleMenuVisibleCount(void);

/* One-shot: the next time the title menu opens, start on `item` instead of
 * GRAND PRIX (used when the graphics page returns to the title). */
void SetTitleMenuReturnSelection(TitleMenuItem item);

TitleMenuItem MoveTitleMenuSelection(s32 selection, s32 direction,
                                     int extraGrandPrixUnlocked);

#endif
