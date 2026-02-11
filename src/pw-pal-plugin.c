/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <limits.h>
#include <math.h>
#include <dirent.h>
#include <linux/input.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/utils/ringbuffer.h>
#include <spa/debug/types.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/buffers.h>
#include <pipewire/impl.h>
#include <pipewire/i18n.h>
#include <PalApi.h>
#include <PalDefs.h>
#include <agm/agm_api.h>


#define LOG_TAG "pw-pal-plugin"

#define BITS_PER_BYTE 8
#define NUM_BYTES ((SW_MAX + BITS_PER_BYTE) / BITS_PER_BYTE)
#define BIT_VALUE(bit, array) \
    ((array[(bit) / BITS_PER_BYTE] >> ((bit) % BITS_PER_BYTE)) & 1)

PW_LOG_TOPIC_STATIC(log_topic, "log:" LOG_TAG);
#define PW_LOG_TOPIC_DEFAULT log_topic

#define PW_DEFAULT_SAMPLE_FORMAT "S16"
#define PW_DEFAULT_SAMPLE_RATE 48000
#define PW_DEFAULT_SAMPLE_CHANNELS 2
#define PW_DEFAULT_SAMPLE_POSITION "[ FL FR ]"
#define PW_DEFAULT_BUFFER_DURATION_MS 25
#define PW_LOW_LATENCY_BUFFER_DURATION_MS 5
#define PW_DEEP_BUFFER_BUFFER_DURATION_MS 20
#define MAX_NAME_LENGTH 20
#define DEV_INPUT_DIR "/dev/input"
#define FILE_PREFIX "event"

struct pw_userdata {
    struct pw_context *context;

    struct pw_properties *props;

    struct pw_impl_module *module;

    struct spa_hook module_listener;

    struct pw_core *core;
    struct spa_hook core_proxy_listener;
    struct spa_hook core_listener;

    struct pw_properties *stream_props;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    struct spa_audio_info_raw info;
    uint32_t frame_size;
    struct spa_audio_info format;

    unsigned int do_disconnect:1;

    pal_stream_handle_t *stream_handle;
    struct pal_device *pal_device;
    struct pal_stream_attributes *stream_attributes;
    bool isplayback;
    pal_stream_type_t stream_type;
    pal_device_id_t pal_device_id;
    bool is_offload;
    size_t source_buf_size;
    size_t source_buf_count;
    size_t sink_buf_size;
    size_t sink_buf_count;

    struct spa_source *jack_src;
    int jack_fd;
    char jack_name[MAX_NAME_LENGTH];
};

static void pw_pal_destroy_stream(void *d)
{
    struct pw_userdata *udata = d;

    spa_hook_remove(&udata->stream_listener);
    udata->stream = NULL;
}

