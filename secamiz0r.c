
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <memory.h>
#include <stdbool.h>
#include <frei0r.h>
#include <gavl/gavl.h>
#include "pcg-c-basic/pcg_basic.h"

// -----------------------------------------------------------------------------
// BASICS

#define COLOR_CLAMP(x) ((x) < 0 ? 0 : ((x) > 255 ? 255 : (x)))

#define SECAM_FIRE_INTENSITY 192

typedef struct secamiz0r_instance_s {
    unsigned int width;
    unsigned int height;
    unsigned int cwidth;
    unsigned int cheight;

    // Parameters set by a user.
    // All values must be in 0..1 range.
    double rnd_factor; /* how much random fires will be emitted */
    double luma_factor; /* threshold of fires that will be emitted on horizontal edges */
    double chroma_factor;
    double simple_noise; /* controls amount of chroma noise, screws quality of picture */

    // Input and output frames are in RGBA format.
    gavl_video_format_t format_rgba;
    gavl_video_frame_t *frame_in;
    gavl_video_frame_t *frame_out;

    // This is the Y'CbCr frame we are working on.
    gavl_video_frame_t *frame_ycbcr;
    gavl_video_format_t format_ycbcr;
    
    // I think it's obvious.
    gavl_video_converter_t *rgba_to_ycbcr;
    gavl_video_converter_t *ycbcr_to_rgba;

    pcg32_random_t rng;

    double probability;
    double threshold;
    double noise_amplitude;
} secamiz0r_instance_t;

void secam_fire(secamiz0r_instance_t *inst, double time);

// -----------------------------------------------------------------------------
// 1D NOISE GENERATOR

#define MAX_NOISE_VERTICES 4096
#define MAX_NOISE_VERTICES_MASK (MAX_NOISE_VERTICES - 1)

double *noise_vertices;

static int noise_init() {
    pcg32_random_t rng;

    noise_vertices = malloc(sizeof(double) * MAX_NOISE_VERTICES);
    if (!noise_vertices) {
        return 0;
    }

    pcg32_srandom_r(&rng, 213u, 1996u);
    for (int i = 0; i < MAX_NOISE_VERTICES; i++) {
        noise_vertices[i] = ldexp(pcg32_random_r(&rng), -32);
    }

    return 1;
}

static double get_noise(double x, double amp, double scale) {
    double xs = x * scale;
    int xf = floor(xs);
    double t = xs - xf;
    double ts = t * t * (3 - 2 * t);
    int xmin = xf & MAX_NOISE_VERTICES_MASK;
    int xmax = (xmin + 1) & MAX_NOISE_VERTICES_MASK;
    double y = noise_vertices[xmin] * (1 - ts) + noise_vertices[xmax] * ts;
    
    return y * amp;
}

// -----------------------------------------------------------------------------
// FREI0R ENTRIES

int f0r_init(void) {
    if (!noise_init()) {
        return 0;
    }

    return 1;
}

void f0r_deinit(void) {
    free(noise_vertices);
}

void f0r_get_plugin_info(f0r_plugin_info_t *info) {
    info->name = "Secamiz0R";
    info->author = "Valery Khabarov";
    info->plugin_type = F0R_PLUGIN_TYPE_FILTER;
    info->color_model = F0R_COLOR_MODEL_RGBA8888;
    info->frei0r_version = FREI0R_MAJOR_VERSION;
    info->major_version = 0;
    info->minor_version = 9;
    info->num_params = 4;
    info->explanation = "Adds so called \"SECAM fire\" to the image.";
}

void f0r_get_param_info(f0r_param_info_t* info, int param_index) {
    switch (param_index) {
    case 0:
        info->name = "Randomization factor";
        info->type = F0R_PARAM_DOUBLE;
        info->explanation = "Amount of flames that appear randomly across the frame.";
        break;
    case 1:
        info->name = "Sharp edge factor";
        info->type = F0R_PARAM_DOUBLE;
        info->explanation = "Amount of flames that appear on sharp edges.";
        break;
    case 2:
        info->name = "Oversaturation factor";
        info->type = F0R_PARAM_DOUBLE;
        info->explanation = "Amount of flames that appear on oversaturated areas.";
        break;
    case 3:
        info->name = "Background color noise";
        info->type = F0R_PARAM_DOUBLE;
        info->explanation = "Background color noise. Requires a lot of bitrate.";
        break;
    }
}

