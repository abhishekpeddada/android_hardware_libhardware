/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "usb_audio_hw"
/*#define LOG_NDEBUG 0*/

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <stdlib.h>

#include <cutils/log.h>
#include <cutils/str_parms.h>
#include <cutils/properties.h>

#include <hardware/hardware.h>
#include <system/audio.h>
#include <hardware/audio.h>

#include <tinyalsa/asoundlib.h>

/* This is the default configuration to hand to The Framework on the initial
 * adev_open_output_stream(). Actual device attributes will be used on the subsequent
 * adev_open_output_stream() after the card and device number have been set in out_set_parameters()
 */
#define OUT_PERIOD_SIZE 1024
#define OUT_PERIOD_COUNT 4
#define OUT_SAMPLING_RATE 44100

struct pcm_config default_alsa_out_config = {
    .channels = 2,
    .rate = OUT_SAMPLING_RATE,
    .period_size = OUT_PERIOD_SIZE,
    .period_count = OUT_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

/*
 * Input defaults.  See comment above.
 */
#define IN_PERIOD_SIZE 1024
#define IN_PERIOD_COUNT 4
#define IN_SAMPLING_RATE 44100

struct pcm_config default_alsa_in_config = {
    .channels = 2,
    .rate = IN_SAMPLING_RATE,
    .period_size = IN_PERIOD_SIZE,
    .period_count = IN_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = 1,
    .stop_threshold = (IN_PERIOD_SIZE * IN_PERIOD_COUNT),
};

struct audio_device {
    struct audio_hw_device hw_device;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */

    /* output */
    int out_card;
    int out_device;

    /* input */
    int in_card;
    int in_device;

    bool standby;
};

struct stream_out {
    struct audio_stream_out stream;

    pthread_mutex_t lock;               /* see note below on mutex acquisition order */
    struct pcm *pcm;                    /* state of the stream */
    bool standby;

    struct audio_device *dev;           /* hardware information */

    void * conversion_buffer;           /* any conversions are put into here
                                         * they could come from here too if
                                         * there was a previous conversion */
    size_t conversion_buffer_size;      /* in bytes */
};

/*
 * Output Configuration Cache
 * FIXME(pmclean) This is not rentrant. Should probably be moved into the stream structure
 * but that will involve changes in The Framework.
 */
static struct pcm_config cached_output_hardware_config;
static bool output_hardware_config_is_cached = false;

struct stream_in {
    struct audio_stream_in stream;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    struct pcm *pcm;
    bool standby;

    struct pcm_config alsa_pcm_config;

    struct audio_device *dev;

    struct audio_config hal_pcm_config;

