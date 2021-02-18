/*
 * Copyright (c) 2020 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description: tsync sysnode wrapper
 * Author: song.zhao@amlogic.com
 */

#ifndef _AML_TSYNC_H_
#define _AML_TSYNC_H_

#include <stdbool.h>
#include <stdint.h>
#include "aml_avsync.h"

int tsync_enable(int session, bool enable);
uint32_t  tsync_get_pcr(int session);
int tsync_set_pcr(int session, uint32_t pcr);
//uint32_t  tsync_get_vpts(int session);
int tsync_send_video_start(int session, uint32_t vpts);
int tsync_send_video_stop(int session);
int tsync_send_video_pause(int session, bool pause);
int tsync_send_video_disc(int session, uint32_t vpts);
int tsync_set_pts_inc_mode(int session, bool enable);
int tsync_set_mode(int session, enum sync_mode mode);
int tsync_set_video_peek_mode(int session);
int tsync_set_video_sync_thres(int session, bool enable);
int tsync_disable_video_stop_event(int session, bool disable);
int tsync_set_speed(int session, float speed);
int tsync_set_vpts(int session, pts90K pts);

#endif
