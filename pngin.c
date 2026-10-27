#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>

#include <png.h>

#include "pngin.h"
#include "utils.h"
#include "box.h"
#include "ooura/dct.h"

// IJG standard quantization tables (natural order), scaled by the quality
// factor exactly like libjpeg's jpeg_quality_scaling / jpeg_add_quant_table
static const uint8_t ijg_luma[64] = {
        16,  11,  10,  16,  24,  40,  51,  61,
        12,  12,  14,  19,  26,  58,  60,  55,
        14,  13,  16,  24,  40,  57,  69,  56,
        14,  17,  22,  29,  51,  87,  80,  62,
        18,  22,  37,  56,  68, 109, 103,  77,
        24,  35,  55,  64,  81, 104, 113,  92,
        49,  64,  78,  87, 103, 121, 120, 101,
        72,  92,  95,  98, 112, 100, 103,  99
};

static const uint8_t ijg_chroma[64] = {
        17,  18,  24,  47,  99,  99,  99,  99,
        18,  21,  26,  66,  99,  99,  99,  99,
        24,  26,  56,  99,  99,  99,  99,  99,
        47,  66,  99,  99,  99,  99,  99,  99,
        99,  99,  99,  99,  99,  99,  99,  99,
        99,  99,  99,  99,  99,  99,  99,  99,
        99,  99,  99,  99,  99,  99,  99,  99,
        99,  99,  99,  99,  99,  99,  99,  99
};

static void ijg_table(unsigned quality, const uint8_t *base, uint16_t out[64]) {
        long scale = quality < 50 ? 5000 / (long)quality : 200 - 2 * (long)quality;
        for(unsigned i = 0; i < 64; i++) {
                long q = ((long)base[i] * scale + 50) / 100;
                out[i] = q < 1 ? 1 : q > 255 ? 255 : (uint16_t)q;
        }
}

void pngin_default_options(struct pngin_options *opt) {
        opt->quality = 0;
        opt->grid_x = -1;
        opt->grid_y = -1;
        opt->subsampling = 0;
        opt->threshold = 0.12f;
}

bool pngin_is_png(FILE *in) {
        static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
        uint8_t buf[8];
        long pos = ftell(in);
        size_t n = fread(buf, 1, 8, in);
        fseek(in, pos, SEEK_SET);
        return n == 8 && memcmp(buf, sig, 8) == 0;
}

// ---------------------------------------------------------------------------
// reading the PNG

static void png_die(png_struct *png_ptr, const char *error_msg) {
        (void)png_ptr;
        die("libpng error: %s", error_msg);
}

static void png_warn(png_struct *png_ptr, const char *warning_msg) {
        (void)png_ptr;
        (void)warning_msg;
}

// returns RGBA (4 bytes per pixel); *gray is set when the file is grayscale
static uint8_t *read_png_rgba(FILE *in, unsigned *w, unsigned *h, bool *has_alpha, bool *gray) {
        png_struct *png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, png_die, png_warn);
        if(!png_ptr) { die("could not initialize PNG read struct"); }
        png_info *info_ptr = png_create_info_struct(png_ptr);
        if(!info_ptr) { die("could not initialize PNG info struct"); }
        png_init_io(png_ptr, in);
        png_read_info(png_ptr, info_ptr);
        png_uint_32 width, height;
        int bit_depth, color_type;
        png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type, NULL, NULL, NULL);
        if(width == 0 || height == 0 || width > 65535 || height > 65535) { die("unsupported PNG dimensions"); }
        *gray = (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA);
        *has_alpha = (color_type & PNG_COLOR_MASK_ALPHA) != 0 || png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS);
        if(color_type == PNG_COLOR_TYPE_PALETTE) { png_set_palette_to_rgb(png_ptr); }
        if(color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) { png_set_expand_gray_1_2_4_to_8(png_ptr); }
        if(png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) { png_set_tRNS_to_alpha(png_ptr); }
        if(bit_depth == 16) { png_set_strip_16(png_ptr); }
        if(*gray) { png_set_gray_to_rgb(png_ptr); }
        png_set_filler(png_ptr, 0xff, PNG_FILLER_AFTER);
        png_read_update_info(png_ptr, info_ptr);
        if(png_get_rowbytes(png_ptr, info_ptr) != (size_t)width * 4) { die("unexpected PNG row size"); }

        uint8_t *data = malloc((size_t)width * height * 4);
        png_byte **rows = malloc(sizeof(*rows) * height);
        if(!data || !rows) { die("could not allocate memory for the PNG"); }
        for(unsigned i = 0; i < height; i++) {
                rows[i] = data + (size_t)i * width * 4;
        }
        png_read_image(png_ptr, rows);
        png_read_end(png_ptr, NULL);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        free(rows);
        *w = width;
        *h = height;
        return data;
}

