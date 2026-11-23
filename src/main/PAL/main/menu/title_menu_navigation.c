#include "game/frontend_internal.h"
#include "xr_profile.h"

s32 TitleMenuVisibleCount(void) {
    return PortXrAvailable() ? TITLE_MENU_ITEM_COUNT : TITLE_MENU_ITEM_COUNT - 1;
}

TitleMenuItem MoveTitleMenuSelection(s32 selection, s32 direction,
                                     int extraGrandPrixUnlocked) {
    const s32 count = TitleMenuVisibleCount();

    selection %= count;
    if (selection < 0) {
        selection += count;
    }

    if (direction < 0) {
        selection = selection > 0 ? selection - 1 : count - 1;
        if (!extraGrandPrixUnlocked &&
            selection == TITLE_MENU_EXTRA_GRAND_PRIX) {
            selection = TITLE_MENU_GRAND_PRIX;
        }
    } else if (direction > 0) {
        selection = selection + 1 < count ? selection + 1 : 0;
        if (!extraGrandPrixUnlocked &&
            selection == TITLE_MENU_EXTRA_GRAND_PRIX) {
            selection = TITLE_MENU_TIME_ATTACK;
        }
    }
    return (TitleMenuItem)selection;
}
