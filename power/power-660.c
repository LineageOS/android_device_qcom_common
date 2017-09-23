/*
 * Copyright (c) 2017, The Linux Foundation. All rights reserved.
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

#define MIN_VAL(X,Y) ((X>Y)?(Y):(X))

static int saved_interactive_mode = -1;
static int display_hint_sent;
static int video_encode_hint_sent;
static int cam_preview_hint_sent;

pthread_mutex_t camera_hint_mutex = PTHREAD_MUTEX_INITIALIZER;
static int camera_hint_ref_count;
static void process_video_encode_hint(void *metadata);
//static void process_cam_preview_hint(void *metadata);

static int current_power_profile = PROFILE_BALANCED;

extern void interaction(int duration, int num_args, int opt_list[]);

int get_number_of_profiles() {
    return 5;
}

/**
 * If target is SDM630:
 *     return 1
 * else:
 *     return 0
 */
static bool is_target_SDM630()
{
    static int is_target_SDM630 = false;
    int soc_id;

    soc_id = get_soc_id();
    if (soc_id == 318 || soc_id == 327)
        is_target_SDM630 = true;
    else
        is_target_SDM630 = false;

    return is_target_SDM630;
}

static int profile_high_performance[] = {
    SCHED_BOOST_ON_V3_RESID, 1,
    SCHED_IDLE_NR_RUN_RESID, 1,
    SCHED_IDLE_RESTRICT_CLUSTER_RESID, 0,
    SCHED_FREQ_AGGR_THRH_RESID, 1,
    SCHED_GROUP_DOWN_MIGRATE, 0x5F,
    SCHED_GROUP_UP_MIGRATE, 0x64,
    MIN_FREQ_BIG_CORE_0, 0xFFF,
    MIN_FREQ_LITTLE_CORE_0, 0xFFF,
};

static int profile_power_save[] = {
    MAX_FREQ_BIG_CORE_0, 0,
    MAX_FREQ_LITTLE_CORE_0, 0,
};

static int profile_bias_power[] = {
    MAX_FREQ_BIG_CORE_0_RESID, 3,
    MAX_FREQ_LITTLE_CORE_0_RESID, 1,
};

static int profile_bias_performance[] = {
    MIN_FREQ_BIG_CORE_0_RESID, 3,
    MIN_FREQ_LITTLE_CORE_0_RESID, 2,
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
    SCHED_BOOST_ON_V3_RESID, 1,
    MIN_FREQ_BIG_CORE_0_RESID, 3,
    MIN_FREQ_LITTLE_CORE_0_RESID, 2,
    SCHED_IDLE_NR_RUN_RESID, 1,
    SCHED_IDLE_RESTRICT_CLUSTER_RESID, 0,
};

static int resources_cpu_boost[] = {
    SCHED_BOOST_ON_V3_RESID, 2,
    MIN_FREQ_BIG_CORE_0_RESID, 1,
};

static int resources_interaction_fling_boost[] = {
    SCHED_BOOST_ON_V3_RESID, 2,
    MIN_FREQ_BIG_CORE_0_RESID, 1,
    MIN_FREQ_LITTLE_CORE_0_RESID, 0,
    CPUBW_HWMON_MIN_FREQ_RESID, 1,
    ABOVE_HISPEED_DELAY_BIG_RESID, 1,
    IO_IS_BUSY_BIG, 1,
    SCHED_FREQ_AGGR_THRH_RESID, 1,
    GPU_MIN_FREQ_RESID, 1,
};

static int resources_interaction_boost[] = {
    MIN_FREQ_BIG_CORE_0_RESID, 1,
    MIN_FREQ_LITTLE_CORE_0_RESID, 0,
};

