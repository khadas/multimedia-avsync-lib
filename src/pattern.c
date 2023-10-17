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

/* pattern for 60Hz and under */
enum frame_pattern {
    AV_SYNC_FRAME_P32 = 0,
    AV_SYNC_FRAME_P22 = 1,
    AV_SYNC_FRAME_P41 = 2,
    AV_SYNC_FRAME_P11 = 3,
    AV_SYNC_FRAME_PMAX,
};

/* pattern for 100Hz+ */
enum frame_pattern_ex {
    AV_SYNC_FRAME_P55_EX = 0, /* 24/23.98@FPS@120 */
    AV_SYNC_FRAME_P44_EX = 1, /* 30/29.97FPS@120 25FPS@100 */
    AV_SYNC_FRAME_P32_EX = 2, /* 50FPS@120 */
    AV_SYNC_FRAME_P22_EX = 3, /* 60/59.94FSP@120 50FPS@100 */
    AV_SYNC_FRAME_P11_EX = 4, /* 120FPS@120 100FPS@100 */
    AV_SYNC_FRAME_PMAX_EX,
};

#define PATTERN_32_D_RANGE 10
#define PATTERN_22_D_RANGE 10
#define PATTERN_41_D_RANGE 2
#define PATTERN_44_D_RANGE 10
#define PATTERN_55_D_RANGE 10
#define PATTERN_11_D_RANGE 10
#define PATTERN_32_DURATION 3750
#define PATTERN_22_DURATION 3000
#define PATTERN_11_DURATION 1500

typedef void (* correct_pattern_func)(void* handle, pts90K fpts, pts90K npts,
        int cur_period, int last_period,
        pts90K systime, pts90K vsync_interval, bool *expire);

typedef bool (* detect_pattern_func)(void* handle, int cur_period, int last_period);
typedef void (* reset_pattern_func)(void *handle);

struct pdetector {
    void *priv;

    correct_pattern_func correct_pattern_f;
    detect_pattern_func detect_pattern_f;
    reset_pattern_func reset_pattern_f;
};

struct pattern_detector {
    int match_cnt[AV_SYNC_FRAME_PMAX];
    int enter_cnt[AV_SYNC_FRAME_PMAX];
    int exit_cnt[AV_SYNC_FRAME_PMAX];

    int pattern_41[4];
    int pattern_41_index;
    int detected;
};

struct pattern_detector_ex {
    int match_cnt[AV_SYNC_FRAME_PMAX_EX];
    int enter_cnt[AV_SYNC_FRAME_PMAX_EX];
    int exit_cnt[AV_SYNC_FRAME_PMAX_EX];
    int detected;
};

static void reset_pattern_basic(void *handle)
{
    struct pattern_detector *pd = (struct pattern_detector *)handle;
    if (!pd)
        return;

    pd->detected = -1;
    memset(pd->match_cnt, 0, sizeof(pd->match_cnt));
}

static void reset_pattern_ex(void *handle)
{
    struct pattern_detector_ex *pd = (struct pattern_detector_ex *)handle;
    if (!pd)
        return;

    pd->detected = -1;
    memset(pd->match_cnt, 0, sizeof(pd->match_cnt));
}

void reset_pattern(void *handle)
{
    struct pdetector *pd = (struct pdetector *)handle;

    if (!pd)
        return;

    pd->reset_pattern_f(pd->priv);
}

