#include "game/frontend_internal.h"
#include "game/menu_internal.h"
#include "game/prim.h"
#include "menu_images.h"
#include "menu_label_font.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>

enum { TILE_LIMIT = 4096 };

typedef struct Tile {
    s32 x, y, w, h, r, g, b;
} Tile;

static Tile s_tiles[TILE_LIMIT];
static int s_tileCount;

const uint8_t *MenuImage(int item, int selected) {
    (void)item;
    (void)selected;
    return NULL;
}

u8 *AddTilePrim(GameOrderingTableEntry *ot, u8 *packet, s32 x, s32 y,
                s32 w, s32 h, s32 r, s32 g, s32 b) {
    (void)ot;
    assert(s_tileCount < TILE_LIMIT);
    s_tiles[s_tileCount++] = (Tile){x, y, w, h, r, g, b};
    return packet;
}

static int DarkColumns(const uint8_t *pixels) {
    int column;
    int count = 0;

    for (column = 1; column < MENU_LABEL_WIDTH - 1; column++) {
        int row;
        for (row = 0; row < MENU_LABEL_HEIGHT; row++) {
            const uint8_t *pixel = pixels + (row * MENU_LABEL_WIDTH + column) * 3;
            if (pixel[0] < 40 && pixel[1] < 40 && pixel[2] < 40) {
                count++;
                break;
            }
        }
    }
    return count;
}

static int DarkColumnsIn(const uint8_t *pixels, int width) {
    int column;
    int count = 0;

    for (column = 1; column < width - 1; column++) {
        int row;
        for (row = 0; row < MENU_LABEL_HEIGHT; row++) {
            const uint8_t *pixel = pixels + (row * width + column) * 3;
            if (pixel[0] < 40 && pixel[1] < 40 && pixel[2] < 40) {
                count++;
                break;
            }
        }
    }
    return count;
}

static void ExpectPlate(int selected, s32 height) {
    int index;
    int plates = 0;

    for (index = 0; index < s_tileCount; index++) {
        const Tile *tile = &s_tiles[index];
        assert(tile->x >= 104);
        assert(tile->y >= 100);
        assert(tile->x + tile->w <= 104 + MENU_LABEL_WIDTH);
        assert(tile->y + tile->h <= 100 + height);
        if (tile->w == MENU_LABEL_WIDTH && tile->h == height) {
            plates++;
            if (selected) {
                assert(tile->r == 0xD0 && tile->g == 0x50 && tile->b == 0x48);
            } else {
                assert(tile->r == 0x78 && tile->g == 0x78 && tile->b == 0x78);
            }
        } else {
            assert(tile->h == 1);
        }
    }
    assert(plates == 1);
}

int main(void) {
    int item;
    int timeColumns;
    int optionColumns;
    u8 packet[1];
    const uint8_t *normal;
    const uint8_t *selected;

    for (item = 0; item < TITLE_MENU_ITEM_COUNT; item++) {
        const uint8_t *gray = MenuLabelFontPixels(item, 0);
        const uint8_t *hot = MenuLabelFontPixels(item, 1);
        const uint8_t *plate;
        const uint8_t *edge;

        assert(gray != NULL && hot != NULL);
        plate = gray + 2 * 3;
        edge = gray;
        assert(plate[0] == 0x78 && plate[1] == 0x78 && plate[2] == 0x78);
        assert(edge[0] == 0x98 && edge[1] == 0x98 && edge[2] == 0x98);
        assert(gray[(MENU_LABEL_WIDTH - 1) * 3] == 0x48);
        plate = hot + 2 * 3;
        assert(plate[0] == 0xD0 && plate[1] == 0x50 && plate[2] == 0x48);
        assert(DarkColumns(gray) > 20);
    }

    normal = MenuLabelFontPixels(TITLE_MENU_TIME_ATTACK, 0);
    selected = MenuLabelFontPixels(TITLE_MENU_OPTIONS, 0);
    timeColumns = DarkColumns(normal);
    optionColumns = DarkColumns(selected);
    assert(timeColumns > optionColumns);

    s_tileCount = 0;
    assert(DrawTitleMenuLabel(NULL, packet, TITLE_MENU_GRAND_PRIX,
                              104, 100, 16, 0) == packet);
    ExpectPlate(0, 16);

    s_tileCount = 0;
    DrawTitleMenuLabel(NULL, packet, TITLE_MENU_GRAND_PRIX, 104, 100, 16, 1);
    ExpectPlate(1, 16);

    s_tileCount = 0;
    DrawTitleMenuLabel(NULL, packet, TITLE_MENU_TIME_ATTACK, 104, 100, 4, 0);
    ExpectPlate(0, 4);

    s_tileCount = 0;
    assert(DrawTitleMenuLabel(NULL, packet, TITLE_MENU_CUSTOM, 104, 100, 0, 0) ==
           packet);
    assert(s_tileCount == 0);

    {
        int width = 0;
        int height = 0;
        int otherWidth = 0;
        char text[24];
        const uint8_t *first = MenuFontTextPixels("TEXTURE DETAIL", 0, &width, &height);
        const uint8_t *again;
        const uint8_t *hot;
        const uint8_t *shortText;
        int index;

        /* Page text: the title buttons' height, colours and bevel. */
        assert(first != NULL);
        assert(height == MENU_LABEL_HEIGHT);
        assert(width > 80 && width <= 224);
        assert(first[2 * 3] == 0x78 && first[0] == 0x98);
        assert(first[(width - 1) * 3] == 0x48);
        again = MenuFontTextPixels("TEXTURE DETAIL", 0, &otherWidth, &height);
        assert(again == first && otherWidth == width);
        hot = MenuFontTextPixels("TEXTURE DETAIL", 1, &otherWidth, &height);
        assert(hot != NULL && hot != first && otherWidth == width);
        assert(hot[2 * 3] == 0xD0 && hot[1 + 2 * 3] == 0x50);
        shortText = MenuFontTextPixels("OFF", 0, &otherWidth, &height);
        assert(shortText != NULL && otherWidth < width);
        assert(DarkColumnsIn(first, width) > 20);
        assert(MenuFontTextPixels("", 0, &width, &height) == NULL);
        assert(MenuFontTextPixels(NULL, 0, &width, &height) == NULL);
        assert(MenuFontTextPixels("THIS TEXT IS FAR TOO LONG FOR THE POOL KEY",
                                  0, &width, &height) == NULL);

        /* A flood of distinct strings recycles the fixed pool and still
         * returns drawable pixels. */
        for (index = 0; index < 500; index++) {
            snprintf(text, sizeof(text), "VALUE %d", index);
            assert(MenuFontTextPixels(text, index & 1, &width, &height) != NULL);
            assert(height == MENU_LABEL_HEIGHT && width > 12);
        }
    }

    puts("menu label font tests passed");
    return 0;
}