static int32_t pa_pal_out_cb(pal_stream_handle_t *stream_handle,
                            uint32_t event_id, uint32_t *event_data,
                            uint32_t event_size, uint64_t cookie) {

    return 0;
}
static void pw_pal_set_volume (struct pw_userdata *udata, float gain)
{
    int rc = 0, i;
    uint32_t channel_mask = 1;
    uint32_t no_vol_pair = udata->stream_attributes->out_media_config.ch_info.channels;
    struct pal_volume_data *volume = (struct pal_volume_data *)malloc(sizeof(uint32_t) +
        (sizeof(struct pal_channel_vol_kv) * (no_vol_pair)));
    for (i = 0; i < no_vol_pair; i++)
        channel_mask = (channel_mask | udata->stream_attributes->out_media_config.ch_info.ch_map[i]);
    channel_mask = (channel_mask << 1);
    if (volume) {
        volume->no_of_volpair = no_vol_pair;
        for (i = 0; i < no_vol_pair; i++) {
            volume->volume_pair[i].channel_mask = channel_mask;
            volume->volume_pair[i].vol = gain;
        }
        rc = pal_stream_set_volume(udata->stream_handle, volume);
        free(volume);
    }
}
static int close_pal_stream(struct pw_userdata *udata)
{
    int rc = -1;

    if (udata->stream_handle) {
        rc = pal_stream_stop(udata->stream_handle);
        if (rc) {
            pw_log_error("pal_stream_stop failed for %p error %d", udata->stream_handle, rc);
        }
        rc = pal_stream_close(udata->stream_handle);
        if (rc)
            pw_log_error("could not close sink handle %p, error %d", udata->stream_handle, rc);
        udata->stream_handle = NULL;
    }
    else
        return 0;

    return rc;
}
static void pw_pal_stream_start(struct pw_userdata *udata)
{
    int rc = 0;
    pal_buffer_config_t out_buf_cfg, in_buf_cfg;
    rc = pal_stream_open(udata->stream_attributes, 1, udata->pal_device, 0, NULL, pa_pal_out_cb, (uint64_t)udata,
            &udata->stream_handle);

    if (rc) {
        udata->stream_handle = NULL;
        pw_log_error("Could not open output stream %d", rc);
        goto exit;
    }

    if (udata->isplayback) {
        in_buf_cfg.buf_size = 0;
        in_buf_cfg.buf_count = 0;
        out_buf_cfg.buf_size = udata->sink_buf_size;
        out_buf_cfg.buf_count = udata->sink_buf_count;
    } else {
        out_buf_cfg.buf_size = 0;
        out_buf_cfg.buf_count = 0;
        in_buf_cfg.buf_size = udata->source_buf_size;
        in_buf_cfg.buf_count = udata->source_buf_count;
    }

    rc = pal_stream_set_buffer_size(udata->stream_handle, &in_buf_cfg, &out_buf_cfg);
    if(rc) {
        pw_log_error("pal_stream_set_buffer_size failed\n");
        goto exit;
    }
    rc = pal_stream_start(udata->stream_handle);
    if (rc) {
        pw_log_error("pal_stream_start failed, error %d\n", rc);
        goto cleanup;
        }
    if (udata->isplayback) {
        pw_log_error("pal_stream_start set volume, error %d\n", rc);
        pw_pal_set_volume(udata, 1.0);
    }

    return;
cleanup:
    if (close_pal_stream(udata))
        pw_log_error("could not close sink handle %p", udata->stream_handle);
exit:
    return;

}
static void pw_pal_change_stream_state(void *d, enum pw_stream_state old,
        enum pw_stream_state state, const char *error)
{
    struct pw_userdata *udata = d;

    switch (state) {
    case PW_STREAM_STATE_ERROR:
    case PW_STREAM_STATE_UNCONNECTED:
        pw_impl_module_schedule_destroy(udata->module);
        break;
    case PW_STREAM_STATE_PAUSED:
        close_pal_stream(udata);
        break;
    case PW_STREAM_STATE_STREAMING:
        pw_pal_stream_start(udata);
        break;
    default:
        break;
    }
}

static void pw_pal_process_stream(void *d)
{
    struct pw_userdata *udata = d;
    struct pw_buffer *buf;
    struct spa_data *bd;
    void *data;
    uint32_t offs, size;
    struct pal_buffer pal_buf;
    int rc = 0;
    static int tmp = 0;

    if ((buf = pw_stream_dequeue_buffer(udata->stream)) == NULL) {
        pw_log_error("out of buffers: %m");
        return;
    }

    bd = &buf->buffer->datas[0];
    memset(&pal_buf, 0, sizeof(struct pal_buffer));
    if (udata->isplayback) {
        offs = SPA_MIN(bd->chunk->offset, bd->maxsize);
        size = SPA_MIN(bd->chunk->size, bd->maxsize - offs);
        data = SPA_PTROFF(bd->data, offs, void);

        pal_buf.buffer = data;
        pal_buf.size = size;

        if (udata->stream_handle) {
            if ((rc = pal_stream_write(udata->stream_handle, &pal_buf)) < 0) {
                pw_log_error("Could not write data: %d %d", rc, __LINE__);
            }
        }
    } else {
        data = bd->data;
        size = buf->requested ? buf->requested * udata->frame_size : bd->maxsize;

        pal_buf.buffer = data;
        pal_buf.size = size;
        /* fill buffer contents here */
        if (udata->stream_handle) {
            if ((rc = pal_stream_read(udata->stream_handle, &pal_buf)) < 0) {
                pw_log_error("Could not read data: %d %d", rc, __LINE__);
            }
        }
        pw_log_error("read buffer data %p with up to %u bytes", data, size);

        bd->chunk->size = size;
        bd->chunk->stride = udata->frame_size;
        bd->chunk->offset = 0;
        buf->size = size / udata->frame_size;
    }
    /* write buffer contents here */

    pw_stream_queue_buffer(udata->stream, buf);
}

