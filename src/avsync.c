/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>

#include "aml_avsync.h"
#include "queue.h"
#include "pattern.h"
#include "tsync.h"
#include "aml_avsync_log.h"

enum sync_state {
    AV_SYNC_STAT_INIT = 0,
    AV_SYNC_STAT_RUNNING = 1,
    AV_SYNC_STAT_SYNC_SETUP = 2,
    AV_SYNC_STAT_SYNC_LOST = 3,
};

struct  av_sync_session {
    /* session id attached */
    int session_id;
    /* playback time, will stop increasing during pause */
    pts90K stream_time;
    pts90K vpts;

    /* phase adjustment of stream time for rate control */
    pts90K phase;
    bool phase_set;

    /* pts of last rendered frame */
    pts90K last_pts;
    struct vframe *last_frame;

    bool  first_frame_toggled;
    /* Whether in pause state */
    bool  paused;
    enum sync_mode	mode;
    enum sync_state	state;
    void *pattern_detector;
    void *frame_q;
    /* start threshold */
    int start_thres;

    /* display property */
    int delay;
    pts90K vsync_interval;

    /* state  lock */
    pthread_mutex_t lock;
    /* pattern */
    int last_holding_peroid;
    bool tsync_started;

    float speed;

    /*pip sync, remove after multi instance is supported*/
    struct timeval base_sys_time;
    struct timeval pause_start;
    uint64_t pause_duration;
    pts90K first_pts;

    /* pause pts */
    pts90K pause_pts;
    pause_pts_done pause_pts_cb;
    void *pause_cb_priv;

    /* log control */
    uint32_t last_systime;
    uint32_t sync_lost_cnt;
    struct timeval sync_lost_print_time;
};

#define MAX_FRAME_NUM 32
#define DEFAULT_START_THRESHOLD 2
#define TIME_UNIT90K    (90000)
#define AV_DISCONTINUE_THREDHOLD_MIN (TIME_UNIT90K * 3)
#define SYNC_LOST_PRINT_THRESHOLD 10000000 //10 seconds In micro seconds

static uint64_t time_diff (struct timeval *b, struct timeval *a);
static bool frame_expire(struct av_sync_session* avsync,
        uint32_t systime,
        struct vframe * frame,
        struct vframe * next_frame,
        int toggle_cnt);
static bool frame_expire_pip(struct av_sync_session* avsync,
        struct vframe * frame);
static void pattern_detect(struct av_sync_session* avsync,
        int cur_period,
        int last_period);

void* av_sync_create(int session_id,
        enum sync_mode mode,
        int start_thres,
        int delay, pts90K vsync_interval)
{
    struct av_sync_session *avsync = NULL;

    if (start_thres > 5) {
        log_error("start_thres too big: %d", start_thres);
        return NULL;
    }
    if (delay != 1 && delay != 2) {
        log_error("invalid delay: %d\n", delay);
        return NULL;
    }
    if (vsync_interval < 750 || vsync_interval > 3750) {
        log_error("invalid vsync interval: %d", vsync_interval);
        return NULL;
    }
    if (session_id != 0 && session_id != 1) {
        log_error("invalid session: %d", session_id);
        return NULL;
    }

    avsync = (struct av_sync_session *)calloc(1, sizeof(*avsync));
    if (!avsync) {
        log_error("OOM");
        return NULL;
    }
    avsync->pattern_detector = create_pattern_detector();
    if (!avsync->pattern_detector) {
        log_error("pd create fail");
        free(avsync);
        return NULL;
    }
    avsync->state = AV_SYNC_STAT_INIT;
    avsync->first_frame_toggled = false;
    avsync->paused = false;
    avsync->phase_set = false;
    avsync->session_id = session_id;
    avsync->mode = mode;
    avsync->last_frame = NULL;
    avsync->tsync_started = false;
    avsync->speed = 1.0f;
    avsync->pause_pts = AV_SYNC_INVALID_PAUSE_PTS;

    if (!start_thres)
        avsync->start_thres = DEFAULT_START_THRESHOLD;
    else
        avsync->start_thres = start_thres;
    avsync->delay = delay;
    avsync->vsync_interval = vsync_interval;

    avsync->frame_q = create_q(MAX_FRAME_NUM);
    if (!avsync->frame_q) {
        log_error("create queue fail");
        destroy_pattern_detector(avsync->pattern_detector);
        free(avsync);
        return NULL;
    }
    //TODO: connect kernel session

    if (avsync->session_id != 1) {
        /* just in case sysnode is wrongly set */
        tsync_send_video_pause(avsync->session_id, false);
    } else
        avsync->first_pts = -1;

    pthread_mutex_init(&avsync->lock, NULL);
    log_info("mode: %d start_thres: %d delay: %d interval: %d session: %d done\n",
            mode, start_thres, delay, vsync_interval, session_id);
    return avsync;
}