    unsigned int requested_rate;
//    struct resampler_itfe *resampler;
//    struct resampler_buffer_provider buf_provider;
    int16_t *buffer;
    size_t buffer_size;
    size_t frames_in;
    int read_status;
};

/*
 * Utility
 */
/*
 * Translates from ALSA format ID to ANDROID_AUDIO_CORE format ID
 * (see master/system/core/include/core/audio.h)
 * TODO(pmclean) Replace with audio_format_from_pcm_format() (in hardware/audio_alsaops.h).
 *   post-integration.
 */
static audio_format_t alsa_to_fw_format_id(int alsa_fmt_id)
{
    switch (alsa_fmt_id) {
    case PCM_FORMAT_S8:
        return AUDIO_FORMAT_PCM_8_BIT;

    case PCM_FORMAT_S24_3LE:
        //TODO(pmclean) make sure this is the 'right' sort of 24-bit
        return AUDIO_FORMAT_PCM_8_24_BIT;

    case PCM_FORMAT_S32_LE:
    case PCM_FORMAT_S24_LE:
        return AUDIO_FORMAT_PCM_32_BIT;
    }

    return AUDIO_FORMAT_PCM_16_BIT;
}

/*
 * Data Conversions
 */
/*
 * Convert a buffer of PCM16LE samples to packed (3-byte) PCM24LE samples.
 *   in_buff points to the buffer of PCM16 samples
 *   num_in_samples size of input buffer in SAMPLES
 *   out_buff points to the buffer to receive converted PCM24 LE samples.
 *   returns the number of BYTES of output data.
 * We are doing this since we *always* present to The Framework as A PCM16LE device, but need to
 * support PCM24_3LE (24-bit, packed).
 * NOTE: we're just filling the low-order byte of the PCM24LE samples with 0.
 * TODO(pmclean, hung) Move this to a utilities module.
 */
static size_t convert_16_to_24_3(unsigned short * in_buff,
                                 size_t num_in_samples,
                                 unsigned char * out_buff) {
    /*
     * Move from back to front so that the conversion can be done in-place
     * i.e. in_buff == out_buff
     */
    int in_buff_size_in_bytes = num_in_samples * 2;
    /* we need 3 bytes in the output for every 2 bytes in the input */
    int out_buff_size_in_bytes = ((3 * in_buff_size_in_bytes) / 2);
    unsigned char* dst_ptr = out_buff + out_buff_size_in_bytes - 1;
    int src_smpl_index;
    unsigned char* src_ptr = ((unsigned char *)in_buff) + in_buff_size_in_bytes - 1;
    for (src_smpl_index = 0; src_smpl_index < num_in_samples; src_smpl_index++) {
        *dst_ptr-- = *src_ptr--; /* hi-byte */
        *dst_ptr-- = *src_ptr--; /* low-byte */
        *dst_ptr-- = 0;          /* zero-byte */
    }

    /* return number of *bytes* generated */
    return out_buff_size_in_bytes;
}

/*
 * Convert a buffer of 2-channel PCM16 samples to 4-channel PCM16 channels
 *   in_buff points to the buffer of PCM16 samples
 *   num_in_samples size of input buffer in SAMPLES
 *   out_buff points to the buffer to receive converted PCM16 samples.
 *   returns the number of BYTES of output data.
 * NOTE channels 3 & 4 are filled with silence.
 * We are doing this since we *always* present to The Framework as STEREO device, but need to
 * support 4-channel devices.
 * TODO(pmclean, hung) Move this to a utilities module.
 */
static size_t convert_2chan16_to_4chan16(unsigned short* in_buff,
                                          size_t num_in_samples,
                                          unsigned short* out_buff) {
    /*
     * Move from back to front so that the conversion can be done in-place
     * i.e. in_buff == out_buff
     */
    int out_buff_size = num_in_samples * 2;
    unsigned short* dst_ptr = out_buff + out_buff_size - 1;
    int src_index;
    unsigned short* src_ptr = in_buff + num_in_samples - 1;
    for (src_index = 0; src_index < num_in_samples; src_index += 2) {
        *dst_ptr-- = 0;          /* chan 4 */
        *dst_ptr-- = 0;          /* chan 3 */
        *dst_ptr-- = *src_ptr--; /* chan 2 */
        *dst_ptr-- = *src_ptr--; /* chan 1 */
    }

    /* return number of *bytes* generated */
    return out_buff_size * 2;
}

/*
 * ALSA Utilities
 */
/*
 * gets the ALSA bit-format flag from a bits-per-sample value.
 * TODO(pmclean, hung) Move this to a utilities module.
 */
static int bits_to_alsa_format(int bits_per_sample, int default_format)
{
    enum pcm_format format;
    for (format = PCM_FORMAT_S16_LE; format < PCM_FORMAT_MAX; format++) {
        if (pcm_format_to_bits(format) == bits_per_sample) {
            return  format;
         }
    }
    return default_format;
}

/*
 * Reads and decodes configuration info from the specified ALSA card/device
 */
static int read_alsa_device_config(int card, int device, int io_type, struct pcm_config * config)
{
    ALOGV("usb:audio_hw - read_alsa_device_config(card:%d device:%d)", card, device);

    if (card < 0 || device < 0) {
        return -EINVAL;
    }

    struct pcm_params * alsa_hw_params = pcm_params_get(card, device, io_type);
    if (alsa_hw_params == NULL) {
        return -EINVAL;
    }

    /*
     * This Logging will be useful when testing new USB devices.
     */
    /* ALOGV("usb:audio_hw - PCM_PARAM_SAMPLE_BITS min:%d, max:%d", pcm_params_get_min(alsa_hw_params, PCM_PARAM_SAMPLE_BITS), pcm_params_get_max(alsa_hw_params, PCM_PARAM_SAMPLE_BITS)); */
    /* ALOGV("usb:audio_hw - PCM_PARAM_FRAME_BITS min:%d, max:%d", pcm_params_get_min(alsa_hw_params, PCM_PARAM_FRAME_BITS), pcm_params_get_max(alsa_hw_params, PCM_PARAM_FRAME_BITS)); */
    /* ALOGV("usb:audio_hw - PCM_PARAM_CHANNELS min:%d, max:%d", pcm_params_get_min(alsa_hw_params, PCM_PARAM_CHANNELS), pcm_params_get_max(alsa_hw_params, PCM_PARAM_CHANNELS)); */
    /* ALOGV("usb:audio_hw - PCM_PARAM_RATE min:%d, max:%d", pcm_params_get_min(alsa_hw_params, PCM_PARAM_RATE), pcm_params_get_max(alsa_hw_params, PCM_PARAM_RATE)); */
    /* ALOGV("usb:audio_hw - PCM_PARAM_PERIOD_TIME min:%d, max:%d", pcm_params_get_min(alsa_hw_params, PCM_PARAM_PERIOD_TIME), pcm_params_get_max(alsa_hw_params, PCM_PARAM_PERIOD_TIME)); */
    /* ALOGV("usb:audio_hw - PCM_PARAM_PERIOD_SIZE min:%d, max:%d", pcm_params_get_min(alsa_hw_params, PCM_PARAM_PERIOD_SIZE), pcm_params_get_max(alsa_hw_params, PCM_PARAM_PERIOD_SIZE)); */
    /* ALOGV("usb:audio_hw - PCM_PARAM_PERIOD_BYTES min:%d, max:%d", pcm_params_get_min(alsa_hw_params, PCM_PARAM_PERIOD_BYTES), pcm_params_get_max(alsa_hw_params, PCM_PARAM_PERIOD_BYTES)); */
    /* ALOGV("usb:audio_hw - PCM_PARAM_PERIODS min:%d, max:%d", pcm_params_get_min(alsa_hw_params, PCM_PARAM_PERIODS), pcm_params_get_max(alsa_hw_params, PCM_PARAM_PERIODS)); */
    /* ALOGV("usb:audio_hw - PCM_PARAM_BUFFER_TIME min:%d, max:%d", pcm_params_get_min(alsa_hw_params, PCM_PARAM_BUFFER_TIME), pcm_params_get_max(alsa_hw_params, PCM_PARAM_BUFFER_TIME)); */
    /* ALOGV("usb:audio_hw - PCM_PARAM_BUFFER_SIZE min:%d, max:%d", pcm_params_get_min(alsa_hw_params, PCM_PARAM_BUFFER_SIZE), pcm_params_get_max(alsa_hw_params, PCM_PARAM_BUFFER_SIZE)); */
    /* ALOGV("usb:audio_hw - PCM_PARAM_BUFFER_BYTES min:%d, max:%d", pcm_params_get_min(alsa_hw_params, PCM_PARAM_BUFFER_BYTES), pcm_params_get_max(alsa_hw_params, PCM_PARAM_BUFFER_BYTES)); */
    /* ALOGV("usb:audio_hw - PCM_PARAM_TICK_TIME min:%d, max:%d", pcm_params_get_min(alsa_hw_params, PCM_PARAM_TICK_TIME), pcm_params_get_max(alsa_hw_params, PCM_PARAM_TICK_TIME)); */

    config->channels = pcm_params_get_min(alsa_hw_params, PCM_PARAM_CHANNELS);
    config->rate = pcm_params_get_min(alsa_hw_params, PCM_PARAM_RATE);
    config->period_size = pcm_params_get_max(alsa_hw_params, PCM_PARAM_PERIODS);
    config->period_count = pcm_params_get_min(alsa_hw_params, PCM_PARAM_PERIODS);

    int bits_per_sample = pcm_params_get_min(alsa_hw_params, PCM_PARAM_SAMPLE_BITS);
    config->format = bits_to_alsa_format(bits_per_sample, PCM_FORMAT_S16_LE);

    return 0;
}

/*
 * HAl Functions
 */
/**
 * NOTE: when multiple mutexes have to be acquired, always respect the
 * following order: hw device > out stream
 */

/* Helper functions */
static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    return cached_output_hardware_config.rate;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return 0;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    return cached_output_hardware_config.period_size * audio_stream_frame_size(stream);
}

