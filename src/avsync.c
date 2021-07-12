/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
//#include <linux/amlogic/msync.h>
#include "aml_avsync.h"
#include "queue.h"
#include "pattern.h"
#include "aml_avsync_log.h"
#include "msync_util.h"
#include "msync.h"
#include <pthread.h>
#include "pcr_monitor.h"
enum sync_state {
    AV_SYNC_STAT_INIT = 0,
    AV_SYNC_STAT_RUNNING = 1,
    AV_SYNC_STAT_SYNC_SETUP = 2,
    AV_SYNC_STAT_SYNC_LOST = 3,
};

enum audio_switch_state_ {
    AUDIO_SWITCH_STAT_INIT = 0,
    AUDIO_SWITCH_STAT_RESET = 1,
    AUDIO_SWITCH_STAT_START = 2,
    AUDIO_SWITCH_STAT_FINISH = 3,
};

#define SESSION_DEV "avsync_s"

struct  av_sync_session {
    /* session id attached */
    int session_id;
    int fd;
    bool attached;
    enum sync_mode mode;
    /* for audio trickplay */
    enum sync_mode backup_mode;
    enum sync_type type;
    uint32_t start_policy;
    int timeout;

    /* playback time, will stop increasing during pause */
    pts90K vpts;
    pts90K apts;

    /* phase adjustment of stream time for rate control (Video ONLY) */
    pts90K phase;
    bool phase_set;

    /* pts of last rendered frame */
    pts90K last_wall;
    pts90K last_pts;
    struct vframe *last_frame;

    bool  first_frame_toggled;
    bool  paused;
    enum sync_state state;
    void *pattern_detector;
    void *frame_q;

    /* start control */
    int start_thres;
    audio_start_cb audio_start;
    void *audio_start_priv;

    /* render property */
    int delay;
    pts90K vsync_interval;

    /* state  lock */
    pthread_mutex_t lock;
    /* pattern */
    int last_holding_peroid;
    bool session_started;

    float speed;

    /* pause pts */
    pts90K pause_pts;
    pause_pts_done pause_pts_cb;
    void *pause_cb_priv;

    /* log control */
    uint32_t last_log_syst;
    uint32_t sync_lost_cnt;
    struct timeval sync_lost_print_time;

    pthread_t poll_thread;
    /* pcr master, IPTV only */
    bool quit_poll;
    enum sync_mode active_mode;
    uint32_t disc_thres_min;
    uint32_t disc_thres_max;

    /* error detection */
    uint32_t last_poptime;
    uint32_t outlier_cnt;
    pts90K last_disc_pts;

    // indicate set audio switch
    bool in_audio_switch;
    enum audio_switch_state_ audio_switch_state;

    //pcr monitor handle
    void *pcr_monitor;
    int ppm;

    //video FPS detection
    pts90K last_fpts;
    int fps_interval;
    int fps_interval_acc;
    int fps_cnt;

    //video freerun with rate control
    uint32_t last_r_syst;

    //Audio dropping detection
    uint32_t audio_drop_cnt;
    struct timeval audio_drop_start;
};

#define MAX_FRAME_NUM 32
#define DEFAULT_START_THRESHOLD 2
#define TIME_UNIT90K    (90000)
#define AV_DISC_THRES_MIN (TIME_UNIT90K / 3)
#define AV_DISC_THRES_MAX (TIME_UNIT90K * 10)
#define A_ADJ_THREDHOLD_HB (900 * 6) //60ms
#define A_ADJ_THREDHOLD_LB (900 * 2) //20ms
#define AV_PATTERN_RESET_THRES (TIME_UNIT90K / 10)
#define SYNC_LOST_PRINT_THRESHOLD 10000000 //10 seconds In micro seconds
#define LIVE_MODE(mode) ((mode) == AV_SYNC_MODE_PCR_MASTER || (mode) == AV_SYNC_MODE_IPTV)

#define STREAM_DISC_THRES (TIME_UNIT90K / 10)
#define OUTLIER_MAX_CNT 8
#define VALID_TS(x) ((x) != -1)

static uint64_t time_diff (struct timeval *b, struct timeval *a);
static bool frame_expire(struct av_sync_session* avsync,
        uint32_t systime,
        uint32_t interval,
        struct vframe * frame,
        struct vframe * next_frame,
        int toggle_cnt);
static bool pattern_detect(struct av_sync_session* avsync,
        int cur_period,
        int last_period);
static void * poll_thread(void * arg);
static void trigger_audio_start_cb(struct av_sync_session *avsync,
        avs_ascb_reason reason);

int av_sync_open_session(int *session_id)
{
    int fd = msync_create_session();
    int id, rc;

    if (fd < 0) {
        log_error("fail");
        return -1;
    }
    rc = ioctl(fd, AMSYNC_IOC_ALLOC_SESSION, &id);
    if (rc) {
        log_error("new session errno:%d", errno);
        return rc;
    }
    *session_id = id;
    return fd;
}

void av_sync_close_session(int session)
{
    msync_destory_session(session);
}