static int internal_stop(struct av_sync_session *avsync)
{
    int ret = 0;
    struct vframe *frame;

    pthread_mutex_lock(&avsync->lock);
    while (!dqueue_item(avsync->frame_q, (void **)&frame)) {
        frame->free(frame);
    }
    avsync->state = AV_SYNC_STAT_INIT;
    pthread_mutex_unlock(&avsync->lock);
    return ret;
}

/* destroy and detach from kernel session */
void av_sync_destroy(void *sync)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync)
        return;

    log_info("begin");
    internal_stop(avsync);

    /* all frames are freed */
    if (avsync->session_id != 1) {
        tsync_set_pts_inc_mode(avsync->session_id, false);
        tsync_send_video_stop(avsync->session_id);
    }

    pthread_mutex_destroy(&avsync->lock);
    destroy_q(avsync->frame_q);
    destroy_pattern_detector(avsync->pattern_detector);
    free(avsync);
    log_info("done");
}

int av_sync_pause(void *sync, bool pause)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync)
        return -1;

    if (avsync->session_id == 1) {
        if (!avsync->paused && pause) {
            gettimeofday(&avsync->pause_start, NULL);
            avsync->paused = true;
        }
        if (avsync->paused && !pause) {
            struct timeval now;

            gettimeofday(&now, NULL);
            avsync->pause_duration += time_diff(&now, &avsync->pause_start);
            avsync->paused = false;
        }
        return 0;
    }

    if (avsync->mode == AV_SYNC_MODE_VMASTER) {
        tsync_send_video_pause(avsync->session_id, pause);
    }
    avsync->paused = pause;
    log_info("paused:%d\n", pause);

    return 0;
}

int av_sync_push_frame(void *sync , struct vframe *frame)
{
    int ret;
    struct vframe *prev;
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync)
        return -1;

    if (!peek_item(avsync->frame_q, (void **)&prev, 0)) {
        if (prev->pts == frame->pts) {
            dqueue_item(avsync->frame_q, (void **)&prev);
            prev->free(prev);
            log_info ("drop frame with same pts %u", frame->pts);
        }
    }

    frame->hold_period = 0;
    ret = queue_item(avsync->frame_q, frame);
    if (avsync->state == AV_SYNC_STAT_INIT &&
        queue_size(avsync->frame_q) >= avsync->start_thres) {
        avsync->state = AV_SYNC_STAT_RUNNING;
        log_info("state: init --> running");
    }

    if (ret)
        log_error("%s queue fail:%d", ret);
    return ret;

}

struct vframe *av_sync_pop_frame(void *sync)
{
    struct vframe *frame = NULL;
    struct av_sync_session *avsync = (struct av_sync_session *)sync;
    int toggle_cnt = 0;
    uint32_t systime;
    bool pause_pts_reached = false;

    pthread_mutex_lock(&avsync->lock);
    if (avsync->state == AV_SYNC_STAT_INIT) {
        log_trace("in state INIT");
        goto exit;
    }

    if (avsync->session_id == 1) {
        if (peek_item(avsync->frame_q, (void **)&frame, 0) || !frame) {
            log_info("empty q");
            goto exit;
        }

        while (!peek_item(avsync->frame_q, (void **)&frame, 0)) {
            if (frame_expire_pip(avsync, frame)) {
                toggle_cnt++;

                dqueue_item(avsync->frame_q, (void **)&frame);
                if (avsync->last_frame) {
                    /* free frame that are not for display */
                    if (toggle_cnt > 1)
                        avsync->last_frame->free(avsync->last_frame);
                } else {
                    avsync->first_frame_toggled = true;
                    log_info("first frame %u", frame->pts);
                }
                avsync->last_frame = frame;
            } else
                break;
        }
        goto exit;
    }