static void pw_pal_change_stream_param(void *data, uint32_t id, const struct spa_pod *param) {
    struct pw_userdata *udata = data;

    if (param == NULL || id != SPA_PARAM_Format)
        return;
    if (spa_format_parse(param, &udata->format.media_type, &udata->format.media_subtype) < 0)
        return;
    if (udata->format.media_type == SPA_MEDIA_TYPE_audio &&
        udata->format.media_subtype == SPA_MEDIA_SUBTYPE_raw) {
        spa_format_audio_raw_parse(param, &udata->format.info.raw);
    } else {
        pw_log_info("Compressed format detected: subtype=%u", udata->format.media_subtype);
    }
}
static const struct pw_stream_events pw_pal_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .destroy = pw_pal_destroy_stream,
    .state_changed = pw_pal_change_stream_state,
    .process = pw_pal_process_stream,
    .param_changed = pw_pal_change_stream_param
};

static int pw_pal_create_stream(struct pw_userdata *udata)
{
    int res;
    uint32_t n_params = 0;
    const struct spa_pod *params[2];
    uint8_t buffer[1024];
    struct spa_pod_builder b;

    spa_pod_builder_init(&b, buffer, sizeof(buffer));
    if (udata->isplayback) {
        udata->stream = pw_stream_new(udata->core, "example sink", udata->stream_props);
        if (udata->is_offload) {
            params[n_params++] = spa_pod_builder_add_object(&b,
                            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                            SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(udata->sink_buf_count),
                            SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(0),
                            SPA_PARAM_BUFFERS_size,    SPA_POD_Int(udata->sink_buf_size),
                            SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(udata->frame_size));
        } else {
            params[n_params++] = spa_pod_builder_add_object(&b,
                            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                            SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(udata->sink_buf_count),
                            SPA_PARAM_BUFFERS_blocks,  0,
                            SPA_PARAM_BUFFERS_size,    SPA_POD_Int(udata->sink_buf_size),
                            SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(udata->frame_size));
        }
    } else {
        udata->stream = pw_stream_new(udata->core, "example source", udata->stream_props);
        params[n_params++] = spa_pod_builder_add_object(&b,
                        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                        SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(udata->source_buf_count),
                        SPA_PARAM_BUFFERS_blocks,  0,
                        SPA_PARAM_BUFFERS_size,    SPA_POD_Int(udata->source_buf_size),
                        SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(udata->frame_size));
    }

    if (udata->stream == NULL)
        return -errno;

    pw_stream_add_listener(udata->stream,
            &udata->stream_listener,
            &pw_pal_stream_events, udata);

    if (udata->is_offload) {
        params[n_params++] =  spa_pod_builder_add_object(&b,
                        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_mp3),
                        SPA_FORMAT_AUDIO_format, SPA_POD_Id(SPA_AUDIO_FORMAT_ENCODED),
                        SPA_FORMAT_AUDIO_rate, SPA_POD_Int(44100),
                        SPA_FORMAT_AUDIO_channels, SPA_POD_Int(2));
    } else {
        params[n_params++] = spa_format_audio_raw_build(&b,
                        SPA_PARAM_EnumFormat, &udata->info);
    }

    res = pw_stream_connect(udata->stream,
              udata->isplayback ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
              PW_ID_ANY,
              PW_STREAM_FLAG_AUTOCONNECT |
              PW_STREAM_FLAG_NO_CONVERT |
              PW_STREAM_FLAG_MAP_BUFFERS |
              PW_STREAM_FLAG_RT_PROCESS,
              params, n_params);

    if (udata->is_offload)
        pw_stream_set_active(udata->stream, true);

   return 0;
}

static void pw_pal_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    struct pw_userdata *udata = data;

    pw_log_error("error id:%u seq:%d res:%d (%s): %s",
            id, seq, res, spa_strerror(res), message);

    if (id == PW_ID_CORE && res == -EPIPE)
        pw_impl_module_schedule_destroy(udata->module);
}

static const struct pw_core_events pw_pal_events_core = {
    PW_VERSION_CORE_EVENTS,
    .error = pw_pal_core_error,
};

static void pw_pal_core_destroy(void *d)
{
    struct pw_userdata *udata = d;
    spa_hook_remove(&udata->core_listener);
    udata->core = NULL;
    pw_impl_module_schedule_destroy(udata->module);
}

static const struct pw_proxy_events pw_pal_proxy_events_core = {
    .destroy = pw_pal_core_destroy,
};

static void pw_pal_userdata_destroy(struct pw_userdata *udata)
{
    if (udata->stream)
        pw_stream_destroy(udata->stream);
    if(fcntl(udata->jack_fd, F_GETFD) != 1 || errno != EBADF)
        close(udata->jack_fd);
    if (udata->core && udata->do_disconnect)
        pw_core_disconnect(udata->core);

    pw_properties_free(udata->stream_props);
    pw_properties_free(udata->props);

    free(udata);
}

