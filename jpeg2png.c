#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdnoreturn.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#ifdef _OPENMP
#include <omp.h>
#endif

#include "gopt/gopt.h"

#include "jpeg2png.h"
#include "utils.h"
#include "jpeg.h"
#include "png.h"
#include "pngin.h"
#include "box.h"
#include "compute.h"
#include "logger.h"
#include "progressbar.h"
#include "fp_exceptions.h"

#define JPEG2PNG_VERSION "1.02"
static const float default_weight = 0.3;
static const float default_pweight = 0.001;
static const unsigned default_iterations = 50;

// Parse the whole argument so malformed lists and trailing text are rejected.
static unsigned parse_weights(const char *arg, float values[3], float maximum, const char *name, const char *example) {
        unsigned n = 0;
        while(n < 3) {
                char *end;
                errno = 0;
                float value = strtof(arg, &end);
                if(end == arg) { break; }
                if(errno == ERANGE || !isfinite(value) || value < 0.0f || value > maximum) {
                        die("%s values must be finite and between 0.0 and %g, got %s", name, maximum, arg);
                }
                values[n++] = value;
                while(isspace((unsigned char)*end)) { end++; }
                if(*end == '\0' && (n == 1 || n == 3)) { return n; }
                if(*end != ',') { break; }
                arg = end + 1;
        }
        die("invalid %s format - use a single value or comma-separated triplet (e.g., %s)", name, example);
}

static unsigned parse_counts(const char *arg, unsigned *values, unsigned maximum, unsigned max_values, const char *name, const char *example) {
        unsigned n = 0;
        while(n < max_values) {
                while(isspace((unsigned char)*arg)) { arg++; }
                char *end;
                errno = 0;
                unsigned long value = strtoul(arg, &end, 10);
                if(end == arg) { break; }
                if(*arg == '-' || errno == ERANGE || value == 0 || value > maximum) {
                        die("%s must be between 1 and %u, got %s", name, maximum, arg);
                }
                values[n++] = (unsigned)value;
                while(isspace((unsigned char)*end)) { end++; }
                if(*end == '\0' && (n == 1 || n == 3)) { return n; }
                if(*end != ',') { break; }
                arg = end + 1;
        }
        die("invalid %s format - use %s", name, example);
}

static bool extension_equals(const char *extension, const char *expected) {
        while(*extension && *expected) {
                if(tolower((unsigned char)*extension++) != *expected++) { return false; }
        }
        return *extension == '\0' && *expected == '\0';
}

