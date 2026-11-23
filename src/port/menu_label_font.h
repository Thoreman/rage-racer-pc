#ifndef MENU_LABEL_FONT_H
#define MENU_LABEL_FONT_H

#include <stdint.h>

enum {
    MENU_LABEL_WIDTH = 112,
    MENU_LABEL_HEIGHT = 16,
};

/* RGB8, row-major, one retail 112x16 title button. NULL when the item is
 * outside the six-entry menu or the face cannot be read. The pointer stays
 * valid for the rest of the process. */
const uint8_t *MenuLabelFontPixels(int item, int selected);

/* RGB8 pixels for one line of text in the same face and colours: a bar of
 * MENU_LABEL_HEIGHT rows and *width columns (the text's advance plus padding).
 * Cached per (text, selected) in a fixed pool, so the pointer is valid until
 * enough other strings have been requested to recycle the entry: use it in the
 * frame it was asked for. NULL if the face cannot be read or the text is empty
 * or too long. */
const uint8_t *MenuFontTextPixels(const char *text, int selected, int *width,
                                  int *height);

#endif