static void pw_pal_module_destroy(void *data)
{
    struct pw_userdata *udata = data;
    spa_hook_remove(&udata->module_listener);
    pw_pal_userdata_destroy(udata);
}

static const struct pw_impl_module_events pw_pal_events_module = {
    PW_VERSION_IMPL_MODULE_EVENTS,
    .destroy = pw_pal_module_destroy,
};

static inline uint32_t format_from_name(const char *name, size_t len)
{
    int i;
    for (i = 0; spa_type_audio_format[i].name; i++) {
        if (strncmp(name, spa_debug_type_short_name(spa_type_audio_format[i].name), len) == 0)
            return spa_type_audio_format[i].type;
    }
    return SPA_AUDIO_FORMAT_UNKNOWN;
}

static uint32_t pw_pal_get_channel(const char *name)
{
    int i;
    for (i = 0; spa_type_audio_channel[i].name; i++) {
        if (spa_streq(name, spa_debug_type_short_name(spa_type_audio_channel[i].name)))
            return spa_type_audio_channel[i].type;
    }
    return SPA_AUDIO_CHANNEL_UNKNOWN;
}

static void pw_pal_get_parse_position(struct spa_audio_info_raw *info, const char *val, size_t len)
{
    struct spa_json it[2];
    char v[256];

    spa_json_init(&it[0], val, len);
        if (spa_json_enter_array(&it[0], &it[1]) <= 0)
                spa_json_init(&it[1], val, len);

    info->channels = 0;
    while (spa_json_get_string(&it[1], v, sizeof(v)) > 0 &&
        info->channels < SPA_AUDIO_MAX_CHANNELS) {
        info->position[info->channels++] = pw_pal_get_channel(v);
    }
}

static void pw_pal_fetch_audio_info(const struct pw_properties *props, struct spa_audio_info_raw *info)
{
    const char *str;

    spa_zero(*info);

    if ((str = pw_properties_get(props, PW_KEY_AUDIO_FORMAT)) == NULL)
        str = PW_DEFAULT_SAMPLE_FORMAT;
    info->format = format_from_name(str, strlen(str));

    info->rate = pw_properties_get_uint32(props, PW_KEY_AUDIO_RATE, info->rate);
    if (info->rate == 0)
        info->rate = PW_DEFAULT_SAMPLE_RATE;

    info->channels = pw_properties_get_uint32(props, PW_KEY_AUDIO_CHANNELS, info->channels);
    info->channels = SPA_MIN(info->channels, SPA_AUDIO_MAX_CHANNELS);
    if ((str = pw_properties_get(props, SPA_KEY_AUDIO_POSITION)) != NULL)
        pw_pal_get_parse_position(info, str, strlen(str));
    if (info->channels == 0)
        pw_pal_get_parse_position(info, PW_DEFAULT_SAMPLE_POSITION, strlen(PW_DEFAULT_SAMPLE_POSITION));
}

static int pw_pal_get_frame_size(const struct spa_audio_info_raw *audio_info)
{
    int res = audio_info->channels;
    switch (audio_info->format) {
    case SPA_AUDIO_FORMAT_U8:
    case SPA_AUDIO_FORMAT_S8:
    case SPA_AUDIO_FORMAT_ALAW:
    case SPA_AUDIO_FORMAT_ULAW:
        return res;
    case SPA_AUDIO_FORMAT_S16:
    case SPA_AUDIO_FORMAT_S16_OE:
    case SPA_AUDIO_FORMAT_U16:
        return res * 2;
    case SPA_AUDIO_FORMAT_S24:
    case SPA_AUDIO_FORMAT_S24_OE:
    case SPA_AUDIO_FORMAT_U24:
        return res * 3;
    case SPA_AUDIO_FORMAT_S24_32:
    case SPA_AUDIO_FORMAT_S24_32_OE:
    case SPA_AUDIO_FORMAT_S32:
    case SPA_AUDIO_FORMAT_S32_OE:
    case SPA_AUDIO_FORMAT_U32:
    case SPA_AUDIO_FORMAT_U32_OE:
    case SPA_AUDIO_FORMAT_F32:
    case SPA_AUDIO_FORMAT_F32_OE:
        return res * 4;
    case SPA_AUDIO_FORMAT_F64:
    case SPA_AUDIO_FORMAT_F64_OE:
        return res * 8;
    default:
        return 0;
    }
}

