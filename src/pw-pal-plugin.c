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

PW_LOG_TOPIC_STATIC(log_topic, "log:" LOG_TAG);
#define PW_LOG_TOPIC_DEFAULT log_topic

#define PW_DEFAULT_SAMPLE_FORMAT "S16"
#define PW_DEFAULT_SAMPLE_RATE 48000
#define PW_DEFAULT_SAMPLE_CHANNELS 2
#define PW_DEFAULT_SAMPLE_POSITION "[ FL FR ]"

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
        out_buf_cfg.buf_size = 1024;
        out_buf_cfg.buf_count = 4;
    } else {
        out_buf_cfg.buf_size = 0;
        out_buf_cfg.buf_count = 0;
        in_buf_cfg.buf_size = 512;
        in_buf_cfg.buf_count = 8;
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
    if (udata->format.media_type != SPA_MEDIA_TYPE_audio ||
            udata->format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
            return;
    spa_format_audio_raw_parse(param, &udata->format.info.raw);

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
        params[n_params++] = spa_pod_builder_add_object(&b,
                        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                        SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(4),
                        SPA_PARAM_BUFFERS_blocks,  0,
                        SPA_PARAM_BUFFERS_size,    SPA_POD_Int(1024),
                        SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(16));
    } else {
        udata->stream = pw_stream_new(udata->core, "example source", udata->stream_props);
        params[n_params++] = spa_pod_builder_add_object(&b,
                        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                        SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(8),
                        SPA_PARAM_BUFFERS_blocks,  0,
                        SPA_PARAM_BUFFERS_size,    SPA_POD_Int(512),
                        SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(16));
    }

    if (udata->stream == NULL)
        return -errno;

    pw_stream_add_listener(udata->stream,
            &udata->stream_listener,
            &pw_pal_stream_events, udata);

    params[n_params++] = spa_format_audio_raw_build(&b,
            SPA_PARAM_EnumFormat, &udata->info);

    if (udata->isplayback) {
        if ((res = pw_stream_connect(udata->stream,
                PW_DIRECTION_INPUT,
                PW_ID_ANY,
                PW_STREAM_FLAG_AUTOCONNECT |
                PW_STREAM_FLAG_NO_CONVERT  |
                PW_STREAM_FLAG_MAP_BUFFERS |
                PW_STREAM_FLAG_RT_PROCESS,
                params, n_params)) < 0)
            return res;
    }
    else {
        if ((res = pw_stream_connect(udata->stream,
                PW_DIRECTION_OUTPUT,
                PW_ID_ANY,
                PW_STREAM_FLAG_AUTOCONNECT |
                PW_STREAM_FLAG_NO_CONVERT  |
                PW_STREAM_FLAG_MAP_BUFFERS |
                PW_STREAM_FLAG_RT_PROCESS,
                params, n_params)) < 0)
            return res;
    }

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
        udata->stream_attributes->out_media_config.sample_rate = udata->info.rate;
        udata->stream_attributes->out_media_config.bit_width = 16;

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

        udata->stream_attributes->out_media_config.ch_info.channels = 2;
        udata->stream_attributes->out_media_config.ch_info.ch_map[0] = PAL_CHMAP_CHANNEL_FL;
        udata->stream_attributes->out_media_config.ch_info.ch_map[1] = PAL_CHMAP_CHANNEL_FR;
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

        udata->stream_attributes->in_media_config.ch_info.channels = 2;
        udata->stream_attributes->in_media_config.ch_info.ch_map[0] = PAL_CHMAP_CHANNEL_FL;
        udata->stream_attributes->in_media_config.ch_info.ch_map[1] = PAL_CHMAP_CHANNEL_FR;
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

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
    struct pw_context *context = pw_impl_module_get_context(module);
    struct pw_properties *props = NULL;
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

    udata->frame_size = pw_pal_get_frame_size(&udata->info);
    if (udata->frame_size == 0) {
        res = -EINVAL;
        pw_log_error( "can't parse audio format");
        goto error;
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
    return 0;

error:
    pw_pal_userdata_destroy(udata);
    return res;
}
