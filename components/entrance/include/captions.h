#pragma once
#include <stdint.h>
#define CAPTION_GLYPH_SCALE 4
#define CAPTION_HEADER_LANDSCAPE 40
#define CAPTION_HEADER_PORTRAIT 50
const char *caption_text(unsigned index);
void caption_row(unsigned index, unsigned width, unsigned height, unsigned y, uint16_t *out);
#define STUDY_CAPTION_PERIOD_US 1350000ULL
unsigned study_caption_index(uint64_t elapsed_us);
const char *study_caption_text(unsigned index);
void room_number_row(unsigned scale, unsigned width, unsigned height, unsigned y, uint16_t *out);
void study_caption_row(unsigned index, unsigned width, unsigned y, uint16_t *out);