// print usage information and die
noreturn static void usage() {
        printf(
                "usage: jpeg2png picture.jpg ... [-o picture.png] ... [flags...]\n"
                "\n"
                "-o picture.png\n"
                "--output picture.png\n"
                "\tpicture.png is the file name of the output file\n"
                "\tthe output file will be overwritten if this flag is used\n"
                "\tmust be specified either zero times or once for every input file\n"
                "\tdefault value: original file name with the extension .png\n"
                "\n");
        printf(
                "-f\n"
                "--force\n"
                "\toverwrite output files even when not given explicit file names\n"
                "\n");
        printf(
                "-w weight[,weight_cb,weight_cr]\n"
                "--second-order-weight weight[,weight_cb,weight_cr]\n"
                "\tweight is a floating point number for TGV weight alpha_1\n"
                "\tallowed range: 0.0..10.0 (finite values only)\n"
                "\thigher values give smoother transitions with less staircasing\n"
                "\ta value of 1.0 means equivalent weight to the first order weight\n"
                "\ta value of 0.0 means plain Total Variation, and gives a speed boost\n"
                "\tweights for the chroma components always default to 0.\n"
                "\tdefault value: %g\n"
                "\n", default_weight);
        printf(
                "-p pweight[,pweight_cb,pweight_cr]\n"
                "--probability-weight pweight[,pweight_cb,pweight_cr]\n"
                "\tpweight is a floating point number for DCT coefficient distance weight\n"
                "\tallowed range: 0.0..1.0 (finite values only)\n"
                "\thigher values make the result more similar to the source JPEG\n"
                "\ta value of 1.0 means about equivalent weight to the first order weight\n"
                "\ta value of 0.0 means to ignore this and gives a speed boost\n"
                "\tweights for the chroma components default to the luma weight\n"
                "\tdefault value: %g\n"
                "\n", default_pweight);
        printf(
                "-i iterations[,iterations_cb,iterations_cr]\n"
                "--iterations iterations[,iterations_cb,iterations_cr]\n"
                "\titerations is an integer for the number of optimization steps\n"
                "\tallowed range: 1..10000\n"
                "\thigher values give better results but take more time\n"
                "\titerations for the chroma components default to the luma iterations\n"
                "\tdefault value: %d\n"
                "\n", default_iterations);
        printf(
                "-q\n"
                "--quiet\n"
                "\tdon't show the progress bar\n"
                "\n");
        printf(
                "-s\n"
                "--separate-components\n"
                "\tseparately optimize components\n"
                "\tthis is faster and makes multithreading more effective\n"
                "\thowever the edges of different components can be different\n"
                "\n");
        printf(
                "-t threads\n"
                "--threads threads\n"
#ifndef _OPENMP
                "\t*this version was compiled without support for threads*\n"
                "\n"
#endif
                "\tthreads is a positive integer for the maximum number of threads used\n"
                "\tallowed range: 1..256\n"
                "\tequivalent to setting the environment variable OMP_NUM_THREADS\n"
                "\tdefault: number of CPUs\n"
                "\n");
        printf(
                "-J quality\n"
                "--jpeg-quality quality\n"
                "\tPNG input: assume this IJG JPEG quality factor (1..100) instead of estimating it\n"
                "\tPNG files are read as pictures that contain a decoded JPEG (for example\n"
                "\ta JPEG that was converted to PNG or WebP); the 8x8 grid, the quantization\n"
                "\ttables and the chroma subsampling are estimated from the picture itself\n"
                "\n"
                "--jpeg-grid x,y\n"
                "\tPNG input: assume this offset of the 8x8 block grid (0..7) instead of estimating it\n"
                "\n"
                "--jpeg-subsampling 444|420\n"
                "\tPNG input: assume this chroma subsampling instead of estimating it\n"
                "\n"
                "-1\n"
                "--16-bits-png\n"
                "\toutput PNG with 16 bits color depth instead of the usual 8 bits\n"
                "\tyou should use a high number of iterations when using this option\n"
                "\n");
        printf(
                "-c csv_log\n"
                "--csv_log csv_log\n"
                "\tcsv_log is a file name for the optimization log\n"
                "\tdefault: none\n"
                "\n");
        printf(
                "-h\n"
                "--help\n"
                "\tdisplay this help text and exit\n"
                "\n");
        printf(
                "-V\n"
                "--version\n"
                "\tdisplay version information and exit\n"
                );
        exit(EXIT_FAILURE);
}

static struct pngin_options png_options;

// decode a single JPEG file smoothly
void decode_file(const char* infile, const char *outfile, unsigned iterations[3], float weights[3], float pweights[3], unsigned png_bits, bool all_together, struct progressbar *pb, struct logger *plog) {
        // decode jpg normally
        FILE *in = fopen(infile, "rb");
        if(!in) { die_perror("could not open input file `%s`", infile); }
        struct jpeg jpeg;
        struct pngin_result pngres = {0};
        bool is_png = pngin_is_png(in);
        if(is_png) {
                // a PNG with a JPEG inside: estimate the JPEG's parameters
                if(!read_png_jpeg(in, &jpeg, &png_options, &pngres)) {
                        die("no JPEG structure found in `%s` (score %.2f); use -J to force a quality factor", infile, pngres.score);
                }
                if(pb) { progressbar_clear(pb); }
                fprintf(stderr, "jpeg2png: %s: JPEG grid at (%u,%u), quality ~%u, chroma %s, score %.2f\n", infile,
                        pngres.grid_x, pngres.grid_y, pngres.quality, pngres.subsampling == 2 ? "4:2:0" : "4:4:4", pngres.score);
        } else {
                read_jpeg(in, &jpeg);
        }
        fclose(in);
        for(unsigned c = 0; c < jpeg.c && !is_png; c++) {
                struct coef *coef = &jpeg.coefs[c];
                decode_coefficients(coef);
        }
        for(unsigned i = 0; i < jpeg.c; i++) {
                struct coef *coef = &jpeg.coefs[i];
                float *temp = alloc_simd(sizeof(float) * coef->h * coef->w);

                unbox(coef->fdata, temp, coef->w, coef->h);

                free_simd(coef->fdata);
                coef->fdata = temp;
        }

        // smooth
        if(all_together && jpeg.c == 3) {
                plog->channel = 3;
                compute(3, jpeg.coefs, plog, pb, weights[0], pweights, iterations[0]);
        } else {
                struct logger log = *plog;
                OPENMP(parallel for schedule(dynamic) firstprivate(log))
                for(unsigned i = 0; i < jpeg.c; i++) {
                        log.channel = i;
                        struct coef *coef = &jpeg.coefs[i];
                        compute(1, coef, &log, pb, weights[i], &pweights[i], iterations[i]);
                }
        }

        // fixup luma range
        struct coef *coef = &jpeg.coefs[0];
        for(size_t i = 0; i < (size_t)coef->h * coef->w; i++) {
                coef->fdata[i] += 128.;
        }

        // write png
        FILE *out = fopen(outfile, "wb");
        if(!out) { die_perror("could not open output file `%s`", outfile); }
        if (jpeg.c == 3)
          write_png(out, jpeg.w, jpeg.h, png_bits, &jpeg.coefs[0], &jpeg.coefs[1], &jpeg.coefs[2], pngres.x0, pngres.y0, pngres.alpha);
        else
          write_png(out, jpeg.w, jpeg.h, png_bits, &jpeg.coefs[0], NULL, NULL, pngres.x0, pngres.y0, pngres.alpha);
        fclose(out);

        // clean up
        for(unsigned i = 0; i < jpeg.c; i++) {
                free_simd(jpeg.coefs[i].fdata);
                free(jpeg.coefs[i].data);
        }
        free(pngres.alpha);
}


