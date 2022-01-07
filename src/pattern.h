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
#ifndef AML_AVSYNC_PATTERN_H__
#define AML_AVSYNC_PATTERN_H__

enum frame_pattern {
    AV_SYNC_FRAME_P32 = 0,
    AV_SYNC_FRAME_P22 = 1,
    AV_SYNC_FRAME_P41 = 2,
    AV_SYNC_FRAME_P11 = 3,
    AV_SYNC_FRAME_PMAX,
};

void* create_pattern_detector();
void destroy_pattern_detector(void *handle);
void reset_pattern(void *handle);
bool detect_pattern(void* handle, enum frame_pattern pattern, int cur_peroid, int last_peroid);
void correct_pattern(void* handle, struct vframe *frame, struct vframe *nextframe,
        int cur_peroid, int last_peroid, pts90K systime, pts90K vsync_interval, bool *expire);
int get_pattern(void* handle);
#endif