    if (!avsync->tsync_started) {
        if (peek_item(avsync->frame_q, (void **)&frame, 0) || !frame) {
            log_info("empty q");
            goto exit;
        }

        if (tsync_enable(avsync->session_id, true))
            log_error("enable tsync fail");
        if (avsync->mode == AV_SYNC_MODE_VMASTER) {
            if (tsync_set_mode(avsync->session_id, AV_SYNC_MODE_VMASTER))
                log_error("set vmaster mode fail");
            if (tsync_set_pcr(avsync->session_id, frame->pts))
                log_error("set pcr fail");
            log_info("update pcr to: %u", frame->pts);
            if (tsync_set_pts_inc_mode(avsync->session_id, true))
                log_error("set inc mode fail");
        } else if (avsync->mode == AV_SYNC_MODE_AMASTER) {
            if (tsync_set_pts_inc_mode(avsync->session_id, false))
                log_error("set inc mode fail");
            if (tsync_set_mode(avsync->session_id, AV_SYNC_MODE_AMASTER))
                log_error("set amaster mode fail");
        } else {
            //PCR master mode should be set alreay, but it won't hurt to set again.
            if (tsync_set_mode(avsync->session_id, AV_SYNC_MODE_PCR_MASTER))
                log_error("set pcrmaster mode fail");
        }

        tsync_set_video_peek_mode(avsync->session_id);
        tsync_disable_video_stop_event(avsync->session_id, true);
        /* video start ASAP */
        tsync_set_video_sync_thres(avsync->session_id, false);
        /* video start event */
        if (tsync_send_video_start(avsync->session_id, frame->pts))
            log_error("send video start fail");
        else
            log_info("video start %u", frame->pts);
        avsync->tsync_started = true;
    }

    systime = tsync_get_pcr(avsync->session_id);
    while (!peek_item(avsync->frame_q, (void **)&frame, 0)) {
        struct vframe *next_frame = NULL;

        peek_item(avsync->frame_q, (void **)&next_frame, 1);
        if (next_frame)
            log_debug("cur_f %u next_f %u", frame->pts, next_frame->pts);
        if (frame_expire(avsync, systime, frame, next_frame, toggle_cnt)) {
            log_debug("cur_f %u expire", frame->pts);
            toggle_cnt++;

            pattern_detect(avsync,
                    (avsync->last_frame?avsync->last_frame->hold_period:0),
                    avsync->last_holding_peroid);
            if (avsync->last_frame)
                avsync->last_holding_peroid = avsync->last_frame->hold_period;

            dqueue_item(avsync->frame_q, (void **)&frame);
            if (avsync->last_frame) {
                /* free frame that are not for display */
                if (toggle_cnt > 1)
                    avsync->last_frame->free(avsync->last_frame);
            } else {
                avsync->first_frame_toggled = true;
                log_info("first frame %u", frame->pts);
            }
            avsync->last_frame = frame;
            avsync->last_pts = frame->pts;
        } else
            break;
    }

    /* pause pts */
    if (avsync->pause_pts != AV_SYNC_INVALID_PAUSE_PTS && avsync->last_frame) {
        if (avsync->pause_pts == AV_SYNC_STEP_PAUSE_PTS)
            pause_pts_reached = true;
        else
            pause_pts_reached = (int)(avsync->last_frame->pts - avsync->pause_pts) >= 0;
    } else if (avsync->pause_pts != AV_SYNC_INVALID_PAUSE_PTS) {
        if (!peek_item(avsync->frame_q, (void **)&frame, 0))
            pause_pts_reached = (int)(frame->pts - avsync->pause_pts) >= 0;
    }

    if (pause_pts_reached) {
        if (avsync->pause_pts_cb)
            avsync->pause_pts_cb(avsync->pause_pts,
                    avsync->pause_cb_priv);

        /* stay in paused until av_sync_pause(false) */
        avsync->paused = true;
        avsync->pause_pts = AV_SYNC_INVALID_PAUSE_PTS;
        log_info ("reach pause pts: %u", avsync->last_frame->pts);
    }

exit:
    pthread_mutex_unlock(&avsync->lock);
    if (avsync->last_frame) {
        log_debug("pop %u", avsync->last_frame->pts);
        tsync_set_vpts(avsync->session_id,avsync->last_frame->pts);
    } else
        log_debug("pop (nil)");
    if (avsync->last_frame)
        avsync->last_frame->hold_period++;
    return avsync->last_frame;
}

void av_sync_update_vsync_interval(void *sync, pts90K vsync_interval)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    pthread_mutex_lock(&avsync->lock);
    avsync->vsync_interval = vsync_interval;
    if (avsync->state >= AV_SYNC_STAT_RUNNING) {
        reset_pattern(avsync->pattern_detector);
        avsync->phase_set = false;
    }
    pthread_mutex_unlock(&avsync->lock);
}