static void correct_pattern_basic(void* handle, pts90K fpts, pts90K npts,
        int cur_period, int last_period,
        pts90K systime, pts90K vsync_interval, bool *expire)
{
    struct pattern_detector *pd = (struct pattern_detector *)handle;
    int pattern_range, expected_cur_period, remain_period;
    int expected_prev_interval;

    /* Dont do anything if we have invalid data */
    if (!pd || fpts == -1 || !fpts)
        return;

    switch (pd->detected) {
        case AV_SYNC_FRAME_P32:
            pattern_range = PATTERN_32_D_RANGE;
            switch (last_period) {
                case 3:
                    expected_prev_interval = 3;
                    expected_cur_period = 2;
                    break;
                case 2:
                    expected_prev_interval = 2;
                    expected_cur_period = 3;
                    break;
                default:
                    goto exit;
            }
            if (!npts)
                npts = fpts + PATTERN_32_DURATION;
            break;
        case AV_SYNC_FRAME_P22:
            if (last_period != 2)
                goto exit;
            pattern_range =  PATTERN_22_D_RANGE;
            expected_prev_interval = 2;
            expected_cur_period = 2;
            if (!npts)
                npts = fpts + 2 * vsync_interval;
            break;
        case AV_SYNC_FRAME_P41:
            /* TODO */
        case AV_SYNC_FRAME_P11:
            if (last_period != 1)
                goto exit;
            pattern_range =  PATTERN_11_D_RANGE;
            expected_prev_interval = 1;
            expected_cur_period = 1;
            if (!npts)
                npts = fpts + vsync_interval;
            break;
        default:
            goto exit;
    }

    /* We do nothing if  we dont have enough data*/
    if (pd->match_cnt[pd->detected] != pattern_range)
        goto exit;

    if (*expire) {
        if (cur_period < expected_cur_period) {
            remain_period = expected_cur_period - cur_period;
            /* 2323232323..2233..2323, prev=2, curr=3,*/
            /* check if next frame will toggle after 3 vsyncs */
            /* 22222...22222 -> 222..2213(2)22...22 */
            /* check if next frame will toggle after 3 vsyncs */
            /* shall only allow one extra interval space to play around */
            if (systime - fpts <= 90) {
                *expire = false;
                log_debug("hold frame for pattern: %d sys: %u fpts: %u",
                        pd->detected, systime, fpts);
            } else if (((int)(systime + (remain_period + 1) *
                        vsync_interval - npts) <= 0) &&
                ((int)(systime + (remain_period + 2) *
                        vsync_interval - npts) > 0)) {
                *expire = false;
                log_debug("hold frame for pattern: %d", pd->detected);
            } else {
                log_debug("not hold frame for pattern: %d sys: %u fpts: %u s-f %u nfps: %u",
                    pd->detected, systime, fpts, systime - fpts, npts);
            }

#if 0 // Frame scattering is the right place to adjust the hold time.
            /* here need to escape a vsync */
            if (systime > (fpts + vsync_interval)) {
                *expire = true;
                pts_escape_vsync = 1;
                log_info("escape a vsync pattern: %d", pd->detected);
            }
#endif
        }
    } else {
        if (cur_period == expected_cur_period) {
            /* 23232323..233223...2323 curr=2, prev=3 */
            /* check if this frame will expire next vsyncs and */
            /* next frame will expire after 3 vsyncs */
            /* 22222...22222 -> 222..223122...22 */
            /* check if this frame will expire next vsyncs and */
            /* next frame will expire after 2 vsyncs */

            if (((int)(systime + vsync_interval - fpts) >= 0) &&
                    ((int)(systime + vsync_interval * (expected_prev_interval - 1) - npts) < 0) &&
                    ((int)(systime + expected_prev_interval * vsync_interval - npts) >= 0)) {
                *expire = true;
                log_debug("squeeze frame for pattern: %d", pd->detected);
            }
        }
    }
exit:
    return;
}