f0r_instance_t f0r_construct(unsigned int width, unsigned int height) {
    secamiz0r_instance_t *inst = malloc(sizeof(secamiz0r_instance_t));
    inst->width = width;
    inst->height = height;

    inst->rnd_factor = 0.01;
    inst->luma_factor = 0.30;
    inst->chroma_factor = 0.48;
    inst->simple_noise = 0.13;
    
    gavl_video_format_t *rgba = &inst->format_rgba;
    rgba->frame_width = rgba->image_width = inst->width;
    rgba->frame_height = rgba->image_height = inst->height;
    rgba->pixel_width = rgba->pixel_height = 1;
    rgba->pixelformat = GAVL_RGBA_32;
    rgba->interlace_mode = GAVL_INTERLACE_NONE;

    inst->frame_in = gavl_video_frame_create(NULL);
    inst->frame_out = gavl_video_frame_create(NULL);
    inst->frame_in->strides[0] = inst->width * 4;
    inst->frame_out->strides[0] = inst->width * 4;
    
    gavl_video_format_t *ycbcr = &inst->format_ycbcr;
    ycbcr->frame_width = ycbcr->image_width = inst->width;
    ycbcr->frame_height = ycbcr->image_height = inst->height;
    ycbcr->pixel_width = ycbcr->pixel_height = 1;
    ycbcr->pixelformat = GAVL_YUV_420_P;
    ycbcr->interlace_mode = GAVL_INTERLACE_NONE;

    inst->frame_ycbcr = gavl_video_frame_create(ycbcr);
    inst->cwidth = inst->frame_ycbcr->strides[1];
    inst->cheight = inst->height / 2;
    
    gavl_video_options_t *options;

    // converters from RGBA to YCbCr and vice versa
    inst->rgba_to_ycbcr = gavl_video_converter_create();
    inst->ycbcr_to_rgba = gavl_video_converter_create();

    options = gavl_video_converter_get_options(inst->rgba_to_ycbcr);
    gavl_video_options_set_defaults(options);

    options = gavl_video_converter_get_options(inst->ycbcr_to_rgba);
    gavl_video_options_set_quality(options, GAVL_QUALITY_DEFAULT);
    gavl_video_options_set_conversion_flags(options, GAVL_RESAMPLE_CHROMA);
    gavl_video_options_set_scale_mode(options, GAVL_SCALE_BILINEAR);

    gavl_video_converter_init(inst->rgba_to_ycbcr, rgba, ycbcr);
    gavl_video_converter_init(inst->ycbcr_to_rgba, ycbcr, rgba);

    pcg32_srandom_r(&inst->rng, 0xdead, 0xcafe);

    return (f0r_instance_t)inst;
}

void f0r_destruct(f0r_instance_t instance) {
    secamiz0r_instance_t* inst = (secamiz0r_instance_t*)instance;

    gavl_video_converter_destroy(inst->ycbcr_to_rgba);
    gavl_video_converter_destroy(inst->rgba_to_ycbcr);
    
    gavl_video_frame_null(inst->frame_in);
    gavl_video_frame_null(inst->frame_out);
    
    gavl_video_frame_destroy(inst->frame_in);
    gavl_video_frame_destroy(inst->frame_out);
    gavl_video_frame_destroy(inst->frame_ycbcr);
    
    free(instance);
}

void f0r_set_param_value(f0r_instance_t instance,
                         f0r_param_t param, int param_index)
{
    secamiz0r_instance_t* inst = (secamiz0r_instance_t*)instance;
    switch (param_index) {
    case 0:
        inst->rnd_factor = *((double *)param);
        break;
    case 1:
        inst->luma_factor = *((double *)param);
        break;
    case 2:
        inst->chroma_factor = *((double *)param);
        break;
    case 3:
        inst->simple_noise = *((double *)param);
        break;
    }
}

void f0r_get_param_value(f0r_instance_t instance,
                         f0r_param_t param, int param_index)
{ 
    secamiz0r_instance_t *inst = (secamiz0r_instance_t *)instance;
    switch (param_index) {
    case 0:
        *((double *)param) = inst->rnd_factor;
        break;
    case 1:
        *((double *)param) = inst->luma_factor;
        break;
    case 2:
        *((double *)param) = inst->chroma_factor;
        break;
    case 3:
        *((double *)param) = inst->simple_noise;
        break;
    }
}

void f0r_update(f0r_instance_t instance, double time,
                const uint32_t *in_frame, uint32_t *out_frame)
{
    secamiz0r_instance_t *inst = (secamiz0r_instance_t *)instance;

    inst->frame_in->planes[0] = (uint8_t *)in_frame;
    gavl_video_convert(inst->rgba_to_ycbcr, inst->frame_in, inst->frame_ycbcr);
    secam_fire(inst, time);
    
    inst->frame_out->planes[0] = (uint8_t *)out_frame;
    gavl_video_convert(inst->ycbcr_to_rgba, inst->frame_ycbcr, inst->frame_out);
}

// -----------------------------------------------------------------------------
// SECAM FIRE