static void* create_internal(int session_id,
        enum sync_mode mode,
        enum sync_type type,
        int start_thres,
        bool attach)
{
    struct av_sync_session *avsync = NULL;
    char dev_name[20];

    avsync = (struct av_sync_session *)calloc(1, sizeof(*avsync));
    if (!avsync) {
        log_error("OOM");
        return NULL;
    }

    if (type == AV_SYNC_TYPE_VIDEO) {
        avsync->pattern_detector = create_pattern_detector();
        if (!avsync->pattern_detector) {
            log_error("pd create fail");
            goto err;
        }

        if (!start_thres)
            avsync->start_thres = DEFAULT_START_THRESHOLD;
        else {
            if (start_thres > 5) {
                log_error("start_thres too big: %d", start_thres);
                goto err2;
            }
            avsync->start_thres = start_thres;
        }
        avsync->phase_set = false;
        avsync->first_frame_toggled = false;
    }

    avsync->type = type;
    avsync->state = AV_SYNC_STAT_INIT;
    avsync->paused = false;
    avsync->session_id = session_id;
    avsync->backup_mode = mode;
    avsync->last_frame = NULL;
    avsync->session_started = false;
    avsync->speed = 1.0f;
    avsync->pause_pts = AV_SYNC_INVALID_PAUSE_PTS;
    avsync->vsync_interval = -1;
    avsync->last_disc_pts = -1;
    avsync->last_log_syst = -1;
    avsync->last_pts = -1;
    avsync->last_wall = -1;
    avsync->fps_interval = -1;
    avsync->last_r_syst = -1;
    avsync->timeout = -1;

    if (msync_session_get_disc_thres(session_id,
                &avsync->disc_thres_min, &avsync->disc_thres_max)) {
        log_error("fail to get disc thres", dev_name, errno);
        avsync->disc_thres_min = AV_DISC_THRES_MIN;
        avsync->disc_thres_max = AV_DISC_THRES_MAX;
    }

    pthread_mutex_init(&avsync->lock, NULL);
    log_info("[%d] mode %d type %d start_thres %d disc_thres %u/%u",
        session_id, mode, type, start_thres,
        avsync->disc_thres_min, avsync->disc_thres_max);

    snprintf(dev_name, sizeof(dev_name), "/dev/%s%d", SESSION_DEV, session_id);
    avsync->fd = open(dev_name, O_RDONLY | O_CLOEXEC);
    if (avsync->fd < 0) {
        log_error("open %s errno %d", dev_name, errno);
        goto err2;
    }

    if (avsync->type == AV_SYNC_TYPE_PCR) {
        if (pcr_monitor_init(&avsync->pcr_monitor)) {
            log_error("pcr monitor init");
            goto err2;
        }
    }

    if (!attach) {
        msync_session_set_mode(avsync->fd, mode);
        avsync->mode = mode;
    } else {
        avsync->attached = true;
        if (msync_session_get_mode(avsync->fd, &avsync->mode)) {
            log_error("get mode");
            goto err3;
        }
        avsync->backup_mode = avsync->mode;
        if (msync_session_get_start_policy(avsync->fd, &avsync->start_policy, &avsync->timeout)) {
            log_error("get policy");
            goto err3;
        }
        if (msync_session_get_stat(avsync->fd, &avsync->active_mode,
                NULL, NULL, NULL, &avsync->in_audio_switch)) {
            log_error("get state");
            goto err3;
        }
        if (avsync->in_audio_switch) {
            log_info("audio_switch_state reseted the audio");
            avsync->audio_switch_state = AUDIO_SWITCH_STAT_RESET;
        }

        log_info("[%d]retrieve sync mode %d policy %d",
            session_id, avsync->mode, avsync->start_policy);
    }

    return avsync;
err3:
    if (avsync->pcr_monitor)
        pcr_monitor_destroy(avsync->pcr_monitor);
err2:
    destroy_pattern_detector(avsync->pattern_detector);
err:
    free(avsync);
    return NULL;
}

void* av_sync_create(int session_id,
        enum sync_mode mode,
        enum sync_type type,
        int start_thres)
{
    return create_internal(session_id, mode,
            type, start_thres, false);
}

void* av_sync_attach(int session_id, enum sync_type type)
{
    return create_internal(session_id, AV_SYNC_MODE_MAX,
            type, 0, true);
}

int av_sync_video_config(void *sync, struct video_config* config)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync || !config)
        return -1;

    if (config->delay != 1 && config->delay != 2) {
        log_error("invalid delay: %d\n", config->delay);
        return -1;
    }

    avsync->delay = config->delay;

    log_info("[%d] delay: %d",
            avsync->session_id, config->delay);
    return 0;
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

    log_info("[%d]begin", avsync->session_id);
    if (avsync->state != AV_SYNC_STAT_INIT) {
        if (avsync->type == AV_SYNC_TYPE_VIDEO)
            internal_stop(avsync);

        avsync->quit_poll = true;
        if (avsync->poll_thread) {
            pthread_join(avsync->poll_thread, NULL);
            avsync->poll_thread = 0;
        }
        trigger_audio_start_cb(avsync, AV_SYNC_ASCB_STOP);
    }

    if (avsync->session_started) {
        if (avsync->type == AV_SYNC_TYPE_VIDEO)
            msync_session_set_video_stop(avsync->fd);
        else
            msync_session_set_audio_stop(avsync->fd);
    }

    if(avsync->pcr_monitor)
        pcr_monitor_destroy(avsync->pcr_monitor);

    close(avsync->fd);
    pthread_mutex_destroy(&avsync->lock);
    if (avsync->type == AV_SYNC_TYPE_VIDEO) {
        destroy_q(avsync->frame_q);
        destroy_pattern_detector(avsync->pattern_detector);
    }
    log_info("[%d]done", avsync->session_id);
    free(avsync);
}

int avs_sync_set_start_policy(void *sync, struct start_policy* st_policy)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync || !avsync->fd)
        return -1;

    log_info("[%d]policy %u --> %u, timeout %d --> %d", avsync->start_policy, st_policy->policy, avsync->timeout, st_policy->timeout);
    avsync->start_policy = st_policy->policy;
    avsync->timeout = st_policy->timeout;
    /* v_peek will be handled by libamlavsync */
    if (st_policy->policy != AV_SYNC_START_NONE &&
        st_policy->policy != AV_SYNC_START_V_PEEK)
        return msync_session_set_start_policy(avsync->fd, st_policy->policy, st_policy->timeout);

    return 0;
}

