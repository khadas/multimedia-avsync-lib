/*
 * Copyright (c) 2020 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 * User space AV sync module.
 *
 * Author: song.zhao@amlogic.com
 */

#ifndef MSYNC_UTIL_H
#define MSYNC_UTIL_H

#include <stdbool.h>
#include <stdint.h>
#include "aml_avsync.h"
#include "msync.h"

int msync_create_session();
void msync_destory_session(int id);

int msync_session_set_mode(int fd, enum sync_mode mode);
int msync_session_get_mode(int fd, enum sync_mode *mode);
int msync_session_get_start_policy(int fd, uint32_t *policy);
int msync_session_set_start_policy(int fd, uint32_t policy);
int msync_session_set_pause(int fd, bool pause);
int msync_session_set_video_start(int fd, pts90K pts);
int msync_session_get_wall(int fd, uint32_t *wall, uint32_t *interval);
int msync_session_set_video_start(int fd, pts90K pts);
int msync_session_set_audio_start(int fd, pts90K pts, pts90K delay, uint32_t *mode);
int msync_session_set_video_dis(int fd, pts90K pts);
int msync_session_set_audio_dis(int fd, pts90K pts);
int msync_session_set_rate(int fd, float speed);
int msync_session_get_rate(int fd, float *speed);
int msync_session_set_name(int fd, const char* name);
int msync_session_update_vpts(int fd, uint32_t system, uint32_t pts, uint32_t delay);
int msync_session_update_apts(int fd, uint32_t system, uint32_t pts, uint32_t delay);
int msync_session_set_audio_stop(int fd);
int msync_session_set_video_stop(int fd);
int msync_session_get_stat (int fd, enum sync_mode *mode,
        bool *v_active, bool *a_active, bool *v_timeout,
        bool *a_switch, enum src_flag flag);
bool msync_clock_started(int fd);
int msync_session_set_pcr(int fd, pts90K pts, uint64_t mono_clock);
int msync_session_get_pcr(int fd, pts90K *pts, uint64_t *mono_clock);
int msync_session_get_debug_mode(int fd, struct session_debug *debug);
int msync_session_set_audio_switch(int fd, bool start);
int msync_session_get_clock_dev(int fd, int32_t *ppm);
int msync_session_set_clock_dev(int fd, int32_t ppm);
int msync_session_get_disc_thres(int session_id, uint32_t *min, uint32_t *max);
#endif
