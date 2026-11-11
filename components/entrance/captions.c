#include "captions.h"
#include "font8x8.h"
#include <string.h>

static const char *messages[] = {
    "Shenanigans by the 203 dudes",
    "Achyuth & Jaideep",
    "Knock the mario theme to unlock the door",
};
const char *caption_text(unsigned index)
{
    return messages[index % 3];
}
void caption_row(unsigned index, unsigned width, unsigned height, unsigned y, uint16_t *out)
{
    char lines[4][21] = {{0}};
    const char *text = caption_text(index);
    unsigned columns = width / CAPTION_GLYPH_PIXELS, count = 0;
    if (columns > 20)
        columns = 20;
    while (*text && count < 4) {
        unsigned length = strlen(text);
        if (length > columns) {
            length = columns;
            while (length && text[length] != ' ')
                length--;
            if (!length)
                length = columns;
        }
        memcpy(lines[count++], text, length);
        text += length;
        while (*text == ' ')
            text++;
    }
    unsigned top =
        height > count * CAPTION_GLYPH_PIXELS ? (height - count * CAPTION_GLYPH_PIXELS) / 2 : 0;
    for (unsigned x = 0; x < width; x++) {
        out[x] = 0;
        if (y < top || y >= top + count * CAPTION_GLYPH_PIXELS)
            continue;
        const char *line = lines[(y - top) / CAPTION_GLYPH_PIXELS];
        unsigned length = strlen(line), left = (width - length * CAPTION_GLYPH_PIXELS) / 2;
        if (x >= left && x < left + length * CAPTION_GLYPH_PIXELS) {
            unsigned ch = (unsigned char)line[(x - left) / CAPTION_GLYPH_PIXELS];
            unsigned glyph_y = ((y - top) % CAPTION_GLYPH_PIXELS) * 8 / CAPTION_GLYPH_PIXELS;
            unsigned glyph_x = ((x - left) % CAPTION_GLYPH_PIXELS) * 8 / CAPTION_GLYPH_PIXELS;
            if (ch < 128 && (font8x8_basic[ch][glyph_y] & (1u << glyph_x)))
                out[x] = 0xffff;
        }
    }
}