int av_sync_pause(void *sync, bool pause)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;
    bool v_active, a_active, v_timeout;
    int rc;

    if (!avsync)
        return -1;

    if (avsync->mode == AV_SYNC_MODE_PCR_MASTER)
        return -1;

     rc = msync_session_get_stat(avsync->fd, &avsync->active_mode,
                &v_active, &a_active, &v_timeout,
                &avsync->in_audio_switch);

    /* ignore only when video try to pause when audio is acive, on which
       the control of the STC will be relays.
       When resume,it can do always as it is possible that video just
       paused earlier without audio yet,then audio added later before resume.
       We shall not igore that otherwise it could cause video freeze.       */
    if (avsync->mode == AV_SYNC_MODE_AMASTER &&
        avsync->type == AV_SYNC_TYPE_VIDEO &&
        a_active &&
        !avsync->in_audio_switch) {
        if (!pause) {
            log_info("[%d] clear video pause when audio active",
                     avsync->session_id);
            avsync->paused = pause;
        } else {
            log_info("[%d] ignore the pause from video when audio active",
                     avsync->session_id);
        }
        return 0;
    }

    if (avsync->in_audio_switch && avsync->type == AV_SYNC_TYPE_AUDIO) {
        log_info("[%d] ignore the pause from audio", avsync->session_id);
        avsync->audio_switch_state = AUDIO_SWITCH_STAT_RESET;
        return 0;
    }

    rc = msync_session_set_pause(avsync->fd, pause);
    avsync->paused = pause;
    log_info("[%d]paused:%d type:%d rc %d",
        avsync->session_id, pause, avsync->type, rc);

    return rc;
}

int av_sync_push_frame(void *sync , struct vframe *frame)
{
    int ret;
    struct vframe *prev;
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync)
        return -1;

    if (!avsync->frame_q) {
        /* policy should be final now */
        if (msync_session_get_start_policy(avsync->fd, &avsync->start_policy, &avsync->timeout)) {
            log_error("[%d]get policy", avsync->session_id);
            return -1;
        }

        avsync->frame_q = create_q(MAX_FRAME_NUM);
        if (!avsync->frame_q) {
            log_error("[%d]create queue fail", avsync->session_id);
            return -1;
        }

        if (avsync->mode == AV_SYNC_MODE_PCR_MASTER ||
            avsync->mode == AV_SYNC_MODE_IPTV) {
            int ret;

            ret = pthread_create(&avsync->poll_thread, NULL, poll_thread, avsync);
            if (ret) {
                log_error("[%d]create poll thread errno %d", avsync->session_id, errno);
                destroy_q(avsync->frame_q);
                return -1;
            }
        }
    }

    if (!peek_item(avsync->frame_q, (void **)&prev, 0)) {
        if (prev->pts == frame->pts && avsync->mode == AV_SYNC_MODE_AMASTER) {
            dqueue_item(avsync->frame_q, (void **)&prev);
            prev->free(prev);
            log_info ("[%d]drop frame with same pts %u", avsync->session_id, frame->pts);
        } else if (LIVE_MODE(avsync->mode) && avsync->fps_cnt < 100) {
            int32_t interval = frame->pts - prev->pts;

            if (interval > 0 && interval <= 4500) {
                if (avsync->fps_interval_acc == -1) {
                    avsync->fps_interval_acc = interval;
                    avsync->fps_cnt = 1;
                } else {
                    avsync->fps_interval_acc += interval;
                    avsync->fps_cnt++;
                    avsync->fps_interval = avsync->fps_interval_acc / avsync->fps_cnt;
                    if (avsync->fps_cnt == 100)
                        log_info("[%d] fps_interval = %d", avsync->session_id,  avsync->fps_interval);
                }
            }
        }
    }

    if (frame->duration == -1)
        frame->duration = 0;
    frame->hold_period = 0;
    ret = queue_item(avsync->frame_q, frame);
    if (avsync->state == AV_SYNC_STAT_INIT &&
        queue_size(avsync->frame_q) >= avsync->start_thres) {
        avsync->state = AV_SYNC_STAT_RUNNING;
        log_info("[%d]state: init --> running", avsync->session_id);
    }

    if (ret)
        log_error("%s queue fail:%d", ret);
    log_debug("[%d]push %u", avsync->session_id, frame->pts);
    return ret;

}

struct vframe *av_sync_pop_frame(void *sync)
{
    struct vframe *frame = NULL, *enter_last_frame = NULL;
    struct av_sync_session *avsync = (struct av_sync_session *)sync;
    int toggle_cnt = 0;
    uint32_t systime;
    bool pause_pts_reached = false;
    uint32_t interval;

    pthread_mutex_lock(&avsync->lock);
    if (avsync->state == AV_SYNC_STAT_INIT) {
        log_info("[%d]in state INIT", avsync->session_id);
        goto exit;
    }

    if (!avsync->session_started) {
        uint32_t pts;

        if (peek_item(avsync->frame_q, (void **)&frame, 0) || !frame) {
            log_info("[%d]empty q", avsync->session_id);
            goto exit;
        }
        msync_session_get_wall(avsync->fd, &systime, &interval);
        pts = frame->pts - avsync->delay * interval;
        msync_session_set_video_start(avsync->fd, pts);
        avsync->session_started = true;
        log_info("[%d]video start %u", avsync->session_id, pts);
    }

    if (avsync->start_policy == AV_SYNC_START_ALIGN &&
            !avsync->first_frame_toggled &&
            !msync_clock_started(avsync->fd)) {
        pthread_mutex_unlock(&avsync->lock);
        log_trace("[%d]clock not started", avsync->session_id);
        return NULL;
    }

    enter_last_frame = avsync->last_frame;
    msync_session_get_wall(avsync->fd, &systime, &interval);