struct progressbar *main_progressbar;

int main(int argc, const char **argv) {
        enable_fp_exceptions();

        // define command line flags
        void *options = gopt_sort(&argc, argv, gopt_start(
                gopt_option('h', GOPT_NOARG, gopt_shorts('h','?'), gopt_longs("help")),
                gopt_option('V', GOPT_NOARG, gopt_shorts('V'), gopt_longs("version")),
                gopt_option('o', GOPT_ARG | GOPT_REPEAT, gopt_shorts('o'), gopt_longs("output")),
                gopt_option('f', GOPT_NOARG, gopt_shorts('f'), gopt_longs("force")),
                gopt_option('c', GOPT_ARG, gopt_shorts('c'), gopt_longs("csv-log")),
                gopt_option('t', GOPT_ARG, gopt_shorts('t'), gopt_longs("threads")),
                gopt_option('q', GOPT_NOARG, gopt_shorts('q'), gopt_longs("quiet")),
                gopt_option('s', GOPT_NOARG, gopt_shorts('s'), gopt_longs("separate-components")),
                gopt_option('1', GOPT_NOARG, gopt_shorts('1'), gopt_longs("16-bits-png")),
                gopt_option('J', GOPT_ARG, gopt_shorts('J'), gopt_longs("jpeg-quality")),
                gopt_option('G', GOPT_ARG, gopt_shorts(0), gopt_longs("jpeg-grid")),
                gopt_option('S', GOPT_ARG, gopt_shorts(0), gopt_longs("jpeg-subsampling")),
                gopt_option('i', GOPT_ARG, gopt_shorts('i'), gopt_longs("iterations")),
                gopt_option('p', GOPT_ARG, gopt_shorts('p'), gopt_longs("probability-weight")),
                gopt_option('w', GOPT_ARG, gopt_shorts('w'), gopt_longs("second-order-weight"))));
        // parse command line flags
        if(gopt(options, 'V')) {
                printf("jpeg2png version "JPEG2PNG_VERSION" licensed GPLv3+\n");
                exit(EXIT_FAILURE);
        }
        if(argc < 2 || gopt(options, 'h')) {
                usage();
        }

        bool all_together = ! gopt(options, 's');

        const char *arg_string;
        float weights[3] = {default_weight, 0., 0.};
        if(gopt_arg(options, 'w', &arg_string)) {
                unsigned n = parse_weights(arg_string, weights, 10.0f, "weight", "'0.3' or '0.3,0.1,0.1'");
                if(n == 3) {
                        if(all_together) {
                                die("different weights are only possible when using separated components");
                        }
                }
        }
        float pweights[3] = {default_pweight, default_pweight, default_pweight};
        if(gopt_arg(options, 'p', &arg_string)) {
                unsigned n = parse_weights(arg_string, pweights, 1.0f, "probability weight", "'0.001' or '0.001,0.002,0.001'");
                if(n == 1) {
                        pweights[1] = pweights[0];
                        pweights[2] = pweights[0];
                }
        }
        unsigned iterations[3] = {default_iterations, default_iterations, default_iterations};
        if(gopt_arg(options, 'i', &arg_string)) {
                unsigned n = parse_counts(arg_string, iterations, 10000, 3, "iteration count", "a single value or comma-separated triplet (e.g., '50' or '50,30,30')");
                if(n == 3) {
                        if(all_together) {
                                die("different iteration counts are only possible when using separated components");
                        }
                } else if(n == 1) {
                        iterations[1] = iterations[0];
                        iterations[2] = iterations[0];
                }
        }

        if(gopt_arg(options, 't', &arg_string)) {
#ifdef _OPENMP
                unsigned threads;
                parse_counts(arg_string, &threads, 256, 1, "thread count", "a positive integer (e.g., '4')");
                omp_set_num_threads(threads);
#else
                die("this version is compiled without support for threads");
#endif
        }

        pngin_default_options(&png_options);
        if(gopt_arg(options, 'J', &arg_string)) {
                int n = sscanf(arg_string, "%d", &png_options.quality);
                if(n != 1 || png_options.quality < 1 || png_options.quality > 100) {
                        die("invalid jpeg quality");
                }
        }
        if(gopt_arg(options, 'G', &arg_string)) {
                int n = sscanf(arg_string, "%d,%d", &png_options.grid_x, &png_options.grid_y);
                if(n != 2 || png_options.grid_x < 0 || png_options.grid_x > 7 || png_options.grid_y < 0 || png_options.grid_y > 7) {
                        die("invalid jpeg grid offset");
                }
        }
        if(gopt_arg(options, 'S', &arg_string)) {
                if(strcmp(arg_string, "444") == 0) {
                        png_options.subsampling = 1;
                } else if(strcmp(arg_string, "420") == 0) {
                        png_options.subsampling = 2;
                } else {
                        die("invalid jpeg subsampling, use 444 or 420");
                }
        }

        FILE *csv_log = NULL;
        if(gopt_arg(options, 'c', &arg_string)) {
                csv_log = fopen(arg_string, "wb");
                if(!csv_log) { die_perror("could not open csv log `%s`", csv_log); }
        }

        bool quiet = gopt(options, 'q');
        unsigned png_bits = gopt(options, '1') ? 16 : 8;
        bool force = gopt(options, 'f');

        // initialize logger
        struct logger log;
        logger_start(&log, csv_log);

        // get output filenames, do basic file checking
        unsigned nin = argc - 1;
        unsigned nout = gopt(options, 'o');
        if(!(nout == 0 || nout == nin)) {
                die("must give output file names for all input files or none");
        }

        char **outfiles = malloc(sizeof(*outfiles) * nin);
        if(!outfiles) { die("could not allocate outfiles"); }
        if(nout) {
                gopt_args(options, 'o', (const char **)outfiles, nout);
        } else {
                for(unsigned i = 0; i < nin; i++) {
                        const char *infile = argv[1+i];
                        FILE *in = fopen(infile, "rb");
                        if(!in) { die("could not open input file `%s`", infile); }
                        fclose(in);

                        unsigned l = strlen(infile);
                        unsigned e = l;
                        const char *suffix = ".png";
                        if(l >= 5 && extension_equals(&infile[l-5], ".jpeg")) {
                                e = l-5;
                        } else if(l >= 4 && extension_equals(&infile[l-4], ".jpg")) {
                                e = l-4;
                        } else if(l >= 4 && extension_equals(&infile[l-4], ".png")) {
                                // PNG input: don't overwrite the input file
                                e = l-4;
                                suffix = ".restored.png";
                        }
                        char *outfile = malloc(e + strlen(suffix) + 1);
                        if(!outfile) { die("could not allocate outfile"); }
                        memcpy(outfile, infile, e);
                        memcpy(outfile+e, suffix, strlen(suffix) + 1);

                        if(!nout && !force) {
                                // don't overwrite when not given -o or -f, racy
                                FILE *out = fopen(outfile, "rb");
                                if(out) { die("not overwriting output file `%s`", outfile); }
                        }

                        FILE *out = fopen(outfile, "wb");
                        if(!out) { die("could not open output file `%s`", outfile); }
                        fclose(out);
                        remove(outfile);

                        outfiles[i] = outfile;
                }
        }

        // initialize progress bar
        struct progressbar pb;
        if(!quiet) {
                if(all_together) {
                        progressbar_start(&pb, nin * iterations[0]);
                } else {
                        progressbar_start(&pb, nin * (iterations[0] + iterations[1] + iterations[2]));
                }
                main_progressbar = &pb;
        }

        // decode each file smoothly
        OPENMP(parallel for schedule(dynamic) if(nin > 1) firstprivate(log))
        for(unsigned i = 0; i < nin; i++) {
                const char *infile = argv[1+i];
                const char *outfile = outfiles[i];
                log.filename = infile;

                decode_file(infile, outfile, iterations, weights, pweights, png_bits, all_together, quiet ? NULL : &pb, &log);
        }

        // clean up
        if(!nout) {
                for(unsigned i = 0; i < nin; i++) {
                        free(outfiles[i]);
                }
        }
        free(outfiles);

        if(!quiet) {
                progressbar_clear(&pb);
                main_progressbar = NULL;
        }

        gopt_free(options);

        if(csv_log) {
                fclose(csv_log);
        }
}
