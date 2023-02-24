/*
 * Copyright (C) 2021 Amlogic Corporation.
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
void correct_pattern(void* handle, pts90K fpts, pts90K npts,
        int cur_peroid, int last_peroid, pts90K systime,
        pts90K vsync_interval, bool *expire);
int get_pattern(void* handle);
#endif