    /* handle refresh rate change */
    if (avsync->vsync_interval == AV_SYNC_INVALID_PAUSE_PTS ||
        avsync->vsync_interval != interval) {
        log_info("[%d]vsync interval update %d --> %u",
                avsync->session_id, avsync->vsync_interval, interval);
        if (avsync->fps_interval == -1)
            avsync->fps_interval = interval;
        avsync->vsync_interval = interval;
        avsync->phase_set = false;
        reset_pattern(avsync->pattern_detector);
    }
    while (!peek_item(avsync->frame_q, (void **)&frame, 0)) {
        struct vframe *next_frame = NULL;

        peek_item(avsync->frame_q, (void **)&next_frame, 1);
        if (next_frame)
            log_debug("[%d]cur_f %u next_f %u size %d",
                avsync->session_id, frame->pts, next_frame->pts, queue_size(avsync->frame_q));
        if (frame_expire(avsync, systime, interval,
                frame, next_frame, toggle_cnt)) {
            log_debug("[%d]cur_f %u expire", avsync->session_id, frame->pts);
            toggle_cnt++;

            if (pattern_detect(avsync,
                    (avsync->last_frame?avsync->last_frame->hold_period:0),
                    avsync->last_holding_peroid))
                log_info("[%d] %u break the pattern", avsync->session_id, avsync->last_frame->pts);

            if (avsync->last_frame)
                avsync->last_holding_peroid = avsync->last_frame->hold_period;

            dqueue_item(avsync->frame_q, (void **)&frame);
            if (avsync->last_frame) {
                /* free frame that are not for display */
                if (toggle_cnt > 1) {
                    log_debug("[%d]free %u cur %u system/d %u/%u", avsync->session_id,
                             avsync->last_frame->pts, frame->pts, systime, systime - avsync->last_poptime);
                    avsync->last_frame->free(avsync->last_frame);
                }
            } else {
                avsync->first_frame_toggled = true;
                log_info("[%d]first frame %u", avsync->session_id, frame->pts);
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
        log_info ("[%d]reach pause pts: %u",
            avsync->session_id, avsync->last_frame->pts);
    }

exit:
    pthread_mutex_unlock(&avsync->lock);
    if (avsync->last_frame) {
        if (enter_last_frame != avsync->last_frame)
            log_debug("[%d]pop %u", avsync->session_id, avsync->last_frame->pts);
        log_trace("[%d]pop %u", avsync->session_id, avsync->last_frame->pts);
        /* don't update vpts for out_lier */
        if (avsync->last_frame->duration != -1)
            msync_session_update_vpts(avsync->fd, systime,
                avsync->last_frame->pts, interval * avsync->delay);
    } else
        if (enter_last_frame != avsync->last_frame)
            log_debug("[%d]pop (nil)", avsync->session_id);

    avsync->last_poptime = systime;
    if (avsync->last_frame)
        avsync->last_frame->hold_period++;
    return avsync->last_frame;
}

static inline uint32_t abs_diff(uint32_t a, uint32_t b)
{
    return (int)(a - b) > 0 ? a - b : b - a;
}

static uint64_t time_diff (struct timeval *b, struct timeval *a)
{
    return (b->tv_sec - a->tv_sec)*1000000 + (b->tv_usec - a->tv_usec);
}

static bool frame_expire(struct av_sync_session* avsync,
        uint32_t systime,
        uint32_t interval,
        struct vframe * frame,
        struct vframe * next_frame,
        int toggle_cnt)
{
    uint32_t fpts = frame->pts;
    bool expire = false;
    uint32_t pts_correction = avsync->delay * interval;

    if (avsync->mode == AV_SYNC_MODE_FREE_RUN)
        return true;

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

    log_trace("[%d]systime:%u phase:%u correct:%u fpts:%u",
            avsync->session_id, systime,
            avsync->phase_set?avsync->phase:0, pts_correction, fpts);
    if (abs_diff(systime, fpts) > avsync->disc_thres_min) {
        /* ignore discontinity under pause */
        if (avsync->paused)
            return false;

        if ((VALID_TS(avsync->last_log_syst) && avsync->last_log_syst != systime) ||
                (VALID_TS(avsync->last_pts) && avsync->last_pts != fpts)) {
            struct timeval now;

            gettimeofday(&now, NULL);
            avsync->last_log_syst = systime;
            avsync->last_pts = fpts;
            if (time_diff(&now, &avsync->sync_lost_print_time) >=
                SYNC_LOST_PRINT_THRESHOLD) {
                log_warn("[%d]sync lost systime:%u fpts:%u lost:%u",
                    avsync->session_id, systime, fpts, avsync->sync_lost_cnt);
                avsync->sync_lost_cnt = 0;
                gettimeofday(&avsync->sync_lost_print_time, NULL);
            } else
                avsync->sync_lost_cnt++;
        }

        if (avsync->state == AV_SYNC_STAT_SYNC_SETUP &&
                LIVE_MODE(avsync->mode) &&
                VALID_TS(avsync->last_pts) &&
                abs_diff(avsync->last_pts, fpts) > STREAM_DISC_THRES) {
            /* outlier by stream error */
            avsync->outlier_cnt++;
            frame->duration = -1;
            if (avsync->outlier_cnt < OUTLIER_MAX_CNT) {
                log_info("render outlier %u", fpts);
                return true;
            }
        }

        avsync->outlier_cnt = 0;
        avsync->state = AV_SYNC_STAT_SYNC_LOST;
        avsync->phase_set = false;
        reset_pattern(avsync->pattern_detector);

        if (LIVE_MODE(avsync->mode) && avsync->last_disc_pts != fpts) {
            log_info ("[%d]video disc %u --> %u",
                avsync->session_id, systime, fpts);
            msync_session_set_video_dis(avsync->fd, fpts);
            avsync->last_disc_pts = fpts;
        }

        if ((int)(systime - fpts) > 0) {
            if ((int)(systime - fpts) < avsync->disc_thres_max) {
                /* catch up PCR */
                return true;
            } else {
                /* render according to FPS */
                if (!VALID_TS(avsync->last_r_syst) ||
                        (int)(systime - avsync->last_r_syst) >= avsync->fps_interval) {
                    avsync->last_r_syst = systime;
                    return true;
                }
                return false;
            }
        } else if (LIVE_MODE(avsync->mode)) {
            /* hold if the gap is small */
            if ((int)(fpts - systime) < avsync->disc_thres_max) {
                return false;
            } else {
                /* render according to FPS */
                if (!VALID_TS(avsync->last_r_syst) ||
                        (int)(systime - avsync->last_r_syst) >= avsync->fps_interval) {
                    avsync->last_r_syst = systime;
                    return true;
                }
                return false;
            }
        }
    }

    /* In some cases, keeping pattern will enlarge the gap */
    if (abs_diff(systime, fpts) > AV_PATTERN_RESET_THRES &&
            avsync->first_frame_toggled) {
        reset_pattern(avsync->pattern_detector);
        log_warn("sync pattern reset sys:%u fpts:%u",
                    systime, fpts);
    }

    expire = (int)(systime - fpts) >= 0;

    /* scatter the frame in different vsync whenever possible */
    if (expire && next_frame && next_frame->pts && toggle_cnt) {
        /* multi frame expired in current vsync but no frame in next vsync */
        if (systime + interval < next_frame->pts) {
            expire = false;
            log_debug("[%d]unset expire systime:%d inter:%d next_pts:%d toggle_cnt:%d",
                    avsync->session_id, systime, interval, next_frame->pts, toggle_cnt);
        }
    } else if (!expire && next_frame && next_frame->pts && !toggle_cnt
               && avsync->first_frame_toggled) {
        /* next vsync will have at least 2 frame expired */
        if (systime + interval >= next_frame->pts) {
            expire = true;
            log_debug("[%d]set expire systime:%d inter:%d next_pts:%d",
                    avsync->session_id, systime, interval, next_frame->pts);
        }
    }

    if (avsync->state == AV_SYNC_STAT_SYNC_SETUP)
        correct_pattern(avsync->pattern_detector, frame, next_frame,
                (avsync->last_frame?avsync->last_frame->hold_period:0),
                avsync->last_holding_peroid, systime,
                interval, &expire);

    if (expire) {
        avsync->vpts = fpts;
        /* phase adjustment */
        if (!avsync->phase_set) {
            uint32_t phase_thres = interval / 4;
            if ( systime > fpts && (systime - fpts) < phase_thres) {
                /* too aligned to current VSYNC, separate them to 1/4 VSYNC */
                avsync->phase += phase_thres - (systime - fpts);
                avsync->phase_set = true;
                log_info("[%d]adjust phase to %d", avsync->session_id, avsync->phase);
            }
            if (!avsync->phase_set && systime > fpts &&
                systime < (fpts + interval) &&
                (systime - fpts) > interval - phase_thres) {
                /* too aligned to previous VSYNC, separate them to 1/4 VSYNC */
                avsync->phase += phase_thres + fpts + interval - systime;
                avsync->phase_set = true;
                log_info("[%d]adjust phase to %d", avsync->session_id, avsync->phase);
            }
        }

        if (avsync->state != AV_SYNC_STAT_SYNC_SETUP)
            log_info("[%d]sync setup", avsync->session_id);
        avsync->state = AV_SYNC_STAT_SYNC_SETUP;
        avsync->sync_lost_cnt = 0;
    }
    return expire;
}

static bool pattern_detect(struct av_sync_session* avsync, int cur_period, int last_period)
{
    bool ret = false;
    log_trace("[%d]cur_period: %d last_period: %d",
            avsync->session_id, cur_period, last_period);
    if (detect_pattern(avsync->pattern_detector, AV_SYNC_FRAME_P32, cur_period, last_period))
        ret = true;
    if (detect_pattern(avsync->pattern_detector, AV_SYNC_FRAME_P22, cur_period, last_period))
        ret = true;
    if (detect_pattern(avsync->pattern_detector, AV_SYNC_FRAME_P41, cur_period, last_period))
        ret = true;
    if (detect_pattern(avsync->pattern_detector, AV_SYNC_FRAME_P11, cur_period, last_period))
        ret = true;

    return ret;
}

int av_sync_set_speed(void *sync, float speed)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (speed < 0.001f || speed > 100) {
        log_error("[%d]wrong speed %f [0.0001, 100]", avsync->session_id, speed);
        return -1;
    }

    if (avsync->mode == AV_SYNC_MODE_PCR_MASTER ||
        avsync->mode == AV_SYNC_MODE_IPTV) {
        log_info("[%d]ignore set speed in mode %d", avsync->session_id, avsync->mode);
        return 0;
    }

    avsync->speed = speed;

    if (avsync->type == AV_SYNC_TYPE_AUDIO) {
        if (speed == 1.0) {
            avsync->mode = avsync->backup_mode;
            log_info("[%d]audio back to mode %d", avsync->session_id, avsync->mode);
        } else {
            avsync->backup_mode = avsync->mode;
            avsync->mode = AV_SYNC_MODE_FREE_RUN;
            log_info("[%d]audio to freerun mode", avsync->session_id);
        }
    }
#if 0
    if (avsync->mode != AV_SYNC_MODE_VMASTER) {
        log_info("ignore set speed in mode %d", avsync->mode);
        return 0;
    }
#endif

    log_info("session[%d] set rate to %f", avsync->session_id, speed);
    return msync_session_set_rate(avsync->fd, speed);
}

int av_sync_change_mode(void *sync, enum sync_mode mode)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync)
        return -1;

    if (msync_session_set_mode(avsync->fd, mode)) {
        log_error("[%d]fail to set mode %d", avsync->session_id, mode);
        return -1;
    }
    avsync->mode = mode;
    log_info("[%d]update sync mode to %d", avsync->session_id, mode);
    return 0;
}