static void pw_pal_set_props(struct pw_userdata *udata, struct pw_properties *props, const char *key)
{
    const char *str;
    if ((str = pw_properties_get(props, key)) != NULL) {
        if (pw_properties_get(udata->stream_props, key) == NULL)
            pw_properties_set(udata->stream_props, key, str);
    }
}

static size_t pw_stream_get_buffer_size(struct pw_userdata *udata, struct pal_media_config spec, pal_stream_type_t type)
{
        uint32_t buffer_duration = PW_DEFAULT_BUFFER_DURATION_MS;
        size_t length = 0, frames = 0;
        switch (type) {
        case PAL_STREAM_DEEP_BUFFER:
            buffer_duration = PW_DEEP_BUFFER_BUFFER_DURATION_MS;
            break;
        case PAL_STREAM_LOW_LATENCY:
            buffer_duration = PW_LOW_LATENCY_BUFFER_DURATION_MS;
        default:
            break;
        }

        frames = spec.sample_rate * buffer_duration;
        length = ((frames * udata->frame_size) / 1000);

        return (length/udata->frame_size) * udata->frame_size;
}
static void pw_pal_fill_stream_info(struct pw_userdata *udata)
{
    udata->stream_attributes = calloc(1, sizeof(struct pal_stream_attributes));
    udata->stream_attributes->type = udata->stream_type;

    udata->stream_attributes->info.opt_stream_info.version = 1;
    udata->stream_attributes->info.opt_stream_info.duration_us = -1;
    udata->stream_attributes->info.opt_stream_info.has_video = false;
    udata->stream_attributes->info.opt_stream_info.is_streaming = false;
    udata->stream_attributes->flags = 0;
    if (udata->isplayback) {
        udata->stream_attributes->direction = PAL_AUDIO_OUTPUT;
        udata->stream_attributes->out_media_config.bit_width = 16;
        udata->stream_attributes->out_media_config.ch_info.ch_map[0] = PAL_CHMAP_CHANNEL_FL;
        udata->stream_attributes->out_media_config.ch_info.ch_map[1] = PAL_CHMAP_CHANNEL_FR;
        udata->sink_buf_count = 4;
        if(!(udata->is_offload)) {
            udata->stream_attributes->out_media_config.sample_rate = udata->info.rate;
            switch (udata->stream_attributes->out_media_config.bit_width) {
                case 32:
                    udata->stream_attributes->out_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S32_LE;
                    break;
                case 24:
                    udata->stream_attributes->out_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S24_3LE;
                    break;
                default:
                    udata->stream_attributes->out_media_config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
                    break;
            }
            udata->stream_attributes->out_media_config.ch_info.channels = udata->info.channels;
            udata->sink_buf_size = pw_stream_get_buffer_size(udata, udata->stream_attributes->out_media_config, udata->stream_type);
        } else {
            udata->stream_attributes->flags  = PAL_STREAM_FLAG_NON_BLOCKING_MASK; /* required in PAL as this a non-blocking call*/
            udata->stream_attributes->out_media_config.sample_rate = 44100 ;
            udata->stream_attributes->out_media_config.ch_info.channels = 2;
            udata->stream_attributes->out_media_config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_COMPRESSED;
            udata->sink_buf_size = 16484;
        }
    } else {
        udata->stream_attributes->direction = PAL_AUDIO_INPUT;
        udata->stream_attributes->in_media_config.sample_rate = udata->info.rate;
        udata->stream_attributes->in_media_config.bit_width = 16;

        switch (udata->stream_attributes->in_media_config.bit_width) {
            case 32:
                udata->stream_attributes->in_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S32_LE;
                break;
            case 24:
                udata->stream_attributes->in_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S24_3LE;
                break;
            default:
                udata->stream_attributes->in_media_config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
                break;
        }

        udata->stream_attributes->in_media_config.ch_info.channels = udata->info.channels;
        udata->stream_attributes->in_media_config.ch_info.ch_map[0] = PAL_CHMAP_CHANNEL_FL;
        udata->stream_attributes->in_media_config.ch_info.ch_map[1] = PAL_CHMAP_CHANNEL_FR;
        udata->source_buf_size = 4096;
        udata->source_buf_count = 2;
    }

    udata->pal_device = calloc(1, sizeof(struct pal_device));
    memset(udata->pal_device, 0, sizeof(struct pal_device));

    udata->pal_device->id = udata->pal_device_id;
    udata->pal_device->config.sample_rate = 48000;
    udata->pal_device->config.bit_width = 16;

    udata->pal_device->config.ch_info.channels = 2;
    udata->pal_device->config.ch_info.ch_map[0] = PAL_CHMAP_CHANNEL_FL;
    udata->pal_device->config.ch_info.ch_map[1] = PAL_CHMAP_CHANNEL_FR;
}