static uint32_t out_get_channels(const struct audio_stream *stream)
{
    // Always Stero for now. We will do *some* conversions in this HAL.
    // TODO(pmclean) When AudioPolicyManager & AudioFlinger supports arbitrary channels
    // rewrite this to return the ACTUAL channel format
    return AUDIO_CHANNEL_OUT_STEREO;
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
    // Always return 16-bit PCM. We will do *some* conversions in this HAL.
    // TODO(pmclean) When AudioPolicyManager & AudioFlinger supports arbitrary PCM formats
    // rewrite this to return the ACTUAL data format
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int out_set_format(struct audio_stream *stream, audio_format_t format)
{
    return 0;
}

static int out_standby(struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);

    if (!out->standby) {
        pcm_close(out->pcm);
        out->pcm = NULL;
        out->standby = true;
    }

    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);

    return 0;
}

static int out_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    ALOGV("usb:audio_hw::out out_set_parameters() keys:%s", kvpairs);

    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    struct str_parms *parms;
    char value[32];
    int param_val;
    int routing = 0;
    int ret_value = 0;

    parms = str_parms_create_str(kvpairs);
    pthread_mutex_lock(&adev->lock);

    bool recache_device_params = false;
    param_val = str_parms_get_str(parms, "card", value, sizeof(value));
    if (param_val >= 0) {
        adev->out_card = atoi(value);
        recache_device_params = true;
    }

    param_val = str_parms_get_str(parms, "device", value, sizeof(value));
    if (param_val >= 0) {
        adev->out_device = atoi(value);
        recache_device_params = true;
    }

    if (recache_device_params && adev->out_card >= 0 && adev->out_device >= 0) {
        ret_value = read_alsa_device_config(adev->out_card, adev->out_device, PCM_OUT,
                                            &(cached_output_hardware_config));
        output_hardware_config_is_cached = (ret_value == 0);
    }

    pthread_mutex_unlock(&adev->lock);
    str_parms_destroy(parms);

    return ret_value;
}