int av_sync_get_mode(void *sync, enum sync_mode *mode)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync || !mode)
        return -1;

    *mode = avsync->mode;
    return 0;
}

int av_sync_set_pause_pts(void *sync, pts90K pts)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync)
      return -1;

    avsync->pause_pts = pts;
    log_info("[%d]set pause pts: %u", avsync->session_id, pts);
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

static void trigger_audio_start_cb(struct av_sync_session *avsync,
        avs_ascb_reason reason)
{
    if (avsync) {
        pthread_mutex_lock(&avsync->lock);
        if (avsync->audio_start) {
            avsync->audio_start(avsync->audio_start_priv, reason);
            avsync->session_started = true;
            avsync->audio_start = NULL;
            avsync->state = AV_SYNC_STAT_SYNC_SETUP;
        }
        pthread_mutex_unlock(&avsync->lock);
    }
}

avs_start_ret av_sync_audio_start(
    void *sync,
    pts90K pts,
    pts90K delay,
    audio_start_cb cb,
    void *priv)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;
    uint32_t start_mode;
    avs_start_ret ret = AV_SYNC_ASTART_ERR;
    bool create_poll_t = false;

    if (!avsync)
        return ret;

    if (msync_session_set_audio_start(avsync->fd, pts, delay, &start_mode))
        log_error("[%d]fail to set audio start", avsync->session_id);

    if (avsync->in_audio_switch
         && avsync->audio_switch_state == AUDIO_SWITCH_STAT_RESET) {
        log_info("%d audio_switch_state to start start mode %d",
                avsync->session_id, start_mode);
        avsync->audio_switch_state = AUDIO_SWITCH_STAT_START;
    }

    if (start_mode == AVS_START_SYNC) {
        ret = AV_SYNC_ASTART_SYNC;
        avsync->session_started = true;
        avsync->state = AV_SYNC_STAT_RUNNING;
    } else if (start_mode == AVS_START_ASYNC) {
        ret = AV_SYNC_ASTART_ASYNC;
        avsync->state = AV_SYNC_STAT_RUNNING;
    } else if (start_mode == AVS_START_AGAIN) {
        ret = AV_SYNC_ASTART_AGAIN;
    }

    if (ret == AV_SYNC_ASTART_AGAIN)
        goto exit;

    if (avsync->mode == AV_SYNC_MODE_AMASTER || avsync->in_audio_switch) {
        create_poll_t = true;
        if (start_mode == AVS_START_ASYNC) {
            if (!cb) {
                log_error("[%d]invalid cb", avsync->session_id);
                return AV_SYNC_ASTART_ERR;
            }
            avsync->audio_start = cb;
            avsync->audio_start_priv = priv;
        }
    } else if (LIVE_MODE(avsync->mode))
        create_poll_t = true;

    if (create_poll_t && !avsync->poll_thread) {
        int ret;

        log_info("[%d]start poll thread", avsync->session_id);
        avsync->quit_poll = false;
        ret = pthread_create(&avsync->poll_thread, NULL, poll_thread, avsync);
        if (ret) {
            log_error("[%d]create poll thread errno %d", avsync->session_id, errno);
            return AV_SYNC_ASTART_ERR;
        }
    }
    if (LIVE_MODE(avsync->mode)) {
        uint32_t systime;
        msync_session_get_wall(avsync->fd, &systime, NULL);
        log_info("[%d]return %u w %u pts %u d %u",
                avsync->session_id, ret, systime, pts, delay);
    }
