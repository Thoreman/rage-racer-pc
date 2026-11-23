#include "game/menu.h"
#include "game/menu_internal.h"

#include <stdint.h>

enum {
    OPTION_FADE_OPAQUE = 0x100,
    OPTION_FADE_EXIT_STEP = 8,
};

/* The title's GRAPHICS row asks for the graphics page; `s_graphicsActive` marks
 * the visit so neither fade shows the root menu behind it. */
static int s_graphicsPending;
static int s_graphicsActive;

void OptionRequestGraphicsPage(void) { s_graphicsPending = 1; }

/* Arms the fade-out that leaves the setup menu for scene `scene`. */
void StartOptionMenuExit(GameSceneId scene) {
    MenuOption()->exitScene = scene;
    g_GameMode = OPTION_MODE_FADE;
    g_FadeStep = OPTION_FADE_EXIT_STEP;
}

/* OPTION_MODE_FADE: integrates the fade, then opens the root menu or leaves
 * for the destination stored by StartOptionMenuExit. */
void UpdateOptionMenuFade(void) {
    const s32 step = g_FadeStep;
    const int64_t next = (int64_t)g_FadeLevel + step;

    g_FadeLevel = StepFade(g_FadeLevel, step, OPTION_FADE_OPAQUE);
    if (step < 0 && g_FadeLevel == 0) {
        g_FadeStep = 0;
        if (s_graphicsPending) {
            s_graphicsPending = 0;
            s_graphicsActive = 1;
            EnterGraphicsOptionMenu();
            g_GameMode = OPTION_MODE_GRAPHICS;
        } else {
            g_GameMode = OPTION_MODE_ROOT;
        }
    } else if (step > 0 && next > OPTION_FADE_OPAQUE) {
        g_SceneId = MenuOption()->exitScene;
        s_graphicsActive = 0;
    }

    DrawFullscreenFadeTile480(g_FadeLevel, 0x49);
    if (!s_graphicsPending && !s_graphicsActive) {
        DrawOptionRootMenu();
    }
}
