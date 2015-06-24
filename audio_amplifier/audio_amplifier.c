

/*
 * Copyright (C) 2015 The CyanogenMod Open Source Project
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

#define LOG_TAG "audio_amplifier"
//#define LOG_NDEBUG 0

#include <stdint.h>
#include <sys/types.h>

#include <cutils/log.h>

#include <hardware/audio_amplifier.h>
#include <hardware/hardware.h>

#include <system/audio.h>
#include <tinyalsa/asoundlib.h>
#include <tinycompress/tinycompress.h>
#include <msm8974/platform.h>
#include <audio_hw.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/ioctl.h>

#include <linux/ioctl.h>
#define __force
#define __bitwise
#define __user
#include <sound/asound.h>

typedef struct tfa9887_amplifier {
    amplifier_device_t amp;
    int mixer_fd;
    unsigned int quat_mi2s_clk_id;
    bool calibrating;
    bool writing;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t watch_thread;
} tfa9887_amplifier_t;

enum tfa9887_Audio_Mode
{
    Audio_Mode_Music_Normal = 0,
    Audio_Mode_Voice_NB,
    Audio_Mode_Voice_NB_EXTRA,
    Audio_Mode_Voice_WB,
    Audio_Mode_Voice_WB_EXTRA,
    Audio_Mode_VT_NB,
    Audio_Mode_VT_WB,
    Audio_Mode_Voice_VOIP,
    Audio_Mode_Voice_VoLTE,
    Audio_Mode_Voice_VoLTE_EXTRA,
    Audio_Mode_VT_VoLTE,
};

extern void tfa9887_init();
extern int tfa9887_speakeron(int mode, int first);
extern int tfa9887_speakeroff();
extern int tfa9887_calibration();

#define QUAT_MI2S_CLK_CTL "QUAT_MI2S Clock"

static int quat_mi2s_interface_en(bool enable)
{
    enum mixer_ctl_type type;
    struct mixer_ctl *ctl;
    struct mixer *mixer = mixer_open(0);

    if (mixer == NULL) {
        ALOGE("Error opening mixer 0");
        return -1;
    }

    ctl = mixer_get_ctl_by_name(mixer, "QUAT_MI2S_RX Audio Mixer MultiMedia1");
    if (ctl == NULL) {
        mixer_close(mixer);
        ALOGE("Could not find QUAT_MI2S_RX Audio Mixer MultiMedia1");
        return -1;
    }

    type = mixer_ctl_get_type(ctl);
    if (type != MIXER_CTL_TYPE_BOOL) {
        ALOGE("QUAT_MI2S_RX Audio Mixer MultiMedia1 is not supported");
        mixer_close(mixer);
        return -1;
    }

    mixer_ctl_set_value(ctl, 0, enable);
    mixer_close(mixer);
    return 0;
}

void *write_dummy_data(void *param)
{
    char *buffer;
    int size;
    struct pcm *pcm;
    struct pcm_config config;
    tfa9887_amplifier_t *tfa9887 = (tfa9887_amplifier_t *) param;

    config.channels = 2;
    config.rate = 48000;
    config.period_size = 256;
    config.period_count = 2;
    config.format = PCM_FORMAT_S16_LE;
    config.start_threshold = config.period_size * config.period_count - 1;
    config.stop_threshold = config.period_size * config.period_count;
    config.silence_threshold = 0;
    config.avail_min = 1;

    if (quat_mi2s_interface_en(true)) {
        ALOGE("Failed to enable QUAT_MI2S_RX Audio Mixer MultiMedia1");
        return NULL;
    }

    pcm = pcm_open(0, 0, PCM_OUT | PCM_MONOTONIC, &config);
    if (!pcm || !pcm_is_ready(pcm)) {
        ALOGE("pcm_open failed: %s", pcm_get_error(pcm));
        if (pcm) {
            goto err_close_pcm;
        }
        goto err_disable_quat;
    }

    size = DEEP_BUFFER_OUTPUT_PERIOD_SIZE * 8;
    buffer = calloc(size, 1);
    if (!buffer) {
        ALOGE("failed to allocate buffer");
        goto err_close_pcm;
    }

    do {
        if (pcm_write(pcm, buffer, size)) {
            ALOGE("pcm_write failed");
        }
        pthread_mutex_lock(&tfa9887->mutex);
        tfa9887->writing = true;
        pthread_cond_signal(&tfa9887->cond);
        pthread_mutex_unlock(&tfa9887->mutex);
    } while (tfa9887->calibrating);

err_free:
    free(buffer);
err_close_pcm:
    pcm_close(pcm);
err_disable_quat:
    quat_mi2s_interface_en(false);
    ALOGV("--%s:%d", __func__, __LINE__);
    return NULL;
}

static int amp_dev_close(hw_device_t *device)
{
    tfa9887_amplifier_t *tfa9887 = (tfa9887_amplifier_t *) device;
    if (tfa9887) {
        if (tfa9887->mixer_fd >= 0)
            close(tfa9887->mixer_fd);
        pthread_join(tfa9887->watch_thread, NULL);
        pthread_cond_destroy(&tfa9887->cond);
        pthread_mutex_destroy(&tfa9887->mutex);
        free(tfa9887);
    }

    return 0;
}

static int amp_calibrate(tfa9887_amplifier_t *tfa9887)
{
    pthread_t write_thread;
    tfa9887->calibrating = true;
    pthread_create(&write_thread, NULL, write_dummy_data, tfa9887);
    pthread_mutex_lock(&tfa9887->mutex);
    while(!tfa9887->writing) {
        pthread_cond_wait(&tfa9887->cond, &tfa9887->mutex);
    }
    pthread_mutex_unlock(&tfa9887->mutex);
    tfa9887_calibration();
    tfa9887->calibrating = false;
    pthread_join(write_thread, NULL);
    return 0;
}

static void *amp_watch(void *param)
{
    struct snd_ctl_event event;
    tfa9887_amplifier_t *tfa9887 = (tfa9887_amplifier_t *) param;

    while(read(tfa9887->mixer_fd, &event, sizeof(struct snd_ctl_event)) > 0) {
        if (event.data.elem.id.numid == tfa9887->quat_mi2s_clk_id) {
            struct snd_ctl_elem_value ev;
            ev.id.numid = tfa9887->quat_mi2s_clk_id;
            if (ioctl(tfa9887->mixer_fd, SNDRV_CTL_IOCTL_ELEM_READ, &ev) < 0)
                continue;
            ALOGI("Got %s event = %d!", QUAT_MI2S_CLK_CTL, ev.value.enumerated.item[0]);
            if (ev.value.enumerated.item[0]) {
                tfa9887_speakeron(Audio_Mode_Music_Normal, 0);
            } else {
                tfa9887_speakeroff();
            }
        }
    }
    return NULL;
}

static int amp_init(tfa9887_amplifier_t *tfa9887)
{
    size_t i;
    int subscribe = 1;
    struct snd_ctl_elem_list elist;
    struct snd_ctl_elem_id *eid = NULL;
    tfa9887->mixer_fd = open("/dev/snd/controlC0", O_RDWR);
    if (tfa9887->mixer_fd < 0) {
        ALOGE("failed to open");
        goto fail;
    }

    memset(&elist, 0, sizeof(elist));
    if (ioctl(tfa9887->mixer_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &elist) < 0) {
        ALOGE("failed to get alsa control list");
        goto fail;
    }

    eid = calloc(elist.count, sizeof(struct snd_ctl_elem_id));
    if (!eid) {
        ALOGE("failed to allocate snd_ctl_elem_id");
        goto fail;
    }

    elist.space = elist.count;
    elist.pids = eid;

    if (ioctl(tfa9887->mixer_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &elist) < 0) {
        ALOGE("failed to get alsa control list");
        goto fail;
    }

    for (i = 0; i < elist.count; i++) {
        struct snd_ctl_elem_info ei;
        ei.id.numid = eid[i].numid;
        if (ioctl(tfa9887->mixer_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &ei) < 0) {
            ALOGE("failed to get alsa control %d info", eid[i].numid);
            goto fail;
        }

        if (!strcmp(QUAT_MI2S_CLK_CTL, (const char *)ei.id.name)) {
            ALOGI("Found %s! %d", QUAT_MI2S_CLK_CTL, ei.id.numid);
            tfa9887->quat_mi2s_clk_id = ei.id.numid;
            break;
        }
    }

    if (i == elist.count) {
        ALOGE("could not find %s", QUAT_MI2S_CLK_CTL);
        goto fail;
    }

    if (ioctl(tfa9887->mixer_fd, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribe) < 0) {
        ALOGE("failed to subscribe to %s events", QUAT_MI2S_CLK_CTL);
        goto fail;
    }

    pthread_create(&tfa9887->watch_thread, NULL, amp_watch, tfa9887);

    return 0;
fail:
    if (eid)
        free(eid);
    if (tfa9887->mixer_fd >= 0)
        close(tfa9887->mixer_fd);
    return -ENODEV;
}

static int amp_module_open(const hw_module_t *module, const char *name,
        hw_device_t **device)
{
    if (strcmp(name, AMPLIFIER_HARDWARE_INTERFACE)) {
        ALOGE("%s:%d: %s does not match amplifier hardware interface name\n",
                __func__, __LINE__, name);
        return -ENODEV;
    }

    tfa9887_amplifier_t *tfa9887 = calloc(1, sizeof(tfa9887_amplifier_t));
    if (!tfa9887) {
        ALOGE("%s:%d: Unable to allocate memory for amplifier device\n",
                __func__, __LINE__);
        return -ENOMEM;
    }

    tfa9887->amp.common.tag = HARDWARE_DEVICE_TAG;
    tfa9887->amp.common.module = (hw_module_t *) module;
    tfa9887->amp.common.version = AMPLIFIER_DEVICE_API_VERSION_2_0;
    tfa9887->amp.common.close = amp_dev_close;

    pthread_mutex_init(&tfa9887->mutex, NULL);
    pthread_cond_init(&tfa9887->cond, NULL);

    amp_calibrate(tfa9887);
    amp_init(tfa9887);

    *device = (hw_device_t *) tfa9887;

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = amp_module_open,
};

amplifier_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AMPLIFIER_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AMPLIFIER_HARDWARE_MODULE_ID,
        .name = "Sambar audio amplifier HAL",
        .author = "The CyanogenMod Open Source Project",
        .methods = &hal_module_methods,
    },
};
