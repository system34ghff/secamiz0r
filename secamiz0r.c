
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <memory.h>
#include <frei0r.h>
#include <gavl/gavl.h>

// -----------------------------------------------------------------------------
// BASICS

#define COLOR_CLAMP(x) ((x) < 0 ? 0 : ((x) > 255 ? 255 : (x)))
#define FRAND() (rand() / (double)RAND_MAX)

typedef struct secamiz0r_instance_s {
    unsigned int width;
    unsigned int height;
    unsigned int cwidth;
    unsigned int cheight;

    // Parameters set by a user.
    // All values must be in 0..1 range.
    double reception; /* how much random fires will be emitted */
    double shift; /* threshold of fires that will be emitted on horizontal edges */
    double noise; /* controls amount of chroma noise, screws quality of picture */

    // Input and output frames are in RGBA format.
    gavl_video_format_t format_rgba;
    gavl_video_frame_t *frame_in;
    gavl_video_frame_t *frame_out;

    // This is the Y'CbCr frame we are working on.
    gavl_video_frame_t *frame_ycbcr;
    gavl_video_format_t format_ycbcr;

    // We also need a downscaled copy of the Y'CbCr frame
    // for some kind of `edge detect'.
    // This is how I solve problems.
    gavl_video_scaler_t *map_scaler;
    gavl_video_format_t format_map;
    gavl_video_frame_t *frame_map;
    
    // I think it's obvious.
    gavl_video_converter_t *rgba_to_ycbcr;
    gavl_video_converter_t *ycbcr_to_rgba;
} secamiz0r_instance_t;

void burn(double reception, double shift, double noise, double time,
          uint8_t *out, int cwidth, int cheight, const uint8_t *map);

// -----------------------------------------------------------------------------
// 1D NOISE GENERATOR

#define MAX_NOISE_VERTICES 4096
#define MAX_NOISE_VERTICES_MASK (MAX_NOISE_VERTICES - 1)

double *noise_vertices;

