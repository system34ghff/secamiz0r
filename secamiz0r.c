
#include <stdlib.h>
#include <memory.h>
#include <frei0r.h>
#include <gavl/gavl.h>

typedef struct secamiz0r_instance_s {
    unsigned int width;
    unsigned int height;

    unsigned int cwidth;
    unsigned int cheight;
    
    double reception;
    double shift;
    double noise;
    
    gavl_video_frame_t *frame_in;
    gavl_video_frame_t *frame_out;
    gavl_video_frame_t *frame_ycbcr;
    gavl_video_frame_t *frame_map;
    
    gavl_video_format_t format_rgba;
    gavl_video_format_t format_ycbcr;
    gavl_video_format_t format_map;
    
    gavl_video_converter_t *rgba_to_ycbcr;
    gavl_video_converter_t *ycbcr_to_rgba;

    gavl_video_scaler_t *map_scaler;
} secamiz0r_instance_t;

#define COLOR_CLAMP(x) ((x) < 0 ? 0 : ((x) > 255 ? 255 : (x)))
#define FRAND() (rand() / (double)RAND_MAX)

int f0r_init(void) { return 1; }
void f0r_deinit(void) {}

void f0r_get_plugin_info(f0r_plugin_info_t *info) {
    info->name = "secamiz0r";
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
    
    inst->frame_in = gavl_video_frame_create(NULL);
    inst->frame_out = gavl_video_frame_create(NULL);
    inst->frame_in->strides[0] = inst->width * 4;
    inst->frame_out->strides[0] = inst->width * 4;
    
    gavl_video_format_t *rgba = &inst->format_rgba;
    rgba->frame_width = rgba->image_width = inst->width;
    rgba->frame_height = rgba->image_height = inst->height;
    rgba->pixel_width = rgba->pixel_height = 1;
    rgba->pixelformat = GAVL_RGBA_32;
    rgba->interlace_mode = GAVL_INTERLACE_NONE;
    
    gavl_video_format_t *ycbcr = &inst->format_ycbcr;
    ycbcr->frame_width = ycbcr->image_width = inst->width;
    ycbcr->frame_height = ycbcr->image_height = inst->height;
    ycbcr->pixel_width = ycbcr->pixel_height = 1;
    ycbcr->pixelformat = GAVL_YUV_410_P;
    ycbcr->interlace_mode = GAVL_INTERLACE_NONE;

    gavl_video_format_t *map = &inst->format_map;
    map->frame_width = map->image_width = inst->width / 4;
    map->frame_height = map->image_height = inst->height / 4;
    map->pixel_width = map->pixel_height = 1;
    map->pixelformat = GAVL_YUV_410_P;

    inst->frame_ycbcr = gavl_video_frame_create(ycbcr);
    inst->frame_map = gavl_video_frame_create(map);
    
    inst->cwidth = inst->frame_ycbcr->strides[1];
    inst->cheight = inst->height / 4;

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

void burn(double reception, double shift, double noise,
          uint8_t *out, int cwidth, int cheight, const uint8_t *map)
{
    const int len = cwidth * cheight;
    int fire = -1;
    int step = 0;

    for (int idx = 4; idx < len; idx++) {
        double delta;

        if (idx % cwidth == 0) {
            delta = 0.0;
        } else {
            delta = abs(map[idx] - map[idx - 1]) / 256.0;
        }

        if (FRAND() > reception || shift < delta) {
            fire = FRAND() * (256 - out[idx]);
            step = (256 - out[idx]) / 16;

            for (int j = 1; j <= 3; j++) {
                // tail
                int diff = 0.25 * j * fire;
                out[idx - 3 + j] = COLOR_CLAMP(out[idx - 3 + j] + diff);
            }
        }

        if (fire <= 0) {
            continue;
        }

        out[idx] = COLOR_CLAMP(out[idx] + fire);
        fire -= step;
    }
}

void make_map(secamiz0r_instance_t *inst) {
    gavl_video_scaler_scale(inst->map_scaler,
                            inst->frame_ycbcr, inst->frame_map);
}

void f0r_update(f0r_instance_t instance, double time,
                const uint32_t *in_frame, uint32_t *out_frame)
{
    secamiz0r_instance_t *inst = (secamiz0r_instance_t *)instance;

    inst->frame_in->planes[0] = (uint8_t *)in_frame;
    gavl_video_convert(inst->rgba_to_ycbcr, inst->frame_in, inst->frame_ycbcr);
    
    double reception = inst->reception * 0.04 + 0.96;
    double shift = 1.0 - (inst->shift * 0.5 + 0.5);
    double noise = inst->noise;
    
    // create map
    gavl_video_scaler_scale(inst->map_scaler,
                            inst->frame_ycbcr, inst->frame_map);

    burn(reception, shift, noise, inst->frame_ycbcr->planes[1],
         inst->cwidth, inst->cheight, inst->frame_map->planes[0]);
    burn(reception, shift, noise, inst->frame_ycbcr->planes[2],
         inst->cwidth, inst->cheight, inst->frame_map->planes[0]);
    
    inst->frame_out->planes[0] = (uint8_t *)out_frame;
    gavl_video_convert(inst->ycbcr_to_rgba, inst->frame_ycbcr, inst->frame_out);
}