// ---------------------------------------------------------------------------
// detection of the JPEG structure

// DCT coefficients of a sample of the 8x8 blocks of a plane at a grid offset
struct blockset {
        unsigned n;
        float *coef;    // n * 64, natural order
};

static void gather_blocks(const float *plane, unsigned w, unsigned h, unsigned ox, unsigned oy, unsigned max_blocks, struct blockset *bs) {
        unsigned nbx = (w - ox) / 8;
        unsigned nby = (h - oy) / 8;
        size_t total = (size_t)nbx * nby;
        size_t step = total > max_blocks ? (total + max_blocks - 1) / max_blocks : 1;
        bs->n = 0;
        for(size_t b = 0; b < total; b += step) {
                unsigned bx = (unsigned)(b % nbx);
                unsigned by = (unsigned)(b / nbx);
                float *a = &bs->coef[(size_t)bs->n * 64];
                for(unsigned y = 0; y < 8; y++) {
                        for(unsigned x = 0; x < 8; x++) {
                                a[y * 8 + x] = plane[(size_t)(oy + by * 8 + y) * w + ox + bx * 8 + x];
                        }
                }
                dct8x8s(a);
                bs->n++;
        }
}

// periodicity score of the coefficients for a quantization table:
// coefficients of a JPEG are multiples of the table entries
static float score_blocks(const struct blockset *bs, const uint16_t q[64]) {
        const unsigned cap = 300;       // limit the weight of the (noisy) low frequencies
        double total = 0.;
        double weight = 0.;
        for(unsigned k = 0; k < 64; k++) {
                if(q[k] < 2) { continue; }
                float half = q[k] * 0.5f;
                float f = (float)(2. * 3.14159265358979323846 / q[k]);
                double sum = 0.;
                unsigned n = 0;
                for(unsigned b = 0; b < bs->n; b++) {
                        float c = bs->coef[(size_t)b * 64 + k];
                        if(c > half || c < -half) {
                                sum += cos(f * c);
                                n++;
                        }
                }
                if(n < 30) { continue; }
                double wgt = n < cap ? n : cap;
                total += wgt * (sum / n);
                weight += wgt;
        }
        return weight > 0. ? (float)(total / weight) : -1.f;
}

static float best_quality(const struct blockset *bs, const uint8_t *base, unsigned lo, unsigned hi, unsigned step, unsigned *best_q) {
        float best = -2.f;
        *best_q = lo;
        for(unsigned q = lo; q <= hi; q += step) {
                uint16_t table[64];
                ijg_table(q, base, table);
                float s = score_blocks(bs, table);
                if(s > best) {
                        best = s;
                        *best_q = q;
                }
        }
        return best;
}