int power_hint_override(struct power_module *module, power_hint_t hint,
        void *data)
{
    static struct timespec s_previous_boost_timespec;
    struct timespec cur_boost_timespec;
    static int s_previous_duration = 0;
    long long elapsed_time;
    int duration;

    if (hint == POWER_HINT_SET_PROFILE) {
        set_power_profile(*(int32_t *)data);
        return HINT_HANDLED;
    }

    /* Skip other hints in power save mode */
    if (current_power_profile == PROFILE_POWER_SAVE)
        return HINT_HANDLED;

    switch(hint) {
        case POWER_HINT_INTERACTION:
        {
            duration = 1500; // 1.5s by default
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
            return HINT_HANDLED;
        }
        case POWER_HINT_LAUNCH:
        {
            duration = 2000;

            interaction(duration, ARRAY_SIZE(resources_launch),
                    resources_launch);
            return HINT_HANDLED;
        }
        case POWER_HINT_CPU_BOOST:
        {
            duration = *(int32_t *)data / 1000;
            if (duration > 0) {
                interaction(duration, ARRAY_SIZE(resources_cpu_boost),
                        resources_cpu_boost);
                return HINT_HANDLED;
            }
            break;
        }
        case POWER_HINT_VSYNC:
            break;
        case POWER_HINT_VIDEO_ENCODE:
        {
            process_video_encode_hint(data);
            return HINT_HANDLED;
        }
    }
    return HINT_NONE;
}

int  set_interactive_override(struct power_module *module, int on)
{
    char governor[80];
    char tmp_str[NODE_MAX];
    int resource_values[20];
    int num_resources;
    struct video_encode_metadata_t video_encode_metadata;
    int rc;

    ALOGI("Got set_interactive hint");

    if (get_scaling_governor_check_cores(governor, sizeof(governor),CPU0) == -1) {
        if (get_scaling_governor_check_cores(governor, sizeof(governor),CPU1) == -1) {
            if (get_scaling_governor_check_cores(governor, sizeof(governor),CPU2) == -1) {
                if (get_scaling_governor_check_cores(governor, sizeof(governor),CPU3) == -1) {
                    ALOGE("Can't obtain scaling governor.");
                    return HINT_HANDLED;
                }
            }
        }
    }

    if (!on) {
        /* Display off. */
             if ((strncmp(governor, INTERACTIVE_GOVERNOR, strlen(INTERACTIVE_GOVERNOR)) == 0) &&
                (strlen(governor) == strlen(INTERACTIVE_GOVERNOR))) {
             /*
                 1. CPUfreq params
                        - hispeed freq for big - 1113Mhz
                        - go hispeed load for big - 95
                        - above_hispeed_delay for big - 40ms
                2. BusDCVS V2 params
                        - Sample_ms of 10ms
            */
            if(is_target_SDM630()){
                int res[] = { HISPEED_FREQ_BIG, 0x459,
                              GO_HISPEED_LOAD_BIG, 0x5F,
                              ABOVE_HISPEED_DELAY_BIG, 0x4,
                              CPUBW_HWMON_SAMPLE_MS, 0xA };
                memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
                num_resources = sizeof(res)/sizeof(res[0]);
            }
             /*
                 1. CPUfreq params
                        - hispeed freq for little - 902Mhz
                        - go hispeed load for little - 95
                        - above_hispeed_delay for little - 40ms
                 2. BusDCVS V2 params
                        - Sample_ms of 10ms
                 3. Sched group upmigrate - 500
            */
            else{
                int res[] =  { HISPEED_FREQ_LITTLE, 0x386,
                               GO_HISPEED_LOAD_LITTLE, 0x5F,
                               ABOVE_HISPEED_DELAY_LITTLE, 0x4,
                               CPUBW_HWMON_SAMPLE_MS, 0xA,
                               SCHED_GROUP_UP_MIGRATE, 0x1F4};
                memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
                num_resources = sizeof(res)/sizeof(res[0]);

            }
               if (!display_hint_sent) {
                   perform_hint_action(DISPLAY_STATE_HINT_ID,
                   resource_values, num_resources);
                  display_hint_sent = 1;
                }
             }

    } else {
        /* Display on. */
          if ((strncmp(governor, INTERACTIVE_GOVERNOR, strlen(INTERACTIVE_GOVERNOR)) == 0) &&
                (strlen(governor) == strlen(INTERACTIVE_GOVERNOR))) {

             undo_hint_action(DISPLAY_STATE_HINT_ID);
             display_hint_sent = 0;
          }
   }
    saved_interactive_mode = !!on;
    return HINT_HANDLED;
}


