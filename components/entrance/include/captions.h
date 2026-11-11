#pragma once
#include <stdint.h>
#define CAPTION_PERIOD_MS 2000
#define CAPTION_GLYPH_PIXELS 10
#define CAPTION_HEADER_LANDSCAPE 40
#define CAPTION_HEADER_PORTRAIT 50
const char *caption_text(unsigned index);
void caption_row(unsigned index, unsigned width, unsigned height, unsigned y, uint16_t *out);