//TODO(pmclean) it seems like both out_get_parameters() and in_get_parameters()
// could be written in terms of a get_device_parameters(io_type)

static char * out_get_parameters(const struct audio_stream *stream, const char *keys) {
    struct stream_out *out = (struct stream_out *) stream;
    struct audio_device *adev = out->dev;

    unsigned min, max;

    struct str_parms *query = str_parms_create_str(keys);
    struct str_parms *result = str_parms_create();

    int num_written = 0;
    char buffer[256];
    int buffer_size = sizeof(buffer) / sizeof(buffer[0]);
    char* result_str = NULL;

    struct pcm_params * alsa_hw_params = pcm_params_get(adev->out_card, adev->out_device, PCM_OUT);

    // These keys are from hardware/libhardware/include/audio.h
    // supported sample rates
    if (str_parms_has_key(query, AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES)) {
        // pcm_hw_params doesn't have a list of supported samples rates, just a min and a max, so
        // if they are different, return a list containing those two values, otherwise just the one.
        min = pcm_params_get_min(alsa_hw_params, PCM_PARAM_RATE);
        max = pcm_params_get_max(alsa_hw_params, PCM_PARAM_RATE);
        num_written = snprintf(buffer, buffer_size, "%d", min);
        if (min != max) {
            snprintf(buffer + num_written, buffer_size - num_written, "|%d",
                     max);
        }
        str_parms_add_str(result, AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES,
                          buffer);
    }  // AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES

    // supported channel counts
    if (str_parms_has_key(query, AUDIO_PARAMETER_STREAM_SUP_CHANNELS)) {
        // Similarly for output channels count
        min = pcm_params_get_min(alsa_hw_params, PCM_PARAM_CHANNELS);
        max = pcm_params_get_max(alsa_hw_params, PCM_PARAM_CHANNELS);
        num_written = snprintf(buffer, buffer_size, "%d", min);
        if (min != max) {
            snprintf(buffer + num_written, buffer_size - num_written, "|%d", max);
        }
        str_parms_add_str(result, AUDIO_PARAMETER_STREAM_SUP_CHANNELS, buffer);
    }  // AUDIO_PARAMETER_STREAM_SUP_CHANNELS

    // supported sample formats
    if (str_parms_has_key(query, AUDIO_PARAMETER_STREAM_SUP_FORMATS)) {
        // Similarly for output channels count
        //TODO(pmclean): this is wrong.
        min = pcm_params_get_min(alsa_hw_params, PCM_PARAM_SAMPLE_BITS);
        max = pcm_params_get_max(alsa_hw_params, PCM_PARAM_SAMPLE_BITS);
        num_written = snprintf(buffer, buffer_size, "%d", min);
        if (min != max) {
            snprintf(buffer + num_written, buffer_size - num_written, "|%d", max);
        }
        str_parms_add_str(result, AUDIO_PARAMETER_STREAM_SUP_FORMATS, buffer);
    }  // AUDIO_PARAMETER_STREAM_SUP_FORMATS

    result_str = str_parms_to_str(result);

    // done with these...
    str_parms_destroy(query);
    str_parms_destroy(result);

    return result_str;
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    //TODO(pmclean): Do we need a term here for the USB latency
    // (as reported in the USB descriptors)?
    uint32_t latency = (cached_output_hardware_config.period_size *
        cached_output_hardware_config.period_count * 1000) / out_get_sample_rate(&stream->common);
    return latency;
}