static void correct_pattern_ex(void* handle, pts90K fpts, pts90K npts,
        int cur_period, int last_period,
        pts90K systime, pts90K vsync_interval, bool *expire)
{
    struct pattern_detector_ex *pd = (struct pattern_detector_ex *)handle;
    int pattern_range, expected_cur_period, remain_period;
    int expected_prev_interval;

    /* Dont do anything if we have invalid data */
    if (!pd || fpts == -1 || !fpts)
        return;

    switch (pd->detected) {
        case AV_SYNC_FRAME_P32_EX:
            pattern_range = PATTERN_32_D_RANGE;
            switch (last_period) {
                case 3:
                    expected_prev_interval = 3;
                    expected_cur_period = 2;
                    break;
                case 2:
                    expected_prev_interval = 2;
                    expected_cur_period = 3;
                    break;
                default:
                    goto exit;
            }
            if (!npts)
                npts = fpts + PATTERN_32_DURATION;
            break;
        case AV_SYNC_FRAME_P22_EX:
            if (last_period != 2)
                goto exit;
            pattern_range =  PATTERN_22_D_RANGE;
            expected_prev_interval = 2;
            expected_cur_period = 2;
            if (!npts)
                npts = fpts + 2 * vsync_interval;
            break;
        case AV_SYNC_FRAME_P44_EX:
            if (last_period != 4)
                goto exit;
            pattern_range =  PATTERN_44_D_RANGE;
            expected_prev_interval = 4;
            expected_cur_period = 4;
            if (!npts)
                npts = fpts + 4 * vsync_interval;
            break;
        case AV_SYNC_FRAME_P55_EX:
            if (last_period != 5)
                goto exit;
            pattern_range =  PATTERN_55_D_RANGE;
            expected_prev_interval = 5;
            expected_cur_period = 5;
            if (!npts)
                npts = fpts + 5 * vsync_interval;
            break;
        default:
            goto exit;
    }

    /* We do nothing if  we dont have enough data*/
    if (pd->match_cnt[pd->detected] != pattern_range)
        goto exit;

    if (*expire) {
        if (cur_period < expected_cur_period) {
            remain_period = expected_cur_period - cur_period;
            /* 2323232323..2233..2323, prev=2, curr=3,*/
            /* check if next frame will toggle after 3 vsyncs */
            /* 22222...22222 -> 222..2213(2)22...22 */
            /* check if next frame will toggle after 3 vsyncs */
            /* shall only allow one extra interval space to play around */
            if (systime - fpts <= 90) {
                *expire = false;
                log_debug("hold frame for pattern: %d sys: %u fpts: %u",
                        pd->detected, systime, fpts);
            } else if (((int)(systime + (remain_period + 1) *
                        vsync_interval - npts) <= 0) &&
                ((int)(systime + (remain_period + 2) *
                        vsync_interval - npts) > 0)) {
                *expire = false;
                log_debug("hold frame for pattern: %d", pd->detected);
            } else {
                log_debug("not hold frame for pattern: %d sys: %u fpts: %u s-f %u nfps: %u",
                    pd->detected, systime, fpts, systime - fpts, npts);
            }
        }
    } else {
        if (cur_period == expected_cur_period) {
            /* 23232323..233223...2323 curr=2, prev=3 */
            /* check if this frame will expire next vsyncs and */
            /* next frame will expire after 3 vsyncs */
            /* 22222...22222 -> 222..223122...22 */
            /* check if this frame will expire next vsyncs and */
            /* next frame will expire after 2 vsyncs */

            if (((int)(systime + vsync_interval - fpts) >= 0) &&
                    ((int)(systime + vsync_interval * (expected_prev_interval - 1) - npts) < 0) &&
                    ((int)(systime + expected_prev_interval * vsync_interval - npts) >= 0)) {
                *expire = true;
                log_debug("squeeze frame for pattern: %d", pd->detected);
            }
        }
    }
exit:
    return;

}

void correct_pattern(void* handle, pts90K fpts, pts90K npts,
        int cur_period, int last_period,
        pts90K systime, pts90K vsync_interval, bool *expire)
{
    struct pdetector *pd = (struct pdetector *)handle;

    if (!pd)
        return;

    pd->correct_pattern_f(pd->priv, fpts, npts,
        cur_period, last_period, systime,
        vsync_interval, expire);
}

bool detect_pattern_basic(struct pattern_detector *pd,
    enum frame_pattern pattern, int cur_period, int last_period)
{
    int factor1 = 0, factor2 = 0, range = 0;
    bool ret = false;

    if (!pd || pattern >= AV_SYNC_FRAME_PMAX)
        return ret;

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
        if (cur_period == 2) {
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
        } else if (cur_period == 1) {
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
    if (((last_period == factor1) && (cur_period == factor2)) ||
            ((last_period == factor2) && (cur_period == factor1))) {
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
        pd->detected = -1;
        log_info("video %d:%d mode broken by %d:%d cnt %d", factor1, factor2,
                 last_period, cur_period, pd->exit_cnt[pattern]);
        ret = true;
    } else
        pd->match_cnt[pattern] = 0;

exit:
    return ret;
}

bool detect_pattern_basic_all(void* handle, int cur_period, int last_period)
{
    struct pattern_detector *pd = (struct pattern_detector *)handle;
    bool ret = false;

    if (!pd)
        return false;

    if (detect_pattern_basic(pd, AV_SYNC_FRAME_P32, cur_period, last_period))
        ret = true;
    if (detect_pattern_basic(pd, AV_SYNC_FRAME_P22, cur_period, last_period))
        ret = true;
    if (detect_pattern_basic(pd, AV_SYNC_FRAME_P41, cur_period, last_period))
        ret = true;
    if (detect_pattern_basic(pd, AV_SYNC_FRAME_P11, cur_period, last_period))
        ret = true;

    return ret;
}

bool detect_pattern_ex(struct pattern_detector_ex *pd,
    enum frame_pattern_ex pattern, int cur_period, int last_period)
{
    int factor1 = 0, factor2 = 0, range = 0;
    bool ret = false;

