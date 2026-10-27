#ifndef JPEG2PNG_PNG_H
#define JPEG2PNG_PNG_H

#include <stdio.h>
#include <stdint.h>
#include "jpeg2png.h"

// x0, y0: offset of the picture inside the coefficient planes; alpha: optional alpha channel (w*h)
void write_png(FILE *out, unsigned w, unsigned h, unsigned bits, struct coef *y, struct coef *cb, struct coef *cr, unsigned x0, unsigned y0, const uint8_t *alpha);

#endif
