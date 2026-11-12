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
    const char *text = caption_text(index);
    /* The existing 8x8 font's visible "203" ink is 22x7 pixels. Center the
       ink, rather than the padded 24x8 character cells, at an integer scale. */
    const unsigned scale = CAPTION_GLYPH_SCALE, ink_width = 22 * scale, ink_height = 7 * scale;
    memset(out, 0, width * sizeof(*out));
    if (width < ink_width || height < ink_height)
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
