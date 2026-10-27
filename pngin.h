#ifndef JPEG2PNG_PNGIN_H
#define JPEG2PNG_PNGIN_H

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "jpeg.h"

// Reading PNG files that contain a decoded JPEG (for example a JPEG that was
// converted to PNG or WebP at some point).  The 8x8 block grid, the
// quantization tables (IJG standard tables scaled by a quality factor) and
// the chroma subsampling are estimated from the picture itself, and the
// result is presented as if it had been read from a JPEG file so that the
// usual smoothing can run unchanged.

struct pngin_options {
        int quality;            // IJG quality factor 1..100, or 0 to estimate it
        int grid_x, grid_y;     // offset of the 8x8 block grid, or -1 to estimate it
        int subsampling;        // 0 = estimate, 1 = 4:4:4, 2 = 4:2:0
        float threshold;        // minimum detection score (0..1) to accept a JPEG structure
};

struct pngin_result {
        unsigned quality;       // estimated (or given) quality factor
        unsigned grid_x, grid_y;
        int subsampling;        // 1 = 4:4:4, 2 = 4:2:0
        float score;            // detection score, higher is more certain
        unsigned x0, y0;        // where the picture starts inside the (padded) coefficient planes
        uint8_t *alpha;         // alpha channel of the input (w*h) or NULL
};

void pngin_default_options(struct pngin_options *opt);

// true if the file starts with the PNG signature (the file position is restored)
bool pngin_is_png(FILE *in);

// read the PNG and fill jpeg (dimensions, components, coefficients, quantization
// tables and the initial image data in fdata, boxed).  Dies on errors, returns
// false when no JPEG structure was detected (and none was forced).
bool read_png_jpeg(FILE *in, struct jpeg *jpeg, const struct pngin_options *opt, struct pngin_result *res);

#endif