#define POW2(x) ((x) * (x))
#define FRAND() (pcg32_boundedrand_r(&inst->rng, 0xdeadcafe) / (double)0xdeadcafe)

#define RND_CONSTRAINT 0.013
#define LUMA_DELTA_CONSTRAINT 0.667
#define CHROMA_DELTA_CONSTRAINT 0.911

void secam_fire(secamiz0r_instance_t *inst, double time) {
    uint8_t *luma = inst->frame_ycbcr->planes[0];
    uint8_t *cb = inst->frame_ycbcr->planes[1];
    uint8_t *cr = inst->frame_ycbcr->planes[2];

    double rnd_threshold =
        1.0 - RND_CONSTRAINT * POW2(inst->rnd_factor);
    double luma_threshold =
        1.0 - LUMA_DELTA_CONSTRAINT * inst->luma_factor;
    double chroma_threshold =
        1.0 - CHROMA_DELTA_CONSTRAINT * inst->chroma_factor;

    for (int cy = 0; cy < inst->cheight - 1; cy++) {
        // Work only on even lines.
        // Because of this we have to alter two pixels at the same time.
        if (cy % 2 == 1) {
            continue;
        }

        int fire = 0;
        int step = 0;

        uint8_t *dst[2];
        dst[0] = &cb[cy * inst->cwidth];
        dst[1] = &cr[cy * inst->cwidth];

        // We're altering two planes, a blue and a red.
        for (int p = 0; p < 2; p++) {
            int fx = 0;

            unsigned int frame_rand = pcg32_random_r(&inst->rng);

            for (int cx = 0; cx < inst->cwidth; cx++) {
                int ux = cx + inst->cwidth;

                double magic_random_number = FRAND();

                //
                // Step 1: add some static chroma noise
                if (inst->simple_noise) {
                    double amp;
                    double n;
                    int e;

                    // noise increases chroma signal up to 25%
                    amp = inst->simple_noise * 25.0;
                    e = frame_rand + cy * inst->cwidth + cx;
                    n = get_noise(e, amp, 0.18);

                    dst[p][cx] = COLOR_CLAMP(dst[p][cx] + n);
                    dst[p][ux] = COLOR_CLAMP(dst[p][ux] + n);
                }

                //
                // Step 2: Calculate delta value, which is used to create
                // a fire if there are sharp edges.
                double luma_delta = 0.0;
                if (inst->luma_factor > 0.0 && cx < inst->cwidth - 1) {
                    int x, y, w;
                    int a, a_00, a_01, a_10, a_11;
                    int b, b_00, b_01, b_10, b_11;

                    x = cx * 2;
                    y = cy * 2;
                    w = inst->cwidth * 2;

                    a_00 = luma[y * w + x];
                    a_01 = luma[y * w + x + 1];
                    a_10 = luma[y * w + x + w];
                    a_11 = luma[y * w + x + w + 1];
                    a = a_00 + a_01 + a_10 + a_11;

                    x = x + 2;

                    b_00 = luma[y * w + x];
                    b_01 = luma[y * w + x + 1];
                    b_10 = luma[y * w + x + w];
                    b_11 = luma[y * w + x + w + 1];
                    b = b_00 + b_01 + b_10 + b_11;

                    luma_delta = (abs(b - a) / 256.0) * 0.4 + 0.3;
                }

                //
                // Step 3a: Keep painting a pending fire if it's there
                if (fire >= 16) {
                    int dx = cx - fx;

                    // If we are not too far from the starting point,
                    // we have to draw a "slight tail" in order to
                    // keep a fire more slight.
                    if (dx < 6) {
                        dst[p][cx] = COLOR_CLAMP(
                            dst[p][cx] + 0.15 * dx * fire
                        );
                        dst[p][ux] = COLOR_CLAMP(
                            dst[p][ux] + 0.15 * dx * fire
                        );
                        continue;
                    }

                    dst[p][cx] = COLOR_CLAMP(dst[p][cx] + fire);
                    dst[p][ux] = COLOR_CLAMP(dst[p][ux] + fire);

                    fire = fire - step;
                    continue;
                }

                //
                // Step 3b: Create a fire if there is need to do it
                double rnd = ldexp(pcg32_random_r(&inst->rng), -32);
                double chroma_delta = abs(dst[0][cx] - dst[1][cx]) / 256.0;
                if (rnd > rnd_threshold
                ||  chroma_delta > chroma_threshold + magic_random_number
                ||  luma_delta > luma_threshold + magic_random_number
                ) {
                    int c = SECAM_FIRE_INTENSITY - dst[p][cx];
                    fire = rnd * c;
                    step = c / (SECAM_FIRE_INTENSITY / 8);
                    if (step < 4) {
                        step = 4;
                    }
                    fx = cx;
                }
            }
        }
    }
}