static int out_set_volume(struct audio_stream_out *stream, float left,
                          float right)
{
    return -ENOSYS;
}

/* must be called with hw device and output stream mutexes locked */
static int start_output_stream(struct stream_out *out)
{
    struct audio_device *adev = out->dev;
    int return_val = 0;

     ALOGV("usb:audio_hw::out start_output_stream(card:%d device:%d)",
           adev->out_card, adev->out_device);

    out->pcm = pcm_open(adev->out_card, adev->out_device, PCM_OUT, &cached_output_hardware_config);
    if (out->pcm == NULL) {
        return -ENOMEM;
    }

    if (out->pcm && !pcm_is_ready(out->pcm)) {
        ALOGE("audio_hw audio_hw pcm_open() failed: %s", pcm_get_error(out->pcm));
        pcm_close(out->pcm);
        return -ENOMEM;
    }

    // Setup conversion buffer
    size_t buffer_size = out_get_buffer_size(&(out->stream.common));

    // computer maximum potential buffer size.
    // * 2 for stereo -> quad conversion
    // * 3/2 for 16bit -> 24 bit conversion
    //TODO(pmclean) - remove this when AudioPolicyManger/AudioFlinger support arbitrary formats
    // (and do these conversions themselves)
    out->conversion_buffer_size = (buffer_size * 3 * 2) / 2;
    out->conversion_buffer = realloc(out->conversion_buffer, out->conversion_buffer_size);

    return 0;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer, size_t bytes)
{
    int ret;
    struct stream_out *out = (struct stream_out *)stream;

    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);
    if (out->standby) {
        ret = start_output_stream(out);
        if (ret != 0) {
            goto err;
        }
        out->standby = false;
    }

    void * write_buff = buffer;
    int num_write_buff_bytes = bytes;

    /*
     * Num Channels conversion
     */
    int num_device_channels = cached_output_hardware_config.channels;
    int num_req_channels = 2; /* always, for now */
    if (num_device_channels != num_req_channels && num_device_channels == 4) {
        num_write_buff_bytes =
                convert_2chan16_to_4chan16(write_buff, num_write_buff_bytes / 2,
                                           out->conversion_buffer);
        write_buff = out->conversion_buffer;
    }

    /*
     *  16 vs 24-bit logic here
     */
    switch (cached_output_hardware_config.format) {
    case PCM_FORMAT_S16_LE:
        // the output format is the same as the input format, so just write it out
        break;

    case PCM_FORMAT_S24_3LE:
        // 16-bit LE2 - 24-bit LE3
        num_write_buff_bytes =
                convert_16_to_24_3(write_buff, num_write_buff_bytes / 2, out->conversion_buffer);
        write_buff = out->conversion_buffer;
        break;

    default:
        // hmmmmm.....
        ALOGV("usb:Unknown Format!!!");
        break;
    }

    if (write_buff != NULL && num_write_buff_bytes != 0) {
        pcm_write(out->pcm, write_buff, num_write_buff_bytes);
    }

    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);

    return bytes;

