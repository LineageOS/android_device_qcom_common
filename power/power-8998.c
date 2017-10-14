/*
 * Copyright (c) 2016, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * *    * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#define LOG_NIDEBUG 0

#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdlib.h>

#define LOG_TAG "QTI PowerHAL"
#include <utils/Log.h>
#include <hardware/hardware.h>
#include <hardware/power.h>

#include "utils.h"
#include "metadata-defs.h"
#include "hint-data.h"
#include "performance.h"
#include "power-common.h"
#include "powerhintparser.h"

static int sustained_mode_handle = 0;
static int vr_mode_handle = 0;
int sustained_performance_mode = 0;
int vr_mode = 0;
static pthread_mutex_t s_interaction_lock = PTHREAD_MUTEX_INITIALIZER;
int launch_handle = -1;
int launch_mode;
int cpu_boost_handle = -1;
int cpu_boost_mode;
#define CHECK_HANDLE(x) (((x)>0) && ((x)!=-1))

static int current_power_profile = PROFILE_BALANCED;

extern void interaction(int duration, int num_args, int opt_list[]);

int get_number_of_profiles() {
    return 5;
}

static int profile_high_performance[] = {
    SCHED_BOOST_ON_V3, 0x1,
    MIN_FREQ_BIG_CORE_0, 0xFFF,
    MIN_FREQ_LITTLE_CORE_0, 0xFFF,
    ALL_CPUS_PWR_CLPS_DIS_V3, 0x1,
};

static int profile_power_save[] = {
    MAX_FREQ_BIG_CORE_0, 0x3E8,
    MAX_FREQ_LITTLE_CORE_0, 0x3E8,
};

static int profile_bias_power[] = {
    MAX_FREQ_BIG_CORE_0, 0x514,
    MAX_FREQ_LITTLE_CORE_0, 0x3E8,
};

static int profile_bias_performance[] = {
    MIN_FREQ_BIG_CORE_0, 0x578,
};

static void set_power_profile(int profile) {

    if (profile == current_power_profile)
        return;

    ALOGV("%s: Profile=%d", __func__, profile);

    if (current_power_profile != PROFILE_BALANCED) {
        undo_hint_action(DEFAULT_PROFILE_HINT_ID);
        ALOGV("%s: Hint undone", __func__);
    }

    if (profile == PROFILE_POWER_SAVE) {
        perform_hint_action(DEFAULT_PROFILE_HINT_ID, profile_power_save,
            ARRAY_SIZE(profile_power_save));
        ALOGD("%s: Set powersave mode", __func__);

    } else if (profile == PROFILE_HIGH_PERFORMANCE) {
        perform_hint_action(DEFAULT_PROFILE_HINT_ID, profile_high_performance,
                ARRAY_SIZE(profile_high_performance));
        ALOGD("%s: Set performance mode", __func__);

    } else if (profile == PROFILE_BIAS_POWER) {
        perform_hint_action(DEFAULT_PROFILE_HINT_ID, profile_bias_power,
            ARRAY_SIZE(profile_bias_power));
        ALOGD("%s: Set bias power mode", __func__);

    } else if (profile == PROFILE_BIAS_PERFORMANCE) {
        perform_hint_action(DEFAULT_PROFILE_HINT_ID, profile_bias_performance,
                ARRAY_SIZE(profile_bias_performance));
        ALOGD("%s: Set bias perf mode", __func__);

    }

    current_power_profile = profile;
}

static int resources_launch[] = {
    SCHED_BOOST_ON_V3, 0x1,
    MAX_FREQ_BIG_CORE_0, 0x939,
    MAX_FREQ_LITTLE_CORE_0, 0xFFF,
    MIN_FREQ_BIG_CORE_0, 0xFFF,
    MIN_FREQ_LITTLE_CORE_0, 0xFFF,
    CPUBW_HWMON_MIN_FREQ, 0x8C,
    ALL_CPUS_PWR_CLPS_DIS_V3, 0x1,
    STOR_CLK_SCALE_DIS, 0x1,
};

static int resources_cpu_boost[] = {
    SCHED_BOOST_ON_V3, 0x1,
    MIN_FREQ_BIG_CORE_0, 0x3E8,
};

static int resources_interaction_fling_boost[] = {
    CPUBW_HWMON_MIN_FREQ, 0x33,
    MIN_FREQ_BIG_CORE_0, 0x3E8,
    MIN_FREQ_LITTLE_CORE_0, 0x3E8,
    SCHED_BOOST_ON_V3, 0x2,
};

static int resources_interaction_boost[] = {
    MIN_FREQ_BIG_CORE_0, 0x3E8,
};

static int process_interaction_hint(void *data)
{
    static struct timespec s_previous_boost_timespec;
    struct timespec cur_boost_timespec;
    static int s_previous_duration = 0;
    long long elapsed_time;
    int duration = 1500; // 1.5s by default

    pthread_mutex_lock(&s_interaction_lock);
    if (sustained_performance_mode || vr_mode) {
        pthread_mutex_unlock(&s_interaction_lock);
        return HINT_HANDLED;
    }

    if (data) {
        int input_duration = *((int*)data) + 750;
        if (input_duration > duration) {
            duration = (input_duration > 5750) ? 5750 : input_duration;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &cur_boost_timespec);

    elapsed_time = calc_timespan_us(s_previous_boost_timespec, cur_boost_timespec);
    // don't hint if previous hint's duration covers this hint's duration
    if ((s_previous_duration * 1000) > (elapsed_time + duration * 1000)) {
        pthread_mutex_unlock(&s_interaction_lock);
        return HINT_HANDLED;
    }
    s_previous_boost_timespec = cur_boost_timespec;
    s_previous_duration = duration;

    if (duration >= 1500) {
        interaction(duration, ARRAY_SIZE(resources_interaction_fling_boost),
                resources_interaction_fling_boost);
    } else {
        interaction(duration, ARRAY_SIZE(resources_interaction_boost),
                resources_interaction_boost);
    }
    pthread_mutex_unlock(&s_interaction_lock);
    return HINT_HANDLED;
}

static int process_launch_hint(void *data)
{
    int duration = 2000;

    if (sustained_performance_mode || vr_mode) {
        return HINT_HANDLED;
    }

    if (data && launch_mode == 0) {
        launch_handle = interaction_with_handle(
            launch_handle, duration, ARRAY_SIZE(resources_launch), resources_launch);
        if (CHECK_HANDLE(launch_handle)) {
            launch_mode = 1;
            ALOGI("Activity launch hint handled");
            return HINT_HANDLED;
        } else {
            return HINT_NONE;
        }
    } else if (data == NULL && launch_mode == 1) {
        release_request(launch_handle);
        launch_mode = 0;
        return HINT_HANDLED;
    }
    return HINT_NONE;
}

static int process_cpu_boost_hint(void *data)
{
    int duration = *(int32_t *)data / 1000;

    if (sustained_performance_mode || vr_mode) {
        return HINT_HANDLED;
    }

    if (duration > 0 && data && cpu_boost_mode == 0) {
        cpu_boost_handle = interaction_with_handle(
            cpu_boost_handle, duration, ARRAY_SIZE(resources_cpu_boost), resources_cpu_boost);
        if (CHECK_HANDLE(cpu_boost_handle)) {
            cpu_boost_mode = 1;
            ALOGI("CPU boost hint handled");
            return HINT_HANDLED;
        } else {
            return HINT_NONE;
        }
    } else if (data == NULL && cpu_boost_mode == 1) {
        release_request(cpu_boost_handle);
        cpu_boost_mode = 0;
        return HINT_HANDLED;
    }
    return HINT_NONE;
}

static int process_sustained_perf_hint(void *data)
{
    int duration = 0;
    int *resource_values = NULL;
    int resources = 0;

    pthread_mutex_lock(&s_interaction_lock);
    if (data && sustained_performance_mode == 0) {
        if (vr_mode == 0) { // Sustained mode only.
            resource_values = getPowerhint(SUSTAINED_PERF_HINT_ID, &resources);
            if (!resource_values) {
                ALOGE("Can't get sustained perf hints from xml ");
                pthread_mutex_unlock(&s_interaction_lock);
                return HINT_NONE;
            }
            // Ensure that POWER_HINT_LAUNCH is not in progress.
            if (launch_mode == 1) {
                release_request(launch_handle);
                launch_mode = 0;
            }
            // Ensure that POWER_HINT_CPU_BOOST is not in progress.
            if (cpu_boost_mode == 1) {
                release_request(cpu_boost_handle);
                cpu_boost_mode = 0;
            }
            sustained_mode_handle = interaction_with_handle(
                sustained_mode_handle, duration, resources, resource_values);
            if (!CHECK_HANDLE(sustained_mode_handle)) {
                ALOGE("Failed interaction_with_handle for sustained_mode_handle");
                pthread_mutex_unlock(&s_interaction_lock);
                return HINT_NONE;
            }
        } else if (vr_mode == 1) { // Sustained + VR mode.
            release_request(vr_mode_handle);
            resource_values = getPowerhint(VR_MODE_SUSTAINED_PERF_HINT_ID, &resources);
            if (!resource_values) {
                ALOGE("Can't get VR mode sustained perf hints from xml ");
                pthread_mutex_unlock(&s_interaction_lock);
                return HINT_NONE;
            }
            sustained_mode_handle = interaction_with_handle(
                sustained_mode_handle, duration, resources, resource_values);
            if (!CHECK_HANDLE(sustained_mode_handle)) {
                ALOGE("Failed interaction_with_handle for sustained_mode_handle");
                pthread_mutex_unlock(&s_interaction_lock);
                return HINT_NONE;
            }
        }
        sustained_performance_mode = 1;
    } else if (sustained_performance_mode == 1) {
        release_request(sustained_mode_handle);
        if (vr_mode == 1) { // Switch back to VR Mode.
            resource_values = getPowerhint(VR_MODE_HINT_ID, &resources);
            if (!resource_values) {
                ALOGE("Can't get VR mode perf hints from xml ");
                pthread_mutex_unlock(&s_interaction_lock);
                return HINT_NONE;
            }
            release_request(vr_mode_handle);
            vr_mode_handle = interaction_with_handle(
                vr_mode_handle, duration, resources, resource_values);
            if (!CHECK_HANDLE(vr_mode_handle)) {
                ALOGE("Failed interaction_with_handle for vr_mode_handle");
                pthread_mutex_unlock(&s_interaction_lock);
                return HINT_NONE;
            }
        }
        sustained_performance_mode = 0;
    }
    pthread_mutex_unlock(&s_interaction_lock);
    return HINT_HANDLED;
}

static int process_vr_mode_hint(void *data)
{
    int duration = 0;
    int *resource_values = NULL;
    int resources = 0;

    pthread_mutex_lock(&s_interaction_lock);
    if (data && vr_mode == 0) {
        if (sustained_performance_mode == 0) { // VR mode only.
            resource_values = getPowerhint(VR_MODE_HINT_ID, &resources);
            if (!resource_values) {
                ALOGE("Can't get VR mode perf hints from xml ");
                pthread_mutex_unlock(&s_interaction_lock);
                return HINT_NONE;
            }
            // Ensure that POWER_HINT_LAUNCH is not in progress.
            if (launch_mode == 1) {
                release_request(launch_handle);
                launch_mode = 0;
            }
            // Ensure that POWER_HINT_CPU_BOOST is not in progress.
            if (cpu_boost_mode == 1) {
                release_request(cpu_boost_handle);
                cpu_boost_mode = 0;
            }
            vr_mode_handle = interaction_with_handle(
                vr_mode_handle, duration, resources, resource_values);
            if (!CHECK_HANDLE(vr_mode_handle)) {
                ALOGE("Failed interaction_with_handle for vr_mode_handle");
                pthread_mutex_unlock(&s_interaction_lock);
                return HINT_NONE;
            }
        } else if (sustained_performance_mode == 1) { // Sustained + VR mode.
            release_request(sustained_mode_handle);
            resource_values = getPowerhint(VR_MODE_SUSTAINED_PERF_HINT_ID, &resources);
            if (!resource_values) {
                ALOGE("Can't get VR mode sustained perf hints from xml ");
                pthread_mutex_unlock(&s_interaction_lock);
                return HINT_NONE;
            }
            vr_mode_handle = interaction_with_handle(
                vr_mode_handle, duration, resources, resource_values);
            if (!CHECK_HANDLE(vr_mode_handle)) {
                ALOGE("Failed interaction_with_handle for vr_mode_handle");
                pthread_mutex_unlock(&s_interaction_lock);
                return HINT_NONE;
            }
        }
        vr_mode = 1;
    } else if (vr_mode == 1) {
        release_request(vr_mode_handle);
        if (sustained_performance_mode == 1) { // Switch back to sustained Mode.
            resource_values = getPowerhint(SUSTAINED_PERF_HINT_ID, &resources);
            if (!resource_values) {
                ALOGE("Can't get sustained perf hints from xml ");
                pthread_mutex_unlock(&s_interaction_lock);
                return HINT_NONE;
            }
            release_request(sustained_mode_handle);
            sustained_mode_handle = interaction_with_handle(
                sustained_mode_handle, duration, resources, resource_values);
            if (!CHECK_HANDLE(sustained_mode_handle)) {
                ALOGE("Failed interaction_with_handle for sustained_mode_handle");
                pthread_mutex_unlock(&s_interaction_lock);
                return HINT_NONE;
            }
        }
        vr_mode = 0;
    }
    pthread_mutex_unlock(&s_interaction_lock);

    return HINT_HANDLED;
}

static int process_video_encode_hint(void *metadata)
{
    char governor[80];
    struct video_encode_metadata_t video_encode_metadata;

    if(!metadata)
       return HINT_NONE;

    if (get_scaling_governor(governor, sizeof(governor)) == -1) {
        ALOGE("Can't obtain scaling governor.");

        return HINT_NONE;
    }

    /* Initialize encode metadata struct fields */
    memset(&video_encode_metadata, 0, sizeof(struct video_encode_metadata_t));
    video_encode_metadata.state = -1;
    video_encode_metadata.hint_id = DEFAULT_VIDEO_ENCODE_HINT_ID;

    if (parse_video_encode_metadata((char *)metadata, &video_encode_metadata) ==
            -1) {
       ALOGE("Error occurred while parsing metadata.");
       return HINT_NONE;
    }

    if (video_encode_metadata.state == 1) {
          if (is_interactive_governor(governor)) {

            int *resource_values;
            int resources;

            /* extract perflock resources */
            resource_values = getPowerhint(video_encode_metadata.hint_id, &resources);

            if (resource_values != NULL)
               perform_hint_action(video_encode_metadata.hint_id, resource_values, resources);
            ALOGI("Video Encode hint start");
            return HINT_HANDLED;
        }
    } else if (video_encode_metadata.state == 0) {
          if (is_interactive_governor(governor)) {
            undo_hint_action(video_encode_metadata.hint_id);
            ALOGI("Video Encode hint stop");
            return HINT_HANDLED;
        }
    }
    return HINT_NONE;
}

int power_hint_override(struct power_module *module, power_hint_t hint, void *data)
{
    int ret_val = HINT_NONE;
    switch(hint) {
        case POWER_HINT_LAUNCH:
            ret_val = process_launch_hint(data);
            break;
        case POWER_HINT_CPU_BOOST:
            ret_val = process_cpu_boost_hint(data);
            break;
        case POWER_HINT_VIDEO_ENCODE:
            ret_val = process_video_encode_hint(data);
            break;
        case POWER_HINT_SUSTAINED_PERFORMANCE:
            ret_val = process_sustained_perf_hint(data);
            break;
        case POWER_HINT_VR_MODE:
            ret_val = process_vr_mode_hint(data);
            break;
        case POWER_HINT_INTERACTION:
            ret_val = process_interaction_hint(data);
            break;
        default:
            break;
    }
    return ret_val;
}

int set_interactive_override(struct power_module *module, int on)
{
    return HINT_HANDLED; /* Don't excecute this code path, not in use */
}