static int noise_init() {
    noise_vertices = malloc(sizeof(double) * MAX_NOISE_VERTICES);
    if (!noise_vertices) {
        return 0;
    }

    // FIXME: I should use better RNG
    srand(time(NULL));
    for (int i = 0; i < MAX_NOISE_VERTICES; i++) {
        noise_vertices[i] = FRAND();
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
    info->minor_version = 8;
    info->num_params = 3;
    info->explanation = "Adds so called \"SECAM fire\" to the image.";
}

void f0r_get_param_info(f0r_param_info_t* info, int param_index) {
    switch (param_index) {
    case 0:
        info->name = "reception";
        info->type = F0R_PARAM_DOUBLE;
        info->explanation = NULL;
        break;
    case 1:
        info->name = "shift";
        info->type = F0R_PARAM_DOUBLE;
        info->explanation = NULL;
        break;
    case 2:
        info->name = "noise";
        info->type = F0R_PARAM_DOUBLE;
        info->explanation = NULL;
        break;
    }
}

f0r_instance_t f0r_construct(unsigned int width, unsigned int height) {
    secamiz0r_instance_t *inst = malloc(sizeof(secamiz0r_instance_t));
    inst->width = width;
    inst->height = height;

    inst->reception = 0.95;
    inst->shift = 0.30;
    inst->noise = 0.36;
    
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

    gavl_video_format_t *map = &inst->format_map;
    map->frame_width = map->image_width = inst->width / 2;
    map->frame_height = map->image_height = inst->height / 2;
    map->pixel_width = map->pixel_height = 1;
    map->pixelformat = GAVL_YUV_420_P;
    
    inst->frame_map = gavl_video_frame_create(map);
    
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

    inst->map_scaler = gavl_video_scaler_create();
    options = gavl_video_scaler_get_options(inst->map_scaler);
    gavl_video_options_set_quality(options, GAVL_QUALITY_FASTEST);
    gavl_video_scaler_init(inst->map_scaler, ycbcr, map);

    return (f0r_instance_t)inst;
}

void f0r_destruct(f0r_instance_t instance) {
    secamiz0r_instance_t* inst = (secamiz0r_instance_t*)instance;
    
    gavl_video_scaler_destroy(inst->map_scaler);
    gavl_video_converter_destroy(inst->ycbcr_to_rgba);
    gavl_video_converter_destroy(inst->rgba_to_ycbcr);
    
    gavl_video_frame_null(inst->frame_in);
    gavl_video_frame_null(inst->frame_out);
    
    gavl_video_frame_destroy(inst->frame_in);
    gavl_video_frame_destroy(inst->frame_out);
    gavl_video_frame_destroy(inst->frame_ycbcr);
    gavl_video_frame_destroy(inst->frame_map);
    
    free(instance);
}

void f0r_set_param_value(f0r_instance_t instance,
                         f0r_param_t param, int param_index)
{
    secamiz0r_instance_t* inst = (secamiz0r_instance_t*)instance;
    switch (param_index) {
    case 0:
        inst->reception = *((double *)param);
        break;
    case 1:
        inst->shift = *((double *)param);
        break;
    case 2:
        inst->noise = *((double *)param);
        break;
    }
}

void f0r_get_param_value(f0r_instance_t instance,
                         f0r_param_t param, int param_index)
{ 
    secamiz0r_instance_t *inst = (secamiz0r_instance_t *)instance;
    switch (param_index) {
    case 0:
        *((double *)param) = inst->reception;
        break;
    case 1:
        *((double *)param) = inst->shift;
        break;
    case 2:
        *((double *)param) = inst->noise;
        break;
    }
}

void f0r_update(f0r_instance_t instance, double time,
                const uint32_t *in_frame, uint32_t *out_frame)
{
    secamiz0r_instance_t *inst = (secamiz0r_instance_t *)instance;

    inst->frame_in->planes[0] = (uint8_t *)in_frame;
    gavl_video_convert(inst->rgba_to_ycbcr, inst->frame_in, inst->frame_ycbcr);
    
    double reception = inst->reception * 0.04 + 0.96;
    double shift = 1.0 - inst->shift * 0.8;
    double noise = inst->noise;
    
    // create map
    gavl_video_scaler_scale(inst->map_scaler,
                            inst->frame_ycbcr, inst->frame_map);

    burn(reception, shift, noise, time, inst->frame_ycbcr->planes[1],
         inst->cwidth, inst->cheight, inst->frame_map->planes[0]);
    burn(reception, shift, noise, time, inst->frame_ycbcr->planes[2],
         inst->cwidth, inst->cheight, inst->frame_map->planes[0]);
    
    inst->frame_out->planes[0] = (uint8_t *)out_frame;
    gavl_video_convert(inst->ycbcr_to_rgba, inst->frame_ycbcr, inst->frame_out);
}

// -----------------------------------------------------------------------------
// SECAM FIRE

void burn(double reception, double shift, double noise, double time,
          uint8_t *out, int cwidth, int cheight, const uint8_t *map)
{
    int fire = -1;
    int step = 0;

    for (int cy = 0; cy < cheight - 1; cy++) {
        // Work only on even lines
        if (cy % 2 == 1) {
            continue;
        }

        // Current and lower scanlines
        uint8_t *dst_cur = &out[cy * cwidth];
        uint8_t *dst_low = dst_cur + cwidth;

        // Map's scanline
        const uint8_t *src_map = &map[cy * cwidth];

        // Initially this value was starting at 0, but this could make
        // beginning of a scanline look empty, so I decided to go with -8.
        // `fx' stands for `fire index', this is the X value where the last
        // fire was flashed.
        int fx = -8;

        for (int cx = 0; cx < cwidth; cx++) {
            double delta = 0.0;

            if (noise) {
                double n = get_noise(time + cy * cwidth + cx, 36.0, 0.48) - 18.0;
                dst_cur[cx] = COLOR_CLAMP(dst_cur[cx] + n);
                dst_low[cx] = COLOR_CLAMP(dst_low[cx] + n);
            }

            if (cx > 0) {
                int alpha = src_map[cx - 1] * 0.25 + dst_cur[cx] * 0.75;
                int beta = src_map[cx] * 0.25 + dst_cur[cx + 1] * 0.75;
                delta = abs(beta - alpha) / 256.0;
            }

            if (FRAND() > reception || delta > shift) {
                fire = FRAND() * (256 - dst_cur[cx]);
                step = (256 - dst_cur[cx]) / 32;
    
                if (delta > shift) {
                    // no tail if the fire was caused by `channel shift'
                    fx = -8;
                } else {
                    fx = cx - 1;
                }
            }

            // tail
            if (cx - fx < 6) {
                dst_cur[cx] = COLOR_CLAMP(dst_cur[cx] + fire * (0.15 * (cx - fx)));
                dst_low[cx] = COLOR_CLAMP(dst_low[cx] + fire * (0.15 * (cx - fx)));
                continue;
            }

            if (fire <= 0) {
                continue;
            }

            dst_cur[cx] = COLOR_CLAMP(dst_cur[cx] + fire);
            dst_low[cx] = COLOR_CLAMP(dst_low[cx] + fire);
            fire -= step;
        }
    }
}