// find the grid offset and quality of a plane; returns the score
static float detect(const float *plane, unsigned w, unsigned h, const uint8_t *base, int fixed_q, int fixed_x, int fixed_y, unsigned *qual, unsigned *gx, unsigned *gy) {
        struct blockset bs;
        bs.coef = alloc_simd(sizeof(float) * 64 * 6000);
        float best = -2.f;
        *gx = 0;
        *gy = 0;
        *qual = fixed_q > 0 ? (unsigned)fixed_q : 50;
        if(w < 16 || h < 16) {
                free_simd(bs.coef);
                return -1.f;
        }
        // coarse search over the grid offsets
        for(unsigned oy = 0; oy < 8; oy++) {
                if(fixed_y >= 0 && oy != (unsigned)fixed_y) { continue; }
                for(unsigned ox = 0; ox < 8; ox++) {
                        if(fixed_x >= 0 && ox != (unsigned)fixed_x) { continue; }
                        gather_blocks(plane, w, h, ox, oy, 2000, &bs);
                        unsigned q;
                        float s;
                        if(fixed_q > 0) {
                                uint16_t table[64];
                                ijg_table((unsigned)fixed_q, base, table);
                                s = score_blocks(&bs, table);
                                q = (unsigned)fixed_q;
                        } else {
                                s = best_quality(&bs, base, 20, 95, 5, &q);
                        }
                        if(s > best) {
                                best = s;
                                *gx = ox;
                                *gy = oy;
                                *qual = q;
                        }
                }
        }
        // refine the quality at the best offset
        gather_blocks(plane, w, h, *gx, *gy, 6000, &bs);
        if(fixed_q > 0) {
                uint16_t table[64];
                ijg_table((unsigned)fixed_q, base, table);
                best = score_blocks(&bs, table);
        } else {
                best = best_quality(&bs, base, 1, 100, 1, qual);
        }
        free_simd(bs.coef);
        return best;
}

// ---------------------------------------------------------------------------
// building the coefficient planes

// fill a coefficient plane: levels = round(coefficient / quant), fdata = the observed samples (boxed)
static void make_coef(struct coef *coef, const float *plane, unsigned w, unsigned h, unsigned samp, const uint16_t quant[64]) {
        coef->w = w;
        coef->h = h;
        coef->w_samp = samp;
        coef->h_samp = samp;
        memcpy(coef->quant_table, quant, sizeof(coef->quant_table));
        unsigned blocks = (w / 8) * (h / 8);
        coef->data = malloc(sizeof(*coef->data) * blocks * 64);
        coef->fdata = alloc_simd(sizeof(float) * w * h);
        if(!coef->data) { die("could not allocate memory for coefs"); }
        float *tmp = alloc_simd(sizeof(float) * w * h);
        memcpy(tmp, plane, sizeof(float) * w * h);
        box(tmp, coef->fdata, w, h);
        for(unsigned i = 0; i < blocks; i++) {
                float a[64];
                memcpy(a, &coef->fdata[i * 64], sizeof(a));
                dct8x8s(a);
                for(unsigned j = 0; j < 64; j++) {
                        float level = roundf(a[j] / quant[j]);
                        if(level > 2047.f) { level = 2047.f; }
                        if(level < -2047.f) { level = -2047.f; }
                        coef->data[i * 64 + j] = (int16_t)level;
                }
        }
        free_simd(tmp);
}

// copy a plane into a padded plane, replicating the edges
static float *pad_plane(const float *src, unsigned w, unsigned h, unsigned left, unsigned top, unsigned pw, unsigned ph) {
        float *dst = alloc_simd(sizeof(float) * pw * ph);
        for(unsigned y = 0; y < ph; y++) {
                unsigned sy = y < top ? 0 : y - top >= h ? h - 1 : y - top;
                for(unsigned x = 0; x < pw; x++) {
                        unsigned sx = x < left ? 0 : x - left >= w ? w - 1 : x - left;
                        dst[(size_t)y * pw + x] = src[(size_t)sy * w + sx];
                }
        }
        return dst;
}

// 2x2 means of a plane (dimensions must be even)
static float *downsample(const float *src, unsigned w, unsigned h) {
        float *dst = alloc_simd(sizeof(float) * (w / 2) * (h / 2));
        for(unsigned y = 0; y < h / 2; y++) {
                for(unsigned x = 0; x < w / 2; x++) {
                        const float *s = &src[(size_t)(2 * y) * w + 2 * x];
                        dst[(size_t)y * (w / 2) + x] = 0.25f * (s[0] + s[1] + s[w] + s[w + 1]);
                }
        }
        return dst;
}