static inline uint32_t abs_diff(uint32_t a, uint32_t b)
{
    return a > b ? a - b : b - a;
}

static uint64_t time_diff (struct timeval *b, struct timeval *a)
{
    return (b->tv_sec - a->tv_sec)*1000000 + (b->tv_usec - a->tv_usec);
}

static bool frame_expire_pip(struct av_sync_session* avsync,
        struct vframe * frame)
{
    struct timeval systime;
    uint64_t passed;
    pts90K passed_90k;

    if (avsync->paused && avsync->pause_pts == AV_SYNC_INVALID_PAUSE_PTS)
        return false;

    if (avsync->pause_pts == AV_SYNC_STEP_PAUSE_PTS)
        return true;

    gettimeofday(&systime, NULL);
    if (avsync->first_pts == -1) {
        avsync->first_pts = frame->pts;
        avsync->base_sys_time = systime;
        log_debug("first_pts %u, sys %d/%d", frame->pts,
            systime.tv_sec, systime.tv_usec);
        return true;
    }

    passed = time_diff(&systime, &avsync->base_sys_time);
    passed -= avsync->pause_duration;
    passed *= avsync->speed;
    passed_90k = (pts90K)(passed * 9 / 100);

    if (passed_90k > (frame->pts - avsync->first_pts)) {
        log_trace("cur_f %u sys: %u/%d stream: %u",
            frame->pts, systime.tv_sec, systime.tv_usec, passed_90k);
        return true;
    }

    return false;
}

static bool frame_expire(struct av_sync_session* avsync,
        uint32_t systime,
        struct vframe * frame,
        struct vframe * next_frame,
        int toggle_cnt)
{
    uint32_t fpts = frame->pts;
    bool expire = false;
    uint32_t pts_correction = avsync->delay * avsync->vsync_interval;

    if (avsync->paused && avsync->pause_pts == AV_SYNC_INVALID_PAUSE_PTS)
        return false;

    if (avsync->pause_pts == AV_SYNC_STEP_PAUSE_PTS)
        return true;

    if (!fpts) {
        if (avsync->last_frame) {
            /* try to accumulate duration as PTS */
            fpts = avsync->vpts + avsync->last_frame->duration;
        } else {
            fpts = avsync->vpts;
        }
    }
    systime += pts_correction;

    /* phase adjustment */
    if (avsync->phase_set)
        systime += avsync->phase;

    log_trace("systime:%u phase:%u correct:%u", systime,
            avsync->phase_set?avsync->phase:0, pts_correction);
    if (abs_diff(systime, fpts) > AV_DISCONTINUE_THREDHOLD_MIN &&
            avsync->first_frame_toggled) {
        /* ignore discontinity under pause */
        if (avsync->paused && avsync->mode != AV_SYNC_MODE_PCR_MASTER)
            return false;

        if (avsync->last_systime != systime || avsync->last_pts != fpts) {
            struct timeval now;

            gettimeofday(&now, NULL);
            avsync->last_systime = systime;
            avsync->last_pts = fpts;
            if (time_diff(&now, &avsync->sync_lost_print_time) >=
                SYNC_LOST_PRINT_THRESHOLD) {
                log_warn("sync lost systime:%x fpts:%x lost:%u",
                    systime, fpts, avsync->sync_lost_cnt);
                avsync->sync_lost_cnt = 0;
                gettimeofday(&avsync->sync_lost_print_time, NULL);
            } else
                avsync->sync_lost_cnt++;
        }
        avsync->state = AV_SYNC_STAT_SYNC_LOST;
        avsync->phase_set = false;
        reset_pattern(avsync->pattern_detector);
        if ((int)(systime - fpts) > 0) {
            if (frame->pts && avsync->mode == AV_SYNC_MODE_VMASTER)
                tsync_send_video_disc(avsync->session_id, frame->pts);
            /*catch up PCR */
            return true;
        } else if (avsync->mode == AV_SYNC_MODE_PCR_MASTER) {
            if (frame->pts)
                tsync_send_video_disc(avsync->session_id, frame->pts);
            else {
                tsync_send_video_disc(avsync->session_id, fpts);
                return true;
            }
        }
    }

    expire = (int)(systime - fpts) >= 0;

    /* scatter the frame in different vsync whenever possible */
    if (expire && next_frame && next_frame->pts && toggle_cnt) {
        /* multi frame expired in current vsync but no frame in next vsync */
        if (systime + avsync->vsync_interval < next_frame->pts) {
            expire = false;
            frame->hold_period++;
            log_debug("unset expire systime:%d inter:%d next_pts:%d toggle_cnt:%d",
                    systime, avsync->vsync_interval, next_frame->pts, toggle_cnt);
        }
    } else if (!expire && next_frame && next_frame->pts && !toggle_cnt
               && avsync->first_frame_toggled) {
        /* next vsync will have at least 2 frame expired */
        if (systime + avsync->vsync_interval > next_frame->pts) {
            expire = true;
            log_debug("set expire systime:%d inter:%d next_pts:%d",
                    systime, avsync->vsync_interval, next_frame->pts);
        }
    }

    if (avsync->state == AV_SYNC_STAT_SYNC_SETUP)
        correct_pattern(avsync->pattern_detector, frame, next_frame,
                (avsync->last_frame?avsync->last_frame->hold_period:0),
                avsync->last_holding_peroid, systime,
                avsync->vsync_interval, &expire);

    if (expire) {
        avsync->vpts = fpts;
        /* phase adjustment */
        if (!avsync->phase_set) {
            uint32_t phase_thres = avsync->vsync_interval / 4;
            //systime = tsync_get_pcr(avsync->session_id);
            if ( systime > fpts && (systime - fpts) < phase_thres) {
                /* too aligned to current VSYNC, separate them to 1/4 VSYNC */
                avsync->phase += phase_thres - (systime - fpts);
                avsync->phase_set = true;
                log_info("adjust phase to %d", avsync->phase);
            }
            if (!avsync->phase_set && systime > fpts &&
                systime < (fpts + avsync->vsync_interval) &&
                (systime - fpts) > avsync->vsync_interval - phase_thres) {
                /* too aligned to previous VSYNC, separate them to 1/4 VSYNC */
                avsync->phase += phase_thres + fpts + avsync->vsync_interval - systime;
                avsync->phase_set = true;
                log_info("adjust phase to %d", avsync->phase);
            }
        }

        if (avsync->state != AV_SYNC_STAT_SYNC_SETUP)
            log_info("sync setup");
        avsync->state = AV_SYNC_STAT_SYNC_SETUP;
        avsync->sync_lost_cnt = 0;
    }
    return expire;
}