exit:
    log_info("[%d]return %u", avsync->session_id, ret);
    return ret;
}

int av_sync_audio_render(
    void *sync,
    pts90K pts,
    struct audio_policy *policy)
{
    int ret = 0;
    bool out_lier = false;
    uint32_t systime;
    struct av_sync_session *avsync = (struct av_sync_session *)sync;
    avs_audio_action action = AA_SYNC_AA_MAX;

    if (!avsync || !policy)
        return -1;

    msync_session_get_wall(avsync->fd, &systime, NULL);

    log_trace("audio render pts %u, systime %u, mode %u diff ms %d",
         pts, systime, avsync->mode, (int)(pts-systime)/90);

    if (avsync->in_audio_switch
         && avsync->audio_switch_state == AUDIO_SWITCH_STAT_START) {
        if (abs_diff(systime, pts) < A_ADJ_THREDHOLD_HB) {
           log_info("Audio pts in system range sys %u pts %u\n", systime, pts);
           avsync->audio_switch_state = AUDIO_SWITCH_STAT_FINISH;
           action = AV_SYNC_AA_RENDER;
        } else if ((int)(systime - pts) > 0) {
            log_info("[%d] audio  change drop %d ms sys %u pts %u", avsync->session_id,
                     (int)(systime - pts)/90, systime, pts);
            action = AV_SYNC_AA_DROP;
        } else {
            action = AV_SYNC_AA_INSERT;
            log_info("[%d] audio change insert %d ms sys %u pts %u", avsync->session_id,
                     (int)(pts - systime)/90, systime, pts);
        }
        goto done;
    }

    if (avsync->mode == AV_SYNC_MODE_FREE_RUN ||
            avsync->mode == AV_SYNC_MODE_AMASTER) {
        action = AV_SYNC_AA_RENDER;
        goto done;
    }

    /* stopping procedure, unblock audio rendering */
    if (avsync->mode == AV_SYNC_MODE_PCR_MASTER &&
            avsync->active_mode == AV_SYNC_MODE_FREE_RUN) {
        action = AV_SYNC_AA_DROP;
        goto done;
    }

    if (avsync->mode == AV_SYNC_MODE_FREE_RUN ||
            avsync->mode == AV_SYNC_MODE_AMASTER) {
        action = AV_SYNC_AA_RENDER;
        goto done;
    }