// the "fancy" (triangle) chroma upsampling of libjpeg: 9/16, 3/16, 3/16, 1/16
static void upsample(const float *src, unsigned sw, unsigned sh, float *dst) {
        unsigned w = 2 * sw, h = 2 * sh;
        for(unsigned y = 0; y < h; y++) {
                unsigned j = y / 2;
                unsigned nj = (y & 1) ? (j + 1 < sh ? j + 1 : j) : (j > 0 ? j - 1 : 0);
                for(unsigned x = 0; x < w; x++) {
                        unsigned i = x / 2;
                        unsigned ni = (x & 1) ? (i + 1 < sw ? i + 1 : i) : (i > 0 ? i - 1 : 0);
                        dst[(size_t)y * w + x] = (9.f * src[(size_t)j * sw + i] + 3.f * src[(size_t)j * sw + ni]
                                                  + 3.f * src[(size_t)nj * sw + i] + src[(size_t)nj * sw + ni]) * (1.f / 16.f);
                }
        }
}

// recover the chroma samples from a fancy upsampled plane (a few Van Cittert iterations)
static float *deconvolve(const float *full, unsigned w, unsigned h) {
        float *s = downsample(full, w, h);
        float *up = alloc_simd(sizeof(float) * w * h);
        float *diff = alloc_simd(sizeof(float) * w * h);
        for(unsigned it = 0; it < 3; it++) {
                upsample(s, w / 2, h / 2, up);
                for(size_t i = 0; i < (size_t)w * h; i++) {
                        diff[i] = full[i] - up[i];
                }
                float *corr = downsample(diff, w, h);
                for(size_t i = 0; i < (size_t)(w / 2) * (h / 2); i++) {
                        s[i] += corr[i];
                }
                free_simd(corr);
        }
        free_simd(up);
        free_simd(diff);
        return s;
}

// A JPEG decoder clips the IDCT output to 0..255, so blocks with saturated
// pixels no longer have coefficients on the quantization lattice.  Recover
// them by alternating between the observed unsaturated pixels and the lattice
// (the saturated pixels are free), so that the levels and the starting point
// are consistent with the JPEG.
static void unclip(float *plane, const uint8_t *mask, unsigned w, unsigned h, const uint16_t q[64]) {
        for(unsigned by = 0; by < h / 8; by++) {
                for(unsigned bx = 0; bx < w / 8; bx++) {
                        bool any = false;
                        for(unsigned y = 0; y < 8 && !any; y++) {
                                for(unsigned x = 0; x < 8; x++) {
                                        if(mask[(size_t)(by * 8 + y) * w + bx * 8 + x]) { any = true; break; }
                                }
                        }
                        if(!any) { continue; }
                        float a[64], obs[64];
                        uint8_t m[64];
                        for(unsigned y = 0; y < 8; y++) {
                                for(unsigned x = 0; x < 8; x++) {
                                        obs[y * 8 + x] = plane[(size_t)(by * 8 + y) * w + bx * 8 + x];
                                        m[y * 8 + x] = mask[(size_t)(by * 8 + y) * w + bx * 8 + x];
                                }
                        }
                        memcpy(a, obs, sizeof(a));
                        for(unsigned it = 0; it < 24; it++) {
                                float cf[64];
                                memcpy(cf, a, sizeof(cf));
                                dct8x8s(cf);
                                for(unsigned k = 0; k < 64; k++) {
                                        cf[k] = roundf(cf[k] / q[k]) * q[k];
                                }
                                idct8x8s(cf);
                                for(unsigned k = 0; k < 64; k++) {
                                        // a pixel clipped at the top must have been at least that bright,
                                        // one clipped at the bottom at most that dark
                                        if(m[k] == 2) {
                                                a[k] = cf[k] > obs[k] ? cf[k] : obs[k];
                                        } else if(m[k] == 1) {
                                                a[k] = cf[k] < obs[k] ? cf[k] : obs[k];
                                        } else if(m[k]) {
                                                a[k] = cf[k];
                                        } else {
                                                a[k] = obs[k];
                                        }
                                }
                        }
                        for(unsigned y = 0; y < 8; y++) {
                                for(unsigned x = 0; x < 8; x++) {
                                        plane[(size_t)(by * 8 + y) * w + bx * 8 + x] = a[y * 8 + x];
                                }
                        }
                }
        }
}

