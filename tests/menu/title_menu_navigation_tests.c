#include "game/frontend_internal.h"

#include <limits.h>
#include <stdio.h>

static int s_xrAvailable;
int PortXrAvailable(void) { return s_xrAvailable; }

#define CHECK(actual, expected)                                                \
    do {                                                                       \
        if ((actual) != (expected)) {                                          \
            fprintf(stderr, "check failed at line %d: got %d, expected %d\n", \
                    __LINE__, (actual), (expected));                            \
            return 1;                                                          \
        }                                                                      \
    } while (0)

int main(void) {
    s_xrAvailable = 0;
    CHECK(MoveTitleMenuSelection(TITLE_MENU_GRAND_PRIX, -1, 1),
          TITLE_MENU_OPTIONS);
    CHECK(MoveTitleMenuSelection(TITLE_MENU_OPTIONS, 1, 1),
          TITLE_MENU_GRAND_PRIX);
    CHECK(MoveTitleMenuSelection(TITLE_MENU_GRAND_PRIX, 1, 1),
          TITLE_MENU_EXTRA_GRAND_PRIX);
    CHECK(MoveTitleMenuSelection(TITLE_MENU_EXTRA_GRAND_PRIX, 1, 1),
          TITLE_MENU_TIME_ATTACK);
    CHECK(MoveTitleMenuSelection(TITLE_MENU_TIME_ATTACK, 1, 1),
          TITLE_MENU_CUSTOM);
    CHECK(MoveTitleMenuSelection(TITLE_MENU_CUSTOM, 1, 1),
          TITLE_MENU_LOAD_SAVE);
    CHECK(MoveTitleMenuSelection(TITLE_MENU_GRAND_PRIX, 1, 0),
          TITLE_MENU_TIME_ATTACK);
    CHECK(MoveTitleMenuSelection(TITLE_MENU_TIME_ATTACK, -1, 0),
          TITLE_MENU_GRAND_PRIX);
    CHECK(MoveTitleMenuSelection(TITLE_MENU_OPTIONS, 1, 0),
          TITLE_MENU_GRAND_PRIX);
    CHECK(MoveTitleMenuSelection(TITLE_MENU_GRAND_PRIX, -1, 0),
          TITLE_MENU_OPTIONS);
    CHECK(MoveTitleMenuSelection(TITLE_MENU_TIME_ATTACK, 0, 0),
          TITLE_MENU_TIME_ATTACK);
    CHECK(MoveTitleMenuSelection(6 + TITLE_MENU_LOAD_SAVE,
                                 0, 1),
          TITLE_MENU_LOAD_SAVE);
    CHECK(MoveTitleMenuSelection(-1, 0, 1), TITLE_MENU_OPTIONS);
    CHECK(MoveTitleMenuSelection(INT_MIN, 0, 1), TITLE_MENU_LOAD_SAVE);

    /* The VR build adds GRAPHICS after OPTION: seven items, wrapping over all. */
    s_xrAvailable = 1;
    CHECK(MoveTitleMenuSelection(TITLE_MENU_OPTIONS, 1, 1),
          TITLE_MENU_GRAPHICS);
    CHECK(MoveTitleMenuSelection(TITLE_MENU_GRAPHICS, 1, 1),
          TITLE_MENU_GRAND_PRIX);
    CHECK(MoveTitleMenuSelection(TITLE_MENU_GRAND_PRIX, -1, 1),
          TITLE_MENU_GRAPHICS);
    CHECK(MoveTitleMenuSelection(TITLE_MENU_GRAPHICS, -1, 1),
          TITLE_MENU_OPTIONS);
    CHECK(MoveTitleMenuSelection(TITLE_MENU_GRAPHICS, 1, 0),
          TITLE_MENU_GRAND_PRIX);
    CHECK(MoveTitleMenuSelection(TITLE_MENU_GRAND_PRIX, -1, 0),
          TITLE_MENU_GRAPHICS);
    CHECK(MoveTitleMenuSelection(TITLE_MENU_GRAND_PRIX, 1, 0),
          TITLE_MENU_TIME_ATTACK);
    CHECK(MoveTitleMenuSelection(TITLE_MENU_TIME_ATTACK, -1, 0),
          TITLE_MENU_GRAND_PRIX);
    CHECK(MoveTitleMenuSelection(TITLE_MENU_GRAPHICS, 0, 1),
          TITLE_MENU_GRAPHICS);
    CHECK(MoveTitleMenuSelection(-1, 0, 1), TITLE_MENU_GRAPHICS);

    puts("title menu navigation tests passed");
    return 0;
}