err:
    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);
    if (ret != 0) {
        usleep(bytes * 1000000 / audio_stream_frame_size(&stream->common) /
               out_get_sample_rate(&stream->common));
    }

    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames)
{
    return -EINVAL;
}

static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
                                        int64_t *timestamp)
{
    return -EINVAL;
}

static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out)
{
    ALOGV("usb:audio_hw::out adev_open_output_stream() handle:0x%X, devices:0x%X, flags:0x%X",
          handle, devices, flags);

    struct audio_device *adev = (struct audio_device *)dev;

    struct stream_out *out;

    out = (struct stream_out *)calloc(1, sizeof(struct stream_out));
    if (!out)
        return -ENOMEM;

    // setup function pointers
    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;

    out->dev = adev;

    if (output_hardware_config_is_cached) {
        config->sample_rate = cached_output_hardware_config.rate;

        config->format = alsa_to_fw_format_id(cached_output_hardware_config.format);
        if (config->format != AUDIO_FORMAT_PCM_16_BIT) {
            // Always report PCM16 for now. AudioPolicyManagerBase/AudioFlinger dont' understand
            // formats with more other format, so we won't get chosen (say with a 24bit DAC).
            //TODO(pmclean) remove this when the above restriction is removed.
            config->format = AUDIO_FORMAT_PCM_16_BIT;
        }

        config->channel_mask =
                audio_channel_out_mask_from_count(cached_output_hardware_config.channels);
        if (config->channel_mask != AUDIO_CHANNEL_OUT_STEREO) {
            // Always report STEREO for now.  AudioPolicyManagerBase/AudioFlinger dont' understand
            // formats with more channels, so we won't get chosen (say with a 4-channel DAC).
            //TODO(pmclean) remove this when the above restriction is removed.
            config->channel_mask = AUDIO_CHANNEL_OUT_STEREO;
        }
    } else {
        cached_output_hardware_config = default_alsa_out_config;

        config->format = out_get_format(&out->stream.common);
        config->channel_mask = out_get_channels(&out->stream.common);
        config->sample_rate = out_get_sample_rate(&out->stream.common);
    }
    ALOGV("usb:audio_hw  config->sample_rate:%d", config->sample_rate);
    ALOGV("usb:audio_hw  config->format:0x%X", config->format);
    ALOGV("usb:audio_hw  config->channel_mask:0x%X", config->channel_mask);

    out->conversion_buffer = NULL;
    out->conversion_buffer_size = 0;

    out->standby = true;

    *stream_out = &out->stream;
    return 0;

err_open:
    free(out);
    *stream_out = NULL;
    return -ENOSYS;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    ALOGV("usb:audio_hw::out adev_close_output_stream()");
    struct stream_out *out = (struct stream_out *)stream;

    //TODO(pmclean) why are we doing this when stream get's freed at the end
    // because it closes the pcm device
    out_standby(&stream->common);

    free(out->conversion_buffer);
    out->conversion_buffer = NULL;
    out->conversion_buffer_size = 0;

    free(stream);
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    return 0;
}

static char * adev_get_parameters(const struct audio_hw_device *dev,
                                  const char *keys)
{
    return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev)
{
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    return -ENOSYS;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    return -ENOSYS;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
                                         const struct audio_config *config)
{
    return 0;
}

/* Helper functions */
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;
    return in->alsa_pcm_config.rate;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return -ENOSYS;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;
    size_t buff_size =
            in->alsa_pcm_config.period_size
            * audio_stream_frame_size((struct audio_stream *)stream);
    return buff_size;
}

static uint32_t in_get_channels(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;
    //TODO(pmclean) this should be done with a num_channels_to_alsa_channels()
    return in->alsa_pcm_config.channels == 2
            ? AUDIO_CHANNEL_IN_STEREO : AUDIO_CHANNEL_IN_MONO;
}