/* Video Encode Hint */
static void process_video_encode_hint(void *metadata)
{
    char governor[80];
    int resource_values[20];
    int num_resources;
    struct video_encode_metadata_t video_encode_metadata;

    ALOGI("Got process_video_encode_hint");

    if (get_scaling_governor_check_cores(governor,
        sizeof(governor),CPU0) == -1) {
            if (get_scaling_governor_check_cores(governor,
                sizeof(governor),CPU1) == -1) {
                    if (get_scaling_governor_check_cores(governor,
                        sizeof(governor),CPU2) == -1) {
                            if (get_scaling_governor_check_cores(governor,
                                sizeof(governor),CPU3) == -1) {
                                    ALOGE("Can't obtain scaling governor.");
                                    // return HINT_HANDLED;
                            }
                    }
            }
    }

    /* Initialize encode metadata struct fields. */
    memset(&video_encode_metadata, 0, sizeof(struct video_encode_metadata_t));
    video_encode_metadata.state = -1;
    video_encode_metadata.hint_id = DEFAULT_VIDEO_ENCODE_HINT_ID;

    if (metadata) {
        if (parse_video_encode_metadata((char *)metadata,
            &video_encode_metadata) == -1) {
            ALOGE("Error occurred while parsing metadata.");
            return;
        }
    } else {
        return;
    }

    if (video_encode_metadata.state == 1) {
        if ((strncmp(governor, INTERACTIVE_GOVERNOR,
            strlen(INTERACTIVE_GOVERNOR)) == 0) &&
            (strlen(governor) == strlen(INTERACTIVE_GOVERNOR))) {
             /*
                 1. CPUfreq params
                        - hispeed freq for big - 1113Mhz
                        - go hispeed load for big - 95
                        - above_hispeed_delay for big - 40ms
                        - target loads - 95
                        - nr_run - 5
                 2. BusDCVS V2 params
                        - Sample_ms of 10ms
            */
            if(is_target_SDM630()){
                int res[] = { HISPEED_FREQ_BIG, 0x459,
                              GO_HISPEED_LOAD_BIG, 0x5F,
                              ABOVE_HISPEED_DELAY_BIG, 0x4,
                              TARGET_LOADS_BIG, 0x5F,
                              SCHED_IDLE_NR_RUN, 0X5,
                              CPUBW_HWMON_SAMPLE_MS, 0xA};
                memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
                num_resources = sizeof(res)/sizeof(res[0]);

            }
            /*
                 1. CPUfreq params
                        - hispeed freq for little - 902Mhz
                        - go hispeed load for little - 95
                        - above_hispeed_delay for little - 40ms
                 2. BusDCVS V2 params
                        - Sample_ms of 10ms
            */
            else{
                int res[] = { HISPEED_FREQ_BIG, 0x386,
                              GO_HISPEED_LOAD_LITTLE, 0x5F,
                              ABOVE_HISPEED_DELAY_LITTLE, 0x4,
                              CPUBW_HWMON_SAMPLE_MS, 0xA};
                memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
                num_resources = sizeof(res)/sizeof(res[0]);
            }
            pthread_mutex_lock(&camera_hint_mutex);
            camera_hint_ref_count++;
            if (camera_hint_ref_count == 1) {
                if (!video_encode_hint_sent) {
                    perform_hint_action(video_encode_metadata.hint_id,
                    resource_values, num_resources);
                    video_encode_hint_sent = 1;
                }
           }
           pthread_mutex_unlock(&camera_hint_mutex);
        }
    } else if (video_encode_metadata.state == 0) {
        if ((strncmp(governor, INTERACTIVE_GOVERNOR,
            strlen(INTERACTIVE_GOVERNOR)) == 0) &&
            (strlen(governor) == strlen(INTERACTIVE_GOVERNOR))) {
            pthread_mutex_lock(&camera_hint_mutex);
            camera_hint_ref_count--;
            if (!camera_hint_ref_count) {
                undo_hint_action(video_encode_metadata.hint_id);
                video_encode_hint_sent = 0;
            }
            pthread_mutex_unlock(&camera_hint_mutex);
            return ;
        }
    }
    return;
}


