#include <stdio.h>
#include <stdlib.h>
#include <png.h>

#include "png.h"
#include "utils.h"

// png error handler
static noreturn void png_die(png_struct *png_ptr, const char *error_msg){
        (void)png_ptr;
        die("libpng error: %s", error_msg);
}

// clamp to RGB range
static float clamp(float x) {
        return CLAMP(x, 0., 255.);
}

// write image to PNG file
void write_png(FILE *out, unsigned w, unsigned h, unsigned bits, struct coef *y, struct coef *cb, struct coef *cr, unsigned x0, unsigned y0, const uint8_t *alpha) {
        // initialize png
        ASSUME(bits == 8 || bits == 16);
        png_struct *png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
        if(!png_ptr) { die("could not initialize PNG write struct"); }
        png_info *info_ptr = png_create_info_struct(png_ptr);
        if (!info_ptr) { die("could not initialize PNG info struct"); }
        void *error = png_get_error_ptr(png_ptr);
        png_set_error_fn(png_ptr, error, png_die, NULL);
        png_init_io(png_ptr, out);
        unsigned channels = alpha ? 4 : 3;
        png_set_IHDR(png_ptr, info_ptr, w, h, bits, alpha ? PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
        png_write_info(png_ptr, info_ptr);
        unsigned depth = bits / 8;
        png_byte *image_data = malloc(sizeof(png_byte) * h * w * channels * depth);
        if(!image_data) { die("could not allocate image data");}

        // write png lines
        for(unsigned i = 0; i < h; i++) {
                for(unsigned j = 0; j < w; j++) {
                        float yi = *p(y->fdata, x0 + j, y0 + i, y->w, y->h);
                        float cbi = cb ? *p(cb->fdata, x0 + j, y0 + i, cb->w, cb->h) : 0.0;
                        float cri = cr ? *p(cr->fdata, x0 + j, y0 + i, cr->w, cr->h) : 0.0;

                        // YCbCr -> RGB
                        float bitfactor = (1 << bits) / 256.;
                        unsigned r = clamp(yi + 1.402 * cri) * bitfactor;
                        unsigned g = clamp(yi - 0.34414 * cbi - 0.71414 * cri) * bitfactor;
                        unsigned b = clamp(yi + 1.772 * cbi) * bitfactor;

                        // write to png line
                        png_byte *here = &image_data[((size_t)i * w + j) * channels * depth];
                        unsigned a = alpha ? alpha[(size_t)i * w + j] * (bits == 8 ? 1 : 257) : 0;
                        if(bits == 8) {
                                here[0] = r & 0xFF;
                                here[1] = g & 0xFF;
                                here[2] = b & 0xFF;
                                if(alpha) { here[3] = a & 0xFF; }
                        } else {
                                here[0] = (r >> 8) & 0xFF;
                                here[1] = r & 0xFF;
                                here[2] = (g >> 8) & 0xFF;
                                here[3] = g & 0xFF;
                                here[4] = (b >> 8) & 0xFF;
                                here[5] = b & 0xFF;
                                if(alpha) {
                                        here[6] = (a >> 8) & 0xFF;
                                        here[7] = a & 0xFF;
                                }
                        }
                }
        }

        // pointers to png lines
        png_byte **rows = malloc(sizeof(*rows) * h);
        if(!rows) { die("allocation failure"); }
        for(unsigned i = 0; i < h; i++) {
                rows[i] = &image_data[(size_t)i * w * channels * depth];
        }
        // write
        png_write_image(png_ptr, rows);
        free(rows);
        png_write_end(png_ptr, info_ptr);
        free(image_data);
        png_destroy_write_struct(&png_ptr, &info_ptr);
}