static audio_format_t in_get_format(const struct audio_stream *stream)
{
    // just report 16-bit, pcm for now.
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int in_set_format(struct audio_stream *stream, audio_format_t format)
{
    return -ENOSYS;
}

static int in_standby(struct audio_stream *stream)
{
    ALOGV("-pcm-audio_hw::in in_standby() [Not Implemented]");
    return 0;
}

static int in_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    ALOGV("Vaudio_hw::in in_set_parameters() keys:%s", kvpairs);

    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    struct str_parms *parms;
    char value[32];
    int param_val;
    int routing = 0;
    int ret_value = 0;

    parms = str_parms_create_str(kvpairs);
    pthread_mutex_lock(&adev->lock);

    // Card/Device
    param_val = str_parms_get_str(parms, "card", value, sizeof(value));
    if (param_val >= 0) {
        adev->in_card = atoi(value);
    }

    param_val = str_parms_get_str(parms, "device", value, sizeof(value));
    if (param_val >= 0) {
        adev->in_device = atoi(value);
    }

    if (adev->in_card >= 0 && adev->in_device >= 0) {
        ret_value = read_alsa_device_config(adev->in_card, adev->in_device, PCM_IN, &(in->alsa_pcm_config));
    }

    pthread_mutex_unlock(&adev->lock);
    str_parms_destroy(parms);

    return ret_value;
}

//TODO(pmclean) it seems like both out_get_parameters() and in_get_parameters()
// could be written in terms of a get_device_parameters(io_type)

static char * in_get_parameters(const struct audio_stream *stream, const char *keys)
{
  ALOGV("usb:audio_hw::in in_get_parameters() keys:%s", keys);

  struct stream_in *in = (struct stream_in *)stream;
  struct audio_device *adev = in->dev;

  struct pcm_params * alsa_hw_params = pcm_params_get(adev->in_card, adev->in_device, PCM_IN);
  if (alsa_hw_params == NULL)
      return strdup("");

  struct str_parms *query = str_parms_create_str(keys);
  struct str_parms *result = str_parms_create();

  int num_written = 0;
  char buffer[256];
  int buffer_size = sizeof(buffer)/sizeof(buffer[0]);
  char* result_str = NULL;

  unsigned min, max;

  // These keys are from hardware/libhardware/include/audio.h
  // supported sample rates
  if (str_parms_has_key(query, AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES)) {
    // pcm_hw_params doesn't have a list of supported samples rates, just a min and a max, so
    // if they are different, return a list containing those two values, otherwise just the one.
    min = pcm_params_get_min(alsa_hw_params, PCM_PARAM_RATE);
    max = pcm_params_get_max(alsa_hw_params, PCM_PARAM_RATE);
    num_written = snprintf(buffer, buffer_size, "%d", min);
    if (min != max) {
      snprintf(buffer + num_written, buffer_size - num_written, "|%d", max);
    }
    str_parms_add_str(result, AUDIO_PARAMETER_STREAM_SAMPLING_RATE, buffer);
  } // AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES

  // supported channel counts
  if (str_parms_has_key(query, AUDIO_PARAMETER_STREAM_SUP_CHANNELS)) {
    // Similarly for output channels count
    min = pcm_params_get_min(alsa_hw_params, PCM_PARAM_CHANNELS);
    max = pcm_params_get_max(alsa_hw_params, PCM_PARAM_CHANNELS);
    num_written = snprintf(buffer, buffer_size, "%d", min);
    if (min != max) {
      snprintf(buffer + num_written, buffer_size - num_written, "|%d", max);
    }
    str_parms_add_str(result, AUDIO_PARAMETER_STREAM_CHANNELS, buffer);
  } // AUDIO_PARAMETER_STREAM_SUP_CHANNELS

  // supported sample formats
  if (str_parms_has_key(query, AUDIO_PARAMETER_STREAM_SUP_FORMATS)) {
    //TODO(pmclean): this is wrong.
    min = pcm_params_get_min(alsa_hw_params, PCM_PARAM_SAMPLE_BITS);
    max = pcm_params_get_max(alsa_hw_params, PCM_PARAM_SAMPLE_BITS);
    num_written = snprintf(buffer, buffer_size, "%d", min);
    if (min != max) {
      snprintf(buffer + num_written, buffer_size - num_written, "|%d", max);
    }
    str_parms_add_str(result, AUDIO_PARAMETER_STREAM_SUP_FORMATS, buffer);
  } // AUDIO_PARAMETER_STREAM_SUP_FORMATS

  result_str = str_parms_to_str(result);

  // done with these...
  str_parms_destroy(query);
  str_parms_destroy(result);

  return result_str;
}