static inline bool pw_stream_is_running(struct pw_userdata *udata)
{
    if (udata == NULL || udata->stream == NULL)
        return false;

    const char *err = NULL;
    enum pw_stream_state st = pw_stream_get_state(udata->stream, &err);

    if (st == PW_STREAM_STATE_ERROR) {
        pw_log_error("%s: stream is in ERROR state%s%s", __func__, err ? ": " : "", err ? err : "");
        return false;
    }

    return st == PW_STREAM_STATE_STREAMING;
}

static int handle_device_connection(struct pw_userdata *udata, bool state)
{
    int ret = 0;
    struct pal_device dev;
    if (!udata) return -EINVAL;

    pw_log_info("%s: processing device connection for jack '%s'", __func__, udata->jack_name);

    if (strstr(udata->jack_name, "DP")) {
        pal_param_device_connection_t *device_connection = (pal_param_device_connection_t *)
            calloc(1, sizeof(pal_param_device_connection_t));
        device_connection->connection_state = state;
        device_connection->id = udata->pal_device_id;
        ret = pal_set_param (PAL_PARAM_ID_DEVICE_CONNECTION, device_connection,
                sizeof(pal_param_device_connection_t));
        free(device_connection);
        return ret;
    }
    else if (strstr(udata->jack_name, "Headset")) {

        if (!pw_stream_is_running(udata) || !udata->stream_handle) {
            pw_log_error("%s: stream not streaming; skip headset routing", __func__);
            return 0;
        }

        const pal_device_id_t target = udata->isplayback
            ? (state ? PAL_DEVICE_OUT_WIRED_HEADSET : PAL_DEVICE_OUT_SPEAKER)
            : (state ? PAL_DEVICE_IN_WIRED_HEADSET  : PAL_DEVICE_IN_SPEAKER_MIC);

        memset(&dev, 0, sizeof(dev));
        dev.id = target;

        ret = pal_stream_set_device(udata->stream_handle, 1, &dev);
        if (ret) {
            pw_log_error("%s: pal_stream_set_device(%d) failed: %d", __func__, target, ret);
            return ret;
        }
        return 0;
    }
}


static void handle_jack_boot_event(struct pw_userdata *udata)
{
    int fd = udata->jack_fd;
    int connected = 0;
    uint8_t sw_bitmask[NUM_BYTES];

    memset(sw_bitmask, 0, sizeof(sw_bitmask));
    // Query current switch state
    if (ioctl(fd, EVIOCGSW(sizeof(sw_bitmask)), sw_bitmask) >= 0) {
        // Check for HDMI/DP jack state
        if (BIT_VALUE(SW_LINEOUT_INSERT, sw_bitmask))
            connected = 1;

        if (connected) {
            pw_log_info("%s: Connected (boot time)", udata->jack_name);
            if(handle_device_connection(udata, true))
                pw_log_error("Failed to handle device connection");
        }
    } else {
        pw_log_error("Failed to query initial jack state");
    }
}

static void on_jack_event(void *userdata, int fd, uint32_t mask)
{
    struct pw_userdata *udata = userdata;
    char name[256] = {0,};
    struct input_event ev;
    bool connected;
    int rc;

    ioctl(fd, EVIOCGNAME(sizeof(name)), name);
    if (!strstr(name, udata->jack_name))
        return;

    if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
        pw_log_error("error or hang-up on the hdmi/dp fd");
        pw_loop_destroy_source(pw_context_get_main_loop(udata->context), udata->jack_src);
        udata->jack_src = NULL;

        if (udata->jack_fd >= 0) {
            close(udata->jack_fd);
            udata->jack_fd = -1;
        }
        return;
    }

    ssize_t ret = read(fd, &ev, sizeof(ev));

    if (ret == sizeof(ev)) {
        if (ev.type != EV_SW)
            return;

        if (ev.code == SW_LINEOUT_INSERT || ev.code == SW_HEADPHONE_INSERT) {
            const char *state = ev.value ? "Connected" : "Disconnected";
            pw_log_info("Jack (%s): %s", udata->jack_name, state);

            if (handle_device_connection(udata, ev.value ? true : false))
                pw_log_error("Failed to handle %s device connection",udata->jack_name);
        }
    } else if (ret < 0) {
        pw_log_error("Error reading event: %s", strerror(errno));
    } else {
        pw_log_error("Short read: got %zd bytes", ret);
    }
}

