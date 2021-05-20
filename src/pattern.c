/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description: frame pattern API ported from kernel video.c
 * Author: song.zhao@amlogic.com
 */
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "aml_avsync.h"
#include "pattern.h"
#include "aml_avsync_log.h"

#define PATTERN_32_D_RANGE 10
#define PATTERN_22_D_RANGE 10
#define PATTERN_41_D_RANGE 2
#define PATTERN_11_D_RANGE 10
#define PATTERN_32_DURATION 3750
#define PATTERN_22_DURATION 3000
#define PATTERN_11_DURATION 1500

struct pattern_detector {
    int match_cnt[AV_SYNC_FRAME_PMAX];
    int enter_cnt[AV_SYNC_FRAME_PMAX];
    int exit_cnt[AV_SYNC_FRAME_PMAX];

    int pattern_41[4];
    int pattern_41_index;
    int detected;

    /* reset lock */
    pthread_mutex_t lock;
};

void* create_pattern_detector()
{
    struct pattern_detector *pd;

    pd = (struct pattern_detector *)calloc(1, sizeof(*pd));
    if (!pd) {
        log_error("OOM");
        return NULL;
    }
    pthread_mutex_init(&pd->lock, NULL);
    pd->detected = -1;
    return pd;
}

void destroy_pattern_detector(void *handle)
{
    struct pattern_detector *pd = (struct pattern_detector *)handle;

    if (pd) {
        pthread_mutex_destroy(&pd->lock);
        free(pd);
    }
}

void reset_pattern(void *handle)
{
    struct pattern_detector *pd = (struct pattern_detector *)handle;

    if (!pd)
        return;

    pthread_mutex_lock(&pd->lock);
    pd->detected = -1;
    memset(pd->match_cnt, 0, sizeof(pd->match_cnt));
    pthread_mutex_unlock(&pd->lock);
}

void correct_pattern(void* handle, struct vframe *frame, struct vframe *nextframe,
        int cur_peroid, int last_peroid,
        pts90K systime, pts90K vsync_interval, bool *expire)
{
    struct pattern_detector *pd = (struct pattern_detector *)handle;
    int pattern_range, expected_cur_peroid;
    int expected_prev_interval;
    int npts = 0;

    /* Dont do anything if we have invalid data */
    if (!pd || !frame || !frame->pts)
        return;

    if (nextframe)
        npts = nextframe->pts;

    pthread_mutex_lock(&pd->lock);
    switch (pd->detected) {
        case AV_SYNC_FRAME_P32:
            pattern_range = PATTERN_32_D_RANGE;
            switch (last_peroid) {
                case 3:
                    expected_prev_interval = 3;
                    expected_cur_peroid = 2;
                    break;
                case 2:
                    expected_prev_interval = 2;
                    expected_cur_peroid = 3;
                    break;
                default:
                    goto exit;
            }
            if (!npts)
                npts = frame->pts + PATTERN_32_DURATION;
            break;
        case AV_SYNC_FRAME_P22:
            if (last_peroid != 2)
                goto exit;
            pattern_range =  PATTERN_22_D_RANGE;
            expected_prev_interval = 2;
            expected_cur_peroid = 2;
            if (!npts)
                npts = frame->pts + PATTERN_22_DURATION;
            break;
        case AV_SYNC_FRAME_P41:
            /* TODO */
        case AV_SYNC_FRAME_P11:
            if (last_peroid != 1)
                goto exit;
            pattern_range =  PATTERN_11_D_RANGE;
            expected_prev_interval = 1;
            expected_cur_peroid = 1;
            if (!npts)
                npts = frame->pts + PATTERN_11_DURATION;
            break;
        default:
            goto exit;
    }

    /* We do nothing if  we dont have enough data*/
    if (pd->match_cnt[pd->detected] != pattern_range)
        goto exit;

    if (*expire) {
        if (cur_peroid < expected_cur_peroid) {
            /* 2323232323..2233..2323, prev=2, curr=3,*/
            /* check if next frame will toggle after 3 vsyncs */
            /* 22222...22222 -> 222..2213(2)22...22 */
            /* check if next frame will toggle after 3 vsyncs */
            /* shall only allow one extra interval space to play around */
            if (((int)(systime + (expected_prev_interval + 1) *
                        vsync_interval - npts) >= 0) &&
                ((int)(systime + (expected_prev_interval + 2) *
                        vsync_interval - npts) < 0)) {
                *expire = false;
                log_debug("hold frame for pattern: %d", pd->detected);
            }

#if 0 // Frame scattering is the right place to adjust the hold time.
            /* here need to escape a vsync */
            if (systime > (frame->pts + vsync_interval)) {
                *expire = true;
                pts_escape_vsync = 1;
                log_info("escape a vsync pattern: %d", pd->detected);
            }
#endif
        }
    } else {
        if (cur_peroid == expected_cur_peroid) {
            /* 23232323..233223...2323 curr=2, prev=3 */
            /* check if this frame will expire next vsyncs and */
            /* next frame will expire after 3 vsyncs */
            /* 22222...22222 -> 222..223122...22 */
            /* check if this frame will expire next vsyncs and */
            /* next frame will expire after 2 vsyncs */

            if (((int)(systime + vsync_interval - frame->pts) >= 0) &&
                    ((int)(systime + vsync_interval * (expected_prev_interval - 1) - npts) < 0) &&
                    ((int)(systime + expected_prev_interval * vsync_interval - npts) >= 0)) {
                *expire = true;
                log_debug("squeeze frame for pattern: %d", pd->detected);
            }
        }
    }
exit:
    pthread_mutex_unlock(&pd->lock);
}