// pad a mask like pad_plane
static uint8_t *pad_mask(const uint8_t *src, unsigned w, unsigned h, unsigned left, unsigned top, unsigned pw, unsigned ph) {
        uint8_t *dst = malloc((size_t)pw * ph);
        if(!dst) { die("could not allocate memory for the mask"); }
        for(unsigned y = 0; y < ph; y++) {
                unsigned sy = y < top ? 0 : y - top >= h ? h - 1 : y - top;
                for(unsigned x = 0; x < pw; x++) {
                        unsigned sx = x < left ? 0 : x - left >= w ? w - 1 : x - left;
                        dst[(size_t)y * pw + x] = src[(size_t)sy * w + sx];
                }
        }
        return dst;
}

// 2x2 "any" of a mask
static uint8_t *downsample_mask(const uint8_t *src, unsigned w, unsigned h) {
        uint8_t *dst = malloc((size_t)(w / 2) * (h / 2));
        if(!dst) { die("could not allocate memory for the mask"); }
        for(unsigned y = 0; y < h / 2; y++) {
                for(unsigned x = 0; x < w / 2; x++) {
                        const uint8_t *s = &src[(size_t)(2 * y) * w + 2 * x];
                        dst[(size_t)y * (w / 2) + x] = (s[0] | s[1] | s[w] | s[w + 1]) ? 3 : 0;
                }
        }
        return dst;
}

#define ROUND_UP(x, n) (((x) + (n) - 1) / (n) * (n))