static void pattern_detect(struct av_sync_session* avsync, int cur_period, int last_period)
{
    log_trace("cur_period: %d last_period: %d", cur_period, last_period);
    detect_pattern(avsync->pattern_detector, AV_SYNC_FRAME_P32, cur_period, last_period);
    detect_pattern(avsync->pattern_detector, AV_SYNC_FRAME_P22, cur_period, last_period);
    detect_pattern(avsync->pattern_detector, AV_SYNC_FRAME_P41, cur_period, last_period);
}

int av_sync_set_speed(void *sync, float speed)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (speed < 0.001f || speed > 100) {
        log_error("wrong speed %f [0.0001, 100]", speed);
        return -1;
    }

    avsync->speed = speed;
    if (avsync->session_id == 1)
        return 0;

    if (avsync->mode != AV_SYNC_MODE_VMASTER) {
        log_info("ignore set speed in mode %d", avsync->mode);
        return 0;
    }

    return tsync_set_speed(avsync->session_id, speed);
}

int av_sync_change_mode(void *sync, enum sync_mode mode)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync)
        return -1;

    if (avsync->session_id == 1)
        return 0;

    if (avsync->mode != AV_SYNC_MODE_VMASTER || mode != AV_SYNC_MODE_AMASTER) {
        log_error("only support V to A mode switch");
        return -1;
    }

    if (tsync_set_pts_inc_mode(avsync->session_id, false))
        log_error("set inc mode fail");
    if (tsync_set_mode(avsync->session_id, AV_SYNC_MODE_AMASTER))
        log_error("set amaster mode fail");
    avsync->mode = mode;
    log_info("update sync mode to %d", mode);
    return 0;
}

int av_sync_set_pause_pts(void *sync, pts90K pts)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync)
      return -1;

    avsync->pause_pts = pts;
    log_info("set pause pts: %u", pts);
    return 0;
}

int av_sync_set_pause_pts_cb(void *sync, pause_pts_done cb, void *priv)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync)
      return -1;

    avsync->pause_pts_cb = cb;
    avsync->pause_cb_priv = priv;
    return 0;
}