static int jack_open_fd(struct pw_userdata *udata)
{
    int ret = 0;
    DIR *d = opendir(DEV_INPUT_DIR);
    if (d == NULL) {
        pw_log_error("opendir() failed");
        ret = -ENOENT;
        goto exit;
    }

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        /* Filter out non block devices and files that don't have
           the right prefix. */
        if (dir->d_type != DT_CHR ||
                strncmp(FILE_PREFIX, dir->d_name,
                    strlen(FILE_PREFIX)) != 0)
            continue;

        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s",
            DEV_INPUT_DIR, dir->d_name);
        int fd = open(filepath, O_RDONLY);
        if (fd < 0) {
            pw_log_error("open() failed %s", filepath);
            continue;
        }

        char name[256] = {0,};
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);

        /* Search for the keyword in the input's name. */
        if (strstr(name, udata->jack_name) != NULL) {
            closedir(d);
            return fd;
        }

        close(fd);
    }

    closedir(d);
    ret = -EINVAL;
exit:
    pw_log_error("No jack input device found in %s", DEV_INPUT_DIR);
    return ret;
}

static int jack_register(struct pw_userdata *udata)
{
    int ret = 0;
    udata->jack_fd = jack_open_fd(udata);
    if (udata->jack_fd < 0) {
        ret = -EBADFD;
        goto exit;
    }

    ret = fcntl(udata->jack_fd, F_SETFL, O_NONBLOCK);
    if (ret < 0) {
        pw_log_error("fcntl() failed");
        goto exit;
    }

    udata->jack_src = pw_loop_add_io(pw_context_get_main_loop(udata->context), udata->jack_fd,
            SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP, true, on_jack_event, udata);

    if (udata->jack_src == NULL) {
        pw_log_error("pw_loop_add_io failed for jack_fd=%d", udata->jack_fd);
        ret = -EIO;
    }

exit:
    return ret;
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
    struct pw_context *context = pw_impl_module_get_context(module);
    struct pw_properties *props = NULL;
    const char *offload = NULL;
    uint32_t id = pw_global_get_id(pw_impl_module_get_global(module));
    uint32_t pid = getpid();
    struct pw_userdata *udata;
    const char *str, *value, *role;
    int res = 0;

    PW_LOG_TOPIC_INIT(log_topic);

    udata = calloc(1, sizeof(struct pw_userdata));
    if (udata == NULL)
        return -errno;
    if (args == NULL)
        args = "";

    props = pw_properties_new_string(args);
    if (props == NULL) {
        res = -errno;
        pw_log_error( "can't create properties: %m");
        goto error;
    }
    udata->props = props;

    udata->stream_props = pw_properties_new(NULL, NULL);
    if (udata->stream_props == NULL) {
        res = -errno;
        pw_log_error( "can't create properties: %m");
        goto error;
    }

    udata->module = module;
    udata->context = context;
    res = agm_init();
    if (res) {
        pw_log_error("%s: agm init failed\n", __func__);
        goto error;
    }
    res = pal_init();
    if (res) {
        pw_log_error("%s: pal init failed\n", __func__);
        goto error;
    }
    if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
        pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");

    if (pw_properties_get(props, PW_KEY_MEDIA_CLASS) == NULL)
        pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Sink");

    value = pw_properties_get(props, PW_KEY_MEDIA_CLASS);
    if (value) {
        if (strstr(value, "Sink")) {
            udata->isplayback = true;
            udata->pal_device_id = PAL_DEVICE_OUT_SPEAKER;
        }
        else {
            udata->isplayback = false;
            udata->pal_device_id = PAL_DEVICE_IN_SPEAKER_MIC;
        }
    }

    value = pw_properties_get(props, PW_KEY_NODE_NAME);
    if (value) {
        if (strstr(value, "pal_sink_speaker"))
            udata->pal_device_id = PAL_DEVICE_OUT_SPEAKER;
        else if (strstr(value, "pal_sink_headset"))
            udata->pal_device_id = PAL_DEVICE_OUT_WIRED_HEADSET;
        else if (strstr(value, "pal_source_speaker_mic"))
            udata->pal_device_id = PAL_DEVICE_IN_SPEAKER_MIC;
        else if (strstr(value, "pal_source_headset_mic"))
            udata->pal_device_id = PAL_DEVICE_IN_WIRED_HEADSET;
        else if (strstr(value, "pal_sink_dp_out"))
            udata->pal_device_id = PAL_DEVICE_OUT_AUX_DIGITAL;
        else if (strstr(value, "pal_sink_hdmi_out"))
            udata->pal_device_id = PAL_DEVICE_OUT_HDMI;
    }

    if (pw_properties_get(props, PW_KEY_MEDIA_ROLE) == NULL)
        pw_properties_set(props, PW_KEY_MEDIA_ROLE, "notification");

    role = pw_properties_get(props, PW_KEY_MEDIA_ROLE);
    if (role && (udata->isplayback)) {
        if (strstr(role, "music"))
            udata->stream_type = PAL_STREAM_DEEP_BUFFER;
        else
            udata->stream_type = PAL_STREAM_LOW_LATENCY;
    } else if (!(udata->isplayback))
          udata->stream_type = PAL_STREAM_DEEP_BUFFER;

    if (pw_properties_get(props, PW_KEY_NODE_NAME) == NULL)
        pw_properties_setf(props, PW_KEY_NODE_NAME, "example-sink-%u-%u", pid, id);

    if (pw_properties_get(props, PW_KEY_NODE_DESCRIPTION) == NULL)
        pw_properties_set(props, PW_KEY_NODE_DESCRIPTION,
        pw_properties_get(props, PW_KEY_NODE_NAME));

    if ((str = pw_properties_get(props, "stream.props")) != NULL)
        pw_properties_update_string(udata->stream_props, str, strlen(str));

    offload = pw_properties_get(udata->stream_props, "compress.offload");
    udata->is_offload = offload && strcmp(offload, "true") == 0;

    if ((str = pw_properties_get(props, "jack-name")) != NULL)
        snprintf(udata->jack_name, sizeof(udata->jack_name), "%s", str);

    pw_pal_set_props(udata, props, PW_KEY_AUDIO_RATE);
    pw_pal_set_props(udata, props, PW_KEY_AUDIO_CHANNELS);
    pw_pal_set_props(udata, props, SPA_KEY_AUDIO_POSITION);
    pw_pal_set_props(udata, props, PW_KEY_NODE_NAME);
    pw_pal_set_props(udata, props, PW_KEY_NODE_DESCRIPTION);
    pw_pal_set_props(udata, props, PW_KEY_NODE_GROUP);
    pw_pal_set_props(udata, props, PW_KEY_NODE_LATENCY);
    pw_pal_set_props(udata, props, PW_KEY_NODE_VIRTUAL);
    pw_pal_set_props(udata, props, PW_KEY_MEDIA_CLASS);

    pw_pal_fetch_audio_info(udata->stream_props, &udata->info);
    if (!udata->is_offload) {
        udata->frame_size = pw_pal_get_frame_size(&udata->info);
        if (udata->frame_size == 0) {
            res = -EINVAL;
            pw_log_error( "can't parse audio format");
            goto error;
        }
    } else {
        udata->stream_type = PAL_STREAM_COMPRESSED;
        udata->frame_size = 16;
        pw_properties_set(udata->stream_props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
        pw_properties_set(udata->stream_props, PW_KEY_AUDIO_FORMAT, "encoded");
        pw_properties_set(udata->stream_props, "audio.coding.format", "mp3");
    }
    udata->core = pw_context_get_object(udata->context, PW_TYPE_INTERFACE_Core);
    if (udata->core == NULL) {
        str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
        udata->core = pw_context_connect(udata->context,
                pw_properties_new(
                    PW_KEY_REMOTE_NAME, str,
                    NULL),
                0);
        udata->do_disconnect = true;
    }

    if (udata->core == NULL) {
        res = -errno;
        pw_log_error("can't connect: %m");
        goto error;
    }

    pw_proxy_add_listener((struct pw_proxy*)udata->core,
            &udata->core_proxy_listener,
            &pw_pal_proxy_events_core, udata);
    pw_core_add_listener(udata->core,
            &udata->core_listener,
            &pw_pal_events_core, udata);
    pw_pal_fill_stream_info(udata);
    if ((res = pw_pal_create_stream(udata)) < 0)
        goto error;
    pw_impl_module_add_listener(module, &udata->module_listener, &pw_pal_events_module, udata);
    if (udata->jack_name && udata->jack_name[0] != '\0') {
        if(jack_register(udata))
            pw_log_error("failed to register jack event for %s", udata->jack_name);
        else {
            handle_jack_boot_event(udata);
        }
    }
    return 0;

error:
    pw_pal_userdata_destroy(udata);
    return res;
}
