#pragma once
#include <stdint.h>
#define CAPTION_GLYPH_SCALE 4
#define CAPTION_HEADER_LANDSCAPE 40
#define CAPTION_HEADER_PORTRAIT 50
const char *caption_text(unsigned index);
void caption_row(unsigned index, unsigned width, unsigned height, unsigned y, uint16_t *out);