bool detect_pattern(void* handle, enum frame_pattern pattern, int cur_peroid, int last_peroid)
{
    struct pattern_detector *pd = (struct pattern_detector *)handle;
    int factor1 = 0, factor2 = 0, range = 0;
    bool ret = false;

    if (!pd || pattern >= AV_SYNC_FRAME_PMAX)
        return ret;

    pthread_mutex_lock(&pd->lock);
    if (pattern == AV_SYNC_FRAME_P32) {
        factor1 = 3;
        factor2 = 2;
        range =  PATTERN_32_D_RANGE;
    } else if (pattern == AV_SYNC_FRAME_P22) {
        factor1 = 2;
        factor2 = 2;
        range =  PATTERN_22_D_RANGE;
    } else if (pattern == AV_SYNC_FRAME_P41) {
        /* update 2111 mode detection */
        if (cur_peroid == 2) {
            if (pd->pattern_41[1] == 1 && pd->pattern_41[2] == 1 && pd->pattern_41[3] == 1 &&
                (pd->match_cnt[pattern] < PATTERN_41_D_RANGE)) {
                pd->match_cnt[pattern]++;
                if (pd->match_cnt[pattern] == PATTERN_41_D_RANGE) {
                    pd->enter_cnt[pattern]++;
                    pd->detected = pattern;
                    log_info("video 4:1 mode detected");
                }
            }
            pd->pattern_41[0] = 2;
            pd->pattern_41_index = 1;
        } else if (cur_peroid == 1) {
            if ((pd->pattern_41_index < 4) &&
                    (pd->pattern_41_index > 0)) {
                pd->pattern_41[pd->pattern_41_index] = 1;
                pd->pattern_41_index++;
            } else if (pd->match_cnt[pattern] == PATTERN_41_D_RANGE) {
                pd->match_cnt[pattern] = 0;
                pd->pattern_41_index = 0;
                pd->exit_cnt[pattern]++;
                memset(&pd->pattern_41[0], 0, sizeof(pd->pattern_41));
                log_info("video 4:1 mode broken");
            } else {
                pd->match_cnt[pattern] = 0;
                pd->pattern_41_index = 0;
                memset(&pd->pattern_41[0], 0, sizeof(pd->pattern_41));
            }
        } else if (pd->match_cnt[pattern] == PATTERN_41_D_RANGE) {
            pd->match_cnt[pattern] = 0;
            pd->pattern_41_index = 0;
            memset(&pd->pattern_41[0], 0, sizeof(pd->pattern_41));
            pd->exit_cnt[pattern]++;
            log_info("video 4:1 mode broken");
        } else {
            pd->match_cnt[pattern] = 0;
            pd->pattern_41_index = 0;
            memset(&pd->pattern_41[0], 0, sizeof(pd->pattern_41));
        }
        goto exit;
    } else if (pattern == AV_SYNC_FRAME_P11) {
        factor1 = 1;
        factor2 = 1;
        range =  PATTERN_11_D_RANGE;
    }

    /* update 1:1 3:2 or 2:2 mode detection */
    if (((last_peroid == factor1) && (cur_peroid == factor2)) ||
            ((last_peroid == factor2) && (cur_peroid == factor1))) {
        if (pd->match_cnt[pattern] < range) {
            pd->match_cnt[pattern]++;
            if (pd->match_cnt[pattern] == range) {
                pd->enter_cnt[pattern]++;
                pd->detected = pattern;
                log_info("video %d:%d mode detected cnt %d", factor1, factor2,
                         pd->enter_cnt[pattern]);
            }
        }
    } else if (pd->match_cnt[pattern] == range) {
        pd->match_cnt[pattern] = 0;
        pd->exit_cnt[pattern]++;
        log_info("video %d:%d mode broken by %d:%d cnt %d", factor1, factor2,
                 last_peroid, cur_peroid, pd->exit_cnt[pattern]);
        ret = true;
    } else
        pd->match_cnt[pattern] = 0;

exit:
    pthread_mutex_unlock(&pd->lock);
    return ret;
}