    if (!pd || pattern >= AV_SYNC_FRAME_PMAX_EX)
        return ret;

    if (pattern == AV_SYNC_FRAME_P11_EX) {
        factor1 = 1;
        factor2 = 1;
        range =  PATTERN_11_D_RANGE;
    } else if (pattern == AV_SYNC_FRAME_P22_EX) {
        factor1 = 2;
        factor2 = 2;
        range =  PATTERN_22_D_RANGE;
    } else if (pattern == AV_SYNC_FRAME_P44_EX) {
        factor1 = 4;
        factor2 = 4;
        range =  PATTERN_44_D_RANGE;
    } else if (pattern == AV_SYNC_FRAME_P55_EX) {
        factor1 = 5;
        factor2 = 5;
        range =  PATTERN_55_D_RANGE;
    } else if (pattern == AV_SYNC_FRAME_P32_EX) {
        factor1 = 3;
        factor2 = 2;
        range =  PATTERN_32_D_RANGE;
    }

    if (((last_period == factor1) && (cur_period == factor2)) ||
            ((last_period == factor2) && (cur_period == factor1))) {
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
        pd->detected = -1;
        log_info("video %d:%d mode broken by %d:%d cnt %d", factor1, factor2,
                 last_period, cur_period, pd->exit_cnt[pattern]);
        ret = true;
    } else
        pd->match_cnt[pattern] = 0;

    return ret;

}

bool detect_pattern_ex_all(void* handle, int cur_period, int last_period)
{
    struct pattern_detector_ex *pd = (struct pattern_detector_ex *)handle;
    bool ret = false;

    if (!pd)
        return false;

    if (detect_pattern_ex(pd, AV_SYNC_FRAME_P55_EX, cur_period, last_period))
        ret = true;
    if (detect_pattern_ex(pd, AV_SYNC_FRAME_P44_EX, cur_period, last_period))
        ret = true;
    if (detect_pattern_ex(pd, AV_SYNC_FRAME_P32_EX, cur_period, last_period))
        ret = true;
    if (detect_pattern_ex(pd, AV_SYNC_FRAME_P22_EX, cur_period, last_period))
        ret = true;
    if (detect_pattern_ex(pd, AV_SYNC_FRAME_P11_EX, cur_period, last_period))
        ret = true;

    return ret;
}


bool detect_pattern(void* handle, int cur_period, int last_period)
{
    struct pdetector *pd = (struct pdetector *)handle;

    if (!pd)
        return false;

    return pd->detect_pattern_f(pd->priv, cur_period, last_period);
}

int get_pattern(void* handle)
{
    struct pattern_detector *pd = (struct pattern_detector *)handle;
    return pd->detected;
}

void* create_pattern_detector(int vsync_interval)
{
    struct pdetector *p = (struct pdetector *)calloc(1, sizeof(*p));

    if (!p) {
        log_error("OOM");
        return NULL;
    }

    if (vsync_interval > 900) {
        struct pattern_detector *pd;
        pd = (struct pattern_detector *)calloc(1, sizeof(*pd));
        if (!pd) {
            log_error("OOM");
            goto error;
        }
        pd->detected = -1;
        p->priv = pd;
        p->detect_pattern_f = detect_pattern_basic_all;
        p->correct_pattern_f = correct_pattern_basic;
        p->reset_pattern_f = reset_pattern_basic;
    } else {
        struct pattern_detector_ex *pd;
        pd = (struct pattern_detector_ex *)calloc(1, sizeof(*pd));
        if (!pd) {
            log_error("OOM");
            goto error;
        }
        pd->detected = -1;
        p->priv = pd;
        p->detect_pattern_f = detect_pattern_ex_all;
        p->correct_pattern_f = correct_pattern_ex;
        p->reset_pattern_f = reset_pattern_ex;
    }
    return p;
error:
    if (p)
        free(p);
    return NULL;
}

void destroy_pattern_detector(void *handle)
{
    struct pdetector *p = (struct pdetector *)handle;

    if (p) {
        if (p->priv)
            free(p->priv);
        free(p);
    }
}