    if (avsync->state == AV_SYNC_STAT_SYNC_SETUP &&
            LIVE_MODE(avsync->mode) &&
            abs_diff(systime, pts) > STREAM_DISC_THRES) {
        /* outlier by stream error */
        avsync->outlier_cnt++;
        if (avsync->outlier_cnt > OUTLIER_MAX_CNT) {
            /* treat as disc, just drop current frame */
            avsync->state = AV_SYNC_STAT_SYNC_LOST;
            avsync->outlier_cnt = 0;
            action = AV_SYNC_AA_DROP;
            systime = pts;
            goto done;
        }
        log_info("[%d]ignore outlier %u", avsync->session_id, pts);
        pts = systime;
        action = AV_SYNC_AA_RENDER;
        out_lier = true;
        goto done;
    }

    avsync->outlier_cnt = 0;
    /* low bound from sync_lost to sync_setup */
    if (abs_diff(systime, pts) < A_ADJ_THREDHOLD_LB) {
        avsync->state = AV_SYNC_STAT_SYNC_SETUP;
        action = AV_SYNC_AA_RENDER;
        goto done;
    }

    /* high bound of sync_setup */
    if (abs_diff(systime, pts) < A_ADJ_THREDHOLD_HB &&
            avsync->state != AV_SYNC_STAT_SYNC_LOST) {
        avsync->state = AV_SYNC_STAT_SYNC_SETUP;
        action = AV_SYNC_AA_RENDER;
        goto done;
    }

    if ((int)(systime - pts) > 0) {
        avsync->state = AV_SYNC_STAT_SYNC_LOST;
        action = AV_SYNC_AA_DROP;
        goto done;
    }

    if ((int)(systime - pts) < 0) {
        avsync->state = AV_SYNC_STAT_SYNC_LOST;
        action = AV_SYNC_AA_INSERT;
        goto done;
    }

done:
    policy->action = action;
    policy->delta = (int)(systime - pts);
    if (action == AV_SYNC_AA_RENDER) {
        avsync->apts = pts;
        if (!avsync->in_audio_switch) {
            if (!out_lier)
                msync_session_update_apts(avsync->fd, systime, pts, 0);
            log_debug("[%d]return %d sys %u - pts %u = %d",
                    avsync->session_id, action, systime, pts, systime - pts);
        } else if(avsync->audio_switch_state == AUDIO_SWITCH_STAT_FINISH) {
            msync_session_update_apts(avsync->fd, systime, pts, 0);
            log_info("[%d] audio switch done sys %u pts %u",
                avsync->session_id, systime, pts);
            msync_session_set_audio_switch(avsync->fd, false);
            avsync->in_audio_switch = false;
            avsync->audio_switch_state = AUDIO_SWITCH_STAT_INIT;
        } else {
            log_trace("[%d] in audio switch ret %d sys %u - pts %u = %d",
                avsync->session_id, action, systime, pts, systime - pts);
        }
        avsync->audio_drop_cnt = 0;
    } else {
        if (abs_diff(systime, pts) > avsync->disc_thres_min &&
                    avsync->last_disc_pts != pts &&
                    !avsync->in_audio_switch) {
            log_info ("[%d]audio disc %u --> %u",
                    avsync->session_id, systime, pts);
            msync_session_set_audio_dis(avsync->fd, pts);
            avsync->last_disc_pts = pts;
        } else if (action == AV_SYNC_AA_DROP) {
            struct timeval now;

            /* dropping recovery */
            gettimeofday(&now, NULL);
            if (!avsync->audio_drop_cnt)
                avsync->audio_drop_start = now;
            avsync->audio_drop_cnt++;
            if (time_diff(&now, &avsync->audio_drop_start) > 500000) {
                log_info ("[%d]audio keep dropping sys %u vs a %u",
                        avsync->session_id, systime, pts);
                msync_session_set_audio_dis(avsync->fd, pts);
            }
        }
        if (action != AV_SYNC_AA_DROP)
            avsync->audio_drop_cnt = 0;
        log_debug("[%d]return %d sys %u - pts %u = %d",
                avsync->session_id, action, systime, pts, systime - pts);
    }

    return ret;
}

int av_sync_get_clock(void *sync, pts90K *pts)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync || !pts)
        return -1;
    return msync_session_get_wall(avsync->fd, pts, NULL);
}

static void handle_mode_change_a(struct av_sync_session* avsync,
    bool v_active, bool a_active, bool v_timeout)
{
    log_info("[%d]amode %d mode %d v/a/vt %d/%d/%d", avsync->session_id,
            avsync->active_mode, avsync->mode, v_active, a_active, v_timeout);
    if (avsync->active_mode == AV_SYNC_MODE_AMASTER) {
        float speed;
        if (avsync->start_policy == AV_SYNC_START_ALIGN &&
                a_active && avsync->audio_start) {
            if (v_active || v_timeout || avsync->in_audio_switch) {
                log_info("audio start cb");
                trigger_audio_start_cb(avsync, AV_SYNC_ASCB_OK);
            }
        }

        if (!msync_session_get_rate(avsync->fd, &speed)) {
            /* speed change is triggered by asink,
             * attached audio HAL will handle it
             */
            if (speed != avsync->speed)
                log_info("[%d]new rate %f", avsync->session_id, speed);
            if (speed == 1.0) {
                avsync->mode = avsync->backup_mode;
                log_info("[%d]audio back to mode %d", avsync->session_id, avsync->mode);
            } else {
                avsync->backup_mode = avsync->mode;
                avsync->mode = AV_SYNC_MODE_FREE_RUN;
                log_info("[%d]audio to freerun mode", avsync->session_id);
            }
            avsync->speed = speed;
        }
    } else if (avsync->active_mode == AV_SYNC_MODE_PCR_MASTER) {
        struct session_debug debug;
        if (!msync_session_get_debug_mode(avsync->fd, &debug)) {
            if (debug.debug_freerun) {
                avsync->backup_mode = avsync->mode;
                avsync->mode = AV_SYNC_MODE_FREE_RUN;
                log_warn("[%d]audio to freerun mode", avsync->session_id);
            } else {
                avsync->mode = avsync->backup_mode;
                log_warn("[%d]audio back to mode %d",
                        avsync->session_id, avsync->mode);
            }
        }
    }
}