bool read_png_jpeg(FILE *in, struct jpeg *jpeg, const struct pngin_options *opt, struct pngin_result *res) {
        unsigned w, h;
        bool has_alpha, gray;
        uint8_t *rgba = read_png_rgba(in, &w, &h, &has_alpha, &gray);
        memset(res, 0, sizeof(*res));

        // JPEG YCbCr (level shifted, as the DCT sees it)
        unsigned ncomp = gray ? 1 : 3;
        float *planes[3];
        for(unsigned c = 0; c < ncomp; c++) {
                planes[c] = alloc_simd(sizeof(float) * w * h);
        }
        uint8_t *saturated = malloc((size_t)w * h);
        if(!saturated) { die("could not allocate memory for the mask"); }
        for(size_t i = 0; i < (size_t)w * h; i++) {
                float r = rgba[4 * i], g = rgba[4 * i + 1], b = rgba[4 * i + 2];
                {
                        bool low = rgba[4 * i] == 0 || rgba[4 * i + 1] == 0 || rgba[4 * i + 2] == 0;
                        bool high = rgba[4 * i] == 255 || rgba[4 * i + 1] == 255 || rgba[4 * i + 2] == 255;
                        saturated[i] = (low ? 1 : 0) | (high ? 2 : 0);
                }
                planes[0][i] = 0.299f * r + 0.587f * g + 0.114f * b - 128.f;
                if(!gray) {
                        planes[1][i] = -0.168736f * r - 0.331264f * g + 0.5f * b;
                        planes[2][i] = 0.5f * r - 0.418688f * g - 0.081312f * b;
                }
        }
        if(has_alpha) {
                res->alpha = malloc((size_t)w * h);
                if(!res->alpha) { die("could not allocate memory for the alpha channel"); }
                for(size_t i = 0; i < (size_t)w * h; i++) {
                        res->alpha[i] = rgba[4 * i + 3];
                }
        }
        free(rgba);

        // luma: grid offset and quality
        unsigned quality, gx, gy;
        float score = detect(planes[0], w, h, ijg_luma, opt->quality, opt->grid_x, opt->grid_y, &quality, &gx, &gy);
        res->score = score;
        bool forced = opt->quality > 0;
        if(score < opt->threshold && !forced) {
                for(unsigned c = 0; c < ncomp; c++) { free_simd(planes[c]); }
                free(saturated);
                free(res->alpha);
                res->alpha = NULL;
                return false;
        }
        res->quality = quality;
        res->grid_x = gx;
        res->grid_y = gy;

        // chroma: 4:2:0 or 4:4:4, and the phase of the 16x16 macroblock grid
        int subsampling = opt->subsampling;
        unsigned mcu_shift_x = 0, mcu_shift_y = 0;      // extra padding so that chroma blocks align (4:2:0)
        bool deconv = false;                            // undo the decoder's fancy chroma upsampling
        uint16_t qc[64];
        ijg_table(quality, ijg_chroma, qc);
        if(ncomp == 3) {
                // the planes are padded so that the luma blocks start at 0; try the two
                // possible chroma block phases in each direction
                unsigned pl = (8 - gx) % 8, pt = (8 - gy) % 8;
                float best420 = -2.f, best444 = -2.f;
                unsigned bsx = 0, bsy = 0;
                unsigned pw = ROUND_UP(w + pl + 8, 16), ph = ROUND_UP(h + pt + 8, 16);
                for(unsigned sy = 0; sy < 16; sy += 8) {
                        for(unsigned sx = 0; sx < 16; sx += 8) {
                                for(unsigned dc = 0; dc < 2; dc++) {
                                        float s = 0.f;
                                        for(unsigned c = 1; c < 3; c++) {
                                                float *padded = pad_plane(planes[c], w, h, pl + sx, pt + sy, pw, ph);
                                                float *half = dc ? deconvolve(padded, pw, ph) : downsample(padded, pw, ph);
                                                struct blockset bs;
                                                bs.coef = alloc_simd(sizeof(float) * 64 * 6000);
                                                gather_blocks(half, pw / 2, ph / 2, 0, 0, 6000, &bs);
                                                s += score_blocks(&bs, qc);
                                                free_simd(bs.coef);
                                                free_simd(half);
                                                free_simd(padded);
                                        }
                                        if(s > best420) {
                                                best420 = s;
                                                bsx = sx;
                                                bsy = sy;
                                                deconv = dc != 0;
                                        }
                                }
                        }
                }
                for(unsigned c = 1; c < 3; c++) {
                        float *padded = pad_plane(planes[c], w, h, pl, pt, pw, ph);
                        struct blockset bs;
                        bs.coef = alloc_simd(sizeof(float) * 64 * 6000);
                        gather_blocks(padded, pw, ph, 0, 0, 6000, &bs);
                        best444 += score_blocks(&bs, qc) + (c == 1 ? 2.f : 0.f);
                        free_simd(bs.coef);
                        free_simd(padded);
                }
                if(subsampling == 0) {
                        // chroma is usually smooth and carries little evidence: only switch to
                        // 4:4:4 when it is clearly better than the (much more common) 4:2:0
                        subsampling = (best444 > best420 + 0.1f && best444 > 0.1f) ? 1 : 2;
                }
                if(subsampling == 2) {
                        mcu_shift_x = bsx;
                        mcu_shift_y = bsy;
                }
        } else {
                subsampling = 1;
        }
        res->subsampling = subsampling;

        // padded planes: the blocks (and macroblocks) start at 0
        unsigned left = (8 - gx) % 8 + mcu_shift_x;
        unsigned top = (8 - gy) % 8 + mcu_shift_y;
        unsigned align = subsampling == 2 ? 16 : 8;
        unsigned pw = ROUND_UP(w + left, align);
        unsigned ph = ROUND_UP(h + top, align);
        res->x0 = left;
        res->y0 = top;

        uint16_t ql[64];
        ijg_table(quality, ijg_luma, ql);
        jpeg->w = w;
        jpeg->h = h;
        jpeg->c = ncomp;
        uint8_t *pmask = pad_mask(saturated, w, h, left, top, pw, ph);
        free(saturated);
        for(unsigned c = 0; c < ncomp; c++) {
                float *padded = pad_plane(planes[c], w, h, left, top, pw, ph);
                if(c == 0 || subsampling == 1) {
                        if(c > 0) {
                                // the clipping direction of the chroma is unknown
                                for(size_t i = 0; i < (size_t)pw * ph; i++) { if(pmask[i]) { pmask[i] = 3; } }
                        }
                        unclip(padded, pmask, pw, ph, c == 0 ? ql : qc);
                        make_coef(&jpeg->coefs[c], padded, pw, ph, 1, c == 0 ? ql : qc);
                } else {
                        float *half = deconv ? deconvolve(padded, pw, ph) : downsample(padded, pw, ph);
                        uint8_t *hmask = downsample_mask(pmask, pw, ph);
                        unclip(half, hmask, pw / 2, ph / 2, qc);
                        make_coef(&jpeg->coefs[c], half, pw / 2, ph / 2, 2, qc);
                        free(hmask);
                        free_simd(half);
                }
                free_simd(padded);
                free_simd(planes[c]);
        }
        free(pmask);
        return true;
}
