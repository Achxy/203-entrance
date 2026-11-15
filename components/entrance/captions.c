#include "captions.h"
#include "font8x8.h"
#include <string.h>

const char *caption_text(unsigned index)
{
    (void)index;
    return "203";
}
void caption_row(unsigned index, unsigned width, unsigned height, unsigned y, uint16_t *out)
{
    (void)index;
    room_number_row(CAPTION_GLYPH_SCALE, width, height, y, out);
}
void room_number_row(unsigned scale, unsigned width, unsigned height, unsigned y, uint16_t *out)
{
    const char *text = "203";
    /* The existing 8x8 font's visible "203" ink is 22x7 pixels. Center the
       ink, rather than the padded 24x8 character cells, at an integer scale. */
    const unsigned ink_width = 22 * scale, ink_height = 7 * scale;
    memset(out, 0, width * sizeof(*out));
    if (!scale || width < ink_width || height < ink_height)
        return;
    unsigned left = (width - ink_width) / 2, top = (height - ink_height) / 2;
    if (y < top || y >= top + ink_height)
        return;
    unsigned glyph_y = (y - top) / scale;
    for (unsigned x = 0; x < ink_width; x++) {
        unsigned column = x / scale, ch = (unsigned char)text[column / 8];
        if (font8x8_basic[ch][glyph_y] & (1u << (column % 8)))
            out[left + x] = 0xffff;
    }
}

unsigned study_caption_index(uint64_t elapsed_us)
{
    return (unsigned)((elapsed_us / STUDY_CAPTION_PERIOD_US) % 3);
}
const char *study_caption_text(unsigned index)
{
    static const char *messages[] = {
        "*paws at you* please study for endsems vro",
        "*paws at you* actually vro",
        "*paws at you* さよなら vro"};
    return messages[index % 3];
}
/* Four unmodified Misaki Gothic glyphs, Copyright (C) 2002-2021 Num Kadoma.
   Source, exact subset and permissive license: assets/study/misaki-glyphs.json. */
static const uint8_t kana[4][8] = {
    {16, 16, 126, 32, 36, 2, 60, 0}, {16, 16, 112, 16, 28, 50, 76, 0},
    {4, 47, 68, 34, 57, 100, 24, 0}, {12, 16, 2, 58, 70, 64, 60, 0}};
typedef struct { const uint8_t *rows; unsigned left, width, advance; } text_glyph_t;
static text_glyph_t text_glyph(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    unsigned ch = *p++;
    if (ch == 0xe3 && p[0] && p[1]) {
        unsigned cp = (ch & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63);
        static const unsigned points[] = {0x3055, 0x3088, 0x306a, 0x3089};
        p += 2;
        *text = (const char *)p;
        for (unsigned i = 0; i < 4; i++)
            if (cp == points[i]) return (text_glyph_t){kana[i], 0, 8, 8};
        ch = '?';
    }
    *text = (const char *)p;
    if (ch == ' ') return (text_glyph_t){font8x8_basic[' '], 0, 0, 3};
    if (ch >= 128) ch = '?';
    unsigned bits = 0, left = 0, right = 7;
    for (unsigned row = 0; row < 8; row++) bits |= font8x8_basic[ch][row];
    while (left < 7 && !(bits & (1u << left))) left++;
    while (right > left && !(bits & (1u << right))) right--;
    return (text_glyph_t){font8x8_basic[ch], left, right - left + 1, right - left + 2};
}
static unsigned text_width(const char *text)
{
    unsigned width = 0;
    while (*text) width += text_glyph(&text).advance;
    return width ? width - 1 : 0;
}
void study_caption_row(unsigned index, unsigned width, unsigned y, uint16_t *out)
{
    memset(out, 0, width * sizeof(*out));
    const char *lines[3] = {"*paws at you*", "", ""};
    unsigned count = 2;
    switch (index % 3) {
    case 0:
        if (width >= 160) { lines[0] = "*paws at you* please"; lines[1] = "study for endsems vro"; }
        else { count = 3; lines[1] = "please study for"; lines[2] = "endsems vro"; }
        break;
    case 1: lines[1] = "actually vro"; break;
    default: lines[1] = "さよなら vro"; break;
    }
    unsigned height = width >= 160 ? 20 : 30, top = (height - count * 10) / 2 + 1;
    if (y < top || y >= top + count * 10) return;
    unsigned line = (y - top) / 10, row = (y - top) % 10;
    if (row >= 8) return;
    const char *text = lines[line];
    unsigned ink = text_width(text);
    if (ink > width) return;
    unsigned x = (width - ink) / 2;
    while (*text) {
        text_glyph_t glyph = text_glyph(&text);
        for (unsigned col = 0; col < glyph.width && x + col < width; col++)
            if (glyph.rows[row] & (1u << (col + glyph.left))) out[x + col] = 0xffff;
        x += glyph.advance;
    }
}