static void handle_mode_change_v(struct av_sync_session* avsync,
    bool v_active, bool a_active, bool v_timeout)
{
    log_info("[%d]amode mode %d %d v/a %d/%d", avsync->session_id,
            avsync->active_mode, avsync->mode, v_active, a_active);
    if (avsync->active_mode == AV_SYNC_MODE_PCR_MASTER) {
        struct session_debug debug;
        if (!msync_session_get_debug_mode(avsync->fd, &debug)) {
            if (debug.debug_freerun) {
                avsync->backup_mode = avsync->mode;
                avsync->mode = AV_SYNC_MODE_FREE_RUN;
                log_warn("[%d]video to freerun mode", avsync->session_id);
            } else
                avsync->mode = avsync->backup_mode;
                log_warn("[%d]video back to mode %d",
                        avsync->session_id, avsync->mode);
        }
    }
}

static void * poll_thread(void * arg)
{
    int ret = 0;
    struct av_sync_session *avsync = (struct av_sync_session *)arg;
    const int fd = avsync->fd;
    struct pollfd pfd = {
      /* default blocking capture */
      .events =  POLLIN | POLLRDNORM | POLLPRI | POLLOUT | POLLWRNORM,
      .fd = avsync->fd,
    };

    prctl (PR_SET_NAME, "avs_poll");
    log_info("[%d]enter", avsync->session_id);
    while (!avsync->quit_poll) {
        for (;;) {
          ret = poll(&pfd, 1, 10);
          if (ret > 0)
              break;
          if (avsync->quit_poll)
              goto exit;
          if (errno == EINTR)
              continue;
        }

        /* error handling */
        if (pfd.revents & POLLERR)
            log_error("[%d]POLLERR received", avsync->session_id);

        /* mode change. Non-exclusive wait so all the processes
         * shall be woken up
         */
        if (pfd.revents & POLLPRI) {
            bool v_active, a_active, v_timeout;

            msync_session_get_stat(fd, &avsync->active_mode,
                &v_active, &a_active, &v_timeout, &avsync->in_audio_switch);

            if (avsync->type == AV_SYNC_TYPE_AUDIO)
                handle_mode_change_a(avsync, v_active, a_active, v_timeout);
            else if (avsync->type == AV_SYNC_TYPE_VIDEO)
                handle_mode_change_v(avsync, v_active, a_active, v_timeout);
        }
    }
exit:
    log_info("[%d]quit", avsync->session_id);
    return NULL;
}

int av_sync_set_pcr_clock(void *sync, pts90K pts, uint64_t mono_clock)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;
    struct pcr_info pcr;
    enum pcr_monitor_status status;
    int ppm;
    if (!avsync)
        return -1;

    if (avsync->type != AV_SYNC_TYPE_PCR)
        return -2;

    pcr.monoclk = mono_clock / 1000;
    pcr.pts = (long long) pts * 1000 / 90;
    pcr_monitor_process(avsync->pcr_monitor, &pcr);

    status = pcr_monitor_get_status(avsync->pcr_monitor);

    if (status >= DEVIATION_READY) {
        pcr_monitor_get_deviation(avsync->pcr_monitor, &ppm);
        if (avsync->ppm != ppm) {
            avsync->ppm = ppm;
            log_info("[%d]ppm:%d", avsync->session_id, ppm);
            if (msync_session_set_clock_dev(avsync->fd, ppm))
                log_error("set clock dev fail");
        }
    }

    return msync_session_set_pcr(avsync->fd, pts, mono_clock);
}

int av_sync_get_pcr_clock(void *sync, pts90K *pts, uint64_t * mono_clock)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync)
        return -1;

    return msync_session_get_pcr(avsync->fd, pts, mono_clock);
}

int av_sync_set_session_name(void *sync, const char *name)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync)
        return -1;

    return msync_session_set_name(avsync->fd, name);
}

int av_sync_set_audio_switch(void *sync,  bool start)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;
    bool v_active, a_active, v_timeout;

    if (!avsync)
        return -1;
    if (msync_session_get_stat(avsync->fd, &avsync->active_mode,
                &v_active, &a_active,
                &v_timeout, &avsync->in_audio_switch)) {
        log_error("[%d] can not get session state",
                avsync->session_id);
        return -1;
    }
    if (!v_active || !a_active) {
        log_error("[%d]  no apply if not AV both active v %d a %d",
            avsync->session_id, v_active, a_active);
        return -1;
    }
    if (msync_session_set_audio_switch(avsync->fd, start)) {
        log_error("[%d]fail to set audio switch %d", avsync->session_id, start);
        return -1;
    }
    avsync->in_audio_switch = start;
    avsync->audio_switch_state = AUDIO_SWITCH_STAT_INIT;
    log_info("[%d]update audio switch to %d", avsync->session_id, start);
    return  0;
}

int av_sync_get_audio_switch(void *sync,  bool *start)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync)
        return -1;
    if (msync_session_get_stat(avsync->fd, &avsync->active_mode,
                NULL, NULL, NULL, &avsync->in_audio_switch)) {
        log_error("[%d] can not audio seamless switch state",
                avsync->session_id);
        return -1;
    }
    if (start) *start =  avsync->in_audio_switch;
    return 0;
}

enum  clock_recovery_stat av_sync_get_clock_devication(void *sync, int32_t *ppm)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync || !ppm)
        return CLK_RECOVERY_ERR;
    if (avsync->mode != AV_SYNC_MODE_PCR_MASTER)
        return CLK_RECOVERY_NOT_RUNNING;

    if (msync_session_get_clock_dev(avsync->fd, ppm))
        return CLK_RECOVERY_ERR;

    if (*ppm == 0)
        return CLK_RECOVERY_ONGOING;
    else
        return CLK_RECOVERY_READY;
}