static int in_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int in_set_gain(struct audio_stream_in *stream, float gain) {
    return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void* buffer, size_t bytes) {
    struct stream_in * in = (struct stream_in *)stream;

    int err = pcm_read(in->pcm, buffer, bytes);

    return err == 0 ? bytes : 0;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream) {
    return 0;
}

static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *hal_config,
                                  struct audio_stream_in **stream_in)
{
    ALOGV("usb:audio_hw::in adev_open_input_stream() rate:%d, chanMask:0x%X, fmt:%d",
          hal_config->sample_rate,
          hal_config->channel_mask,
          hal_config->format);

    struct stream_in *in = (struct stream_in *)calloc(1, sizeof(struct stream_in));
    if (in == NULL)
        return -ENOMEM;

    // setup function pointers
    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;

    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;

    struct audio_device *adev = (struct audio_device *)dev;
    in->dev = adev;

    in->standby = true;
    in->requested_rate = hal_config->sample_rate;
    in->alsa_pcm_config = default_alsa_in_config;

    if (hal_config->sample_rate != 0)
        in->alsa_pcm_config.rate = hal_config->sample_rate;

    //TODO(pmclean) is this correct, or do we need to map from ALSA format?
    // hal_config->format is an audio_format_t
    // logical
    // hal_config->format = default_alsa_in_config.format;
    //TODO(pmclean) use audio_format_from_pcm_format() (in hardware/audio_alsaops.h)
    switch (default_alsa_in_config.format) {
    case PCM_FORMAT_S32_LE:
        hal_config->format = AUDIO_FORMAT_PCM_32_BIT;
        break;

    case PCM_FORMAT_S8:
        hal_config->format = AUDIO_FORMAT_PCM_8_BIT;
        break;

    case PCM_FORMAT_S24_LE:
        hal_config->format = AUDIO_FORMAT_PCM_8_24_BIT;
        break;

    case PCM_FORMAT_S24_3LE:
        hal_config->format = AUDIO_FORMAT_PCM_8_24_BIT;
        break;

    default:
    case PCM_FORMAT_S16_LE:
        hal_config->format = AUDIO_FORMAT_PCM_16_BIT;
        break;
    }

    *stream_in = &in->stream;

    return 0;
}

static void adev_close_input_stream(struct audio_hw_device *dev,
                                   struct audio_stream_in *stream)
{
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    return 0;
}

static int adev_close(hw_device_t *device)
{
    ALOGV("usb:audio_hw::adev_close()");

    struct audio_device *adev = (struct audio_device *)device;
    free(device);

    output_hardware_config_is_cached = false;

    return 0;
}

static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    // ALOGV("usb:audio_hw::adev_open(%s)", name);

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    struct audio_device *adev = calloc(1, sizeof(struct audio_device));
    if (!adev)
        return -ENOMEM;

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->hw_device.common.module = (struct hw_module_t *) module;
    adev->hw_device.common.close = adev_close;

    adev->hw_device.init_check = adev_init_check;
    adev->hw_device.set_voice_volume = adev_set_voice_volume;
    adev->hw_device.set_master_volume = adev_set_master_volume;
    adev->hw_device.set_mode = adev_set_mode;
    adev->hw_device.set_mic_mute = adev_set_mic_mute;
    adev->hw_device.get_mic_mute = adev_get_mic_mute;
    adev->hw_device.set_parameters = adev_set_parameters;
    adev->hw_device.get_parameters = adev_get_parameters;
    adev->hw_device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->hw_device.open_output_stream = adev_open_output_stream;
    adev->hw_device.close_output_stream = adev_close_output_stream;
    adev->hw_device.open_input_stream = adev_open_input_stream;
    adev->hw_device.close_input_stream = adev_close_input_stream;
    adev->hw_device.dump = adev_dump;

    *device = &adev->hw_device.common;

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "USB audio HW HAL",
        .author = "The Android Open Source Project",
        .methods = &hal_module_methods,
    },
};
