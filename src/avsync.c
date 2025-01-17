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
#include "aml_queue.h"
#include "pattern.h"
#include "aml_avsync_log.h"
#include "msync_util.h"
#include "msync.h"
#include <pthread.h>
#include "pcr_monitor.h"
#include "aml_version.h"

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
    AUDIO_SWITCH_STAT_AGAIN = 4,
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
    bool phase_adjusted;

    /* pts of last rendered frame */
    pts90K last_wall;
    pts90K last_pts;
    struct vframe *last_frame;

    /* pts of last pushed frame */
    pts90K last_q_pts;

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
    int extra_delay;
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
    /* underflow */
    underflow_detected underflow_cb;
    void *underflow_cb_priv;
    struct underflow_config underflow_cfg;
    struct timespec frame_last_update_time;

    /* log control */
    uint32_t last_log_syst;
    uint32_t sync_lost_cnt;
    struct timespec sync_lost_print_time;

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
    bool ppm_adjusted;

    //video FPS detection
    pts90K last_fpts;
    int fps_interval;
    int fps_interval_acc;
    int fps_cnt;

    //video freerun with rate control
    uint32_t last_r_syst;
    bool debug_freerun;

    //Audio dropping detection
    uint32_t audio_drop_cnt;
    struct timespec audio_drop_start;

    /*system mono time for current vsync interrupt */
    uint64_t msys;
};

#define MAX_FRAME_NUM 32
#define DEFAULT_START_THRESHOLD 2
#define TIME_UNIT90K    (90000)
#define DEFAULT_WALL_ADJ_THRES (TIME_UNIT90K / 10) //100ms
#define AV_DISC_THRES_MIN (TIME_UNIT90K / 3)
#define AV_DISC_THRES_MAX (TIME_UNIT90K * 10)
#define A_ADJ_THREDHOLD_HB (900 * 6) //60ms
#define A_ADJ_THREDHOLD_LB (900 * 2) //20ms
#define A_ADJ_THREDHOLD_MB (900 * 3) //30ms
#define AV_PATTERN_RESET_THRES (TIME_UNIT90K / 10)
#define SYNC_LOST_PRINT_THRESHOLD 10000000 //10 seconds In micro seconds
#define LIVE_MODE(m) ((m) == AV_SYNC_MODE_PCR_MASTER || (m) == AV_SYNC_MODE_IPTV)
#define V_DISC_MODE(mode) (LIVE_MODE(mode) || (mode) == AV_SYNC_MODE_VMASTER)

#define STREAM_DISC_THRES (TIME_UNIT90K / 10)
#define OUTLIER_MAX_CNT 8
#define VALID_TS(x) ((x) != -1)
#define UNDERFLOW_CHECK_THRESH_MS (100)

static uint64_t time_diff (struct timespec *b, struct timespec *a);
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
static struct vframe * video_mono_pop_frame(struct av_sync_session *avsync);
static int video_mono_push_frame(struct av_sync_session *avsync, struct vframe *frame);

pthread_mutex_t glock = PTHREAD_MUTEX_INITIALIZER;

int av_sync_open_session(int *session_id)
{
    int fd = -1;
    int id, rc;

    pthread_mutex_lock(&glock);
    fd = msync_create_session();
    if (fd < 0) {
        log_error("fail");
        goto exit;
    }
    rc = ioctl(fd, AMSYNC_IOC_ALLOC_SESSION, &id);
    if (rc) {
        log_error("new session errno:%d", errno);
        msync_destory_session(fd);
        goto exit;
    }
    *session_id = id;
    log_debug("new avsession id %d fd %d", id, fd);
exit:
    pthread_mutex_unlock(&glock);
    return fd;
}

void av_sync_close_session(int session)
{
    log_debug("session closed fd %d", session);
    pthread_mutex_lock(&glock);
    msync_destory_session(session);
    pthread_mutex_unlock(&glock);
}

static void* create_internal(int session_id,
        enum sync_mode mode,
        enum sync_type type,
        int start_thres,
        bool attach)
{
    struct av_sync_session *avsync = NULL;
    char dev_name[20];
    int retry = 10;

    /* debug log level */
    {
      const char *env= getenv( "AML_AVSYNC_DEBUG_LEVEL");
      if ( env ) {
        log_set_level(atoi(env));
      }
    }

    log_info("[%d] mode %d type %d", session_id, mode, type);
    avsync = (struct av_sync_session *)calloc(1, sizeof(*avsync));
    if (!avsync) {
        log_error("OOM");
        return NULL;
    }

    if (type == AV_SYNC_TYPE_VIDEO &&
            mode == AV_SYNC_MODE_VIDEO_MONO) {
      if (session_id < AV_SYNC_SESSION_V_MONO) {
          log_error("wrong session id %d", session_id);
          goto err;
      }
      avsync->type = type;
      avsync->mode = mode;
      avsync->fd = -1;
      avsync->session_id = session_id;
      log_info("[%d]init", avsync->session_id);
      return avsync;
    }

    if (type == AV_SYNC_TYPE_VIDEO) {
        int32_t interval = 1500;

        if (msync_session_get_vsync_interval(&interval))
            log_error("read interval error");
        avsync->pattern_detector = create_pattern_detector(interval);
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
        avsync->phase_adjusted = false;
        avsync->first_frame_toggled = false;

        avsync->frame_q = create_q(MAX_FRAME_NUM);
        if (!avsync->frame_q) {
            log_error("[%d]create queue fail", avsync->session_id);
            goto err2;
        }
        {
            int ret;

            ret = pthread_create(&avsync->poll_thread, NULL, poll_thread, avsync);
            if (ret) {
                log_error("[%d]create poll thread errno %d", avsync->session_id, errno);
                goto err2;
            }
        }
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
    avsync->last_q_pts = -1;
    avsync->last_wall = -1;
    avsync->fps_interval = -1;
    avsync->last_r_syst = -1;
    avsync->timeout = -1;
    avsync->apts = AV_SYNC_INVALID_PTS;

    if (msync_session_get_disc_thres(session_id,
                &avsync->disc_thres_min, &avsync->disc_thres_max)) {
        log_error("dev_name:%s; errno:%d; fail to get disc thres", dev_name, errno);
        avsync->disc_thres_min = AV_DISC_THRES_MIN;
        avsync->disc_thres_max = AV_DISC_THRES_MAX;
    }

    pthread_mutex_init(&avsync->lock, NULL);
    log_info("[%d] start_thres %d disc_thres %u/%u", session_id,
        start_thres, avsync->disc_thres_min, avsync->disc_thres_max);

    snprintf(dev_name, sizeof(dev_name), "/dev/%s%d", SESSION_DEV, session_id);
    while (retry) {
        /* wait for sysfs to update */
        avsync->fd = open(dev_name, O_RDONLY | O_CLOEXEC);
        if (avsync->fd > 0)
            break;

        retry--;
        if (!retry) {
          log_error("open %s errno %d", dev_name, errno);
          goto err2;
        }
        usleep(20000);
    }

    if (avsync->type == AV_SYNC_TYPE_PCR) {
        if (pcr_monitor_init(&avsync->pcr_monitor)) {
            log_error("pcr monitor init");
            goto err3;
        }
    }

    if (!attach) {
        msync_session_set_mode(avsync->fd, mode);
        avsync->mode = mode;
        if ((avsync->mode == AV_SYNC_MODE_VMASTER ||
            avsync->mode == AV_SYNC_MODE_IPTV) &&
            avsync->type == AV_SYNC_TYPE_VIDEO)
            msync_session_set_wall_adj_thres(avsync->fd, avsync->disc_thres_min);
    } else {
        avsync->attached = true;
        if (msync_session_get_mode(avsync->fd, &avsync->mode)) {
            log_error("get mode");
            goto err4;
        }
        avsync->backup_mode = avsync->mode;
        if (msync_session_get_start_policy(avsync->fd, &avsync->start_policy, &avsync->timeout)) {
            log_error("get policy");
            goto err4;
        }
        if (msync_session_get_stat(avsync->fd, false, &avsync->active_mode, NULL,
                NULL, NULL, NULL, &avsync->in_audio_switch, SRC_A)) {
            log_error("get state");
            goto err4;
        }
        if (avsync->in_audio_switch) {
            log_info("audio_switch_state reseted the audio");
            avsync->audio_switch_state = AUDIO_SWITCH_STAT_RESET;
        }

        log_info("[%d]retrieve sync mode %d policy %d",
            session_id, avsync->mode, avsync->start_policy);
    }

    return avsync;
err4:
    if (avsync->pcr_monitor)
        pcr_monitor_destroy(avsync->pcr_monitor);
err3:
    if (avsync->fd)
        close(avsync->fd);
err2:
    avsync->quit_poll = true;
    if (avsync->poll_thread) {
        pthread_join(avsync->poll_thread, NULL);
        avsync->poll_thread = 0;
    }
    if (avsync->frame_q)
        destroy_q(avsync->frame_q);
    if (avsync->pattern_detector)
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
    if (type == AV_SYNC_TYPE_VIDEO)
        return NULL;
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
    avsync->extra_delay = config->extra_delay * 90;

    log_info("[%d] vsync delay: %d extra_delay: %d ms",
            avsync->session_id, config->delay, config->extra_delay);
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

    if (avsync->type == AV_SYNC_TYPE_VIDEO &&
            avsync->mode == AV_SYNC_MODE_VIDEO_MONO) {
        log_info("[%d]done", avsync->session_id);
        internal_stop(avsync);
        destroy_q(avsync->frame_q);
        free(avsync);
        return;
    }
    log_info("[%d]begin type %d", avsync->session_id, avsync->type);
    if (avsync->type == AV_SYNC_TYPE_VIDEO)
        internal_stop(avsync);

    avsync->quit_poll = true;
    if (avsync->poll_thread) {
        pthread_join(avsync->poll_thread, NULL);
        avsync->poll_thread = 0;
    }
    if (avsync->type == AV_SYNC_TYPE_AUDIO)
        trigger_audio_start_cb(avsync, AV_SYNC_ASCB_STOP);

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
    log_info("[%d]done type %d", avsync->session_id, avsync->type);
    free(avsync);
}

int avs_sync_set_start_policy(void *sync, struct start_policy* st_policy)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync || !avsync->fd)
        return -1;

    log_info("[%d]policy %u --> %u, timeout %d --> %d", avsync->session_id,
        avsync->start_policy, st_policy->policy, avsync->timeout, st_policy->timeout);
    if (avsync->mode == AV_SYNC_MODE_IPTV &&
        st_policy->policy != AV_SYNC_START_ASAP) {
        log_error("policy %d not supported in live mode", st_policy->policy);
        return -1;
    }

    if (avsync->mode == AV_SYNC_MODE_PCR_MASTER)
        msync_session_set_start_thres(avsync->fd, st_policy->timeout);

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

    if (!avsync) {
        log_error("invalid handle");
        return -1;
    }

    if (avsync->type == AV_SYNC_TYPE_VIDEO && avsync->mode == AV_SYNC_MODE_VIDEO_MONO) {
        log_warn("ignore pause in video mono mode");
        return -1;
    }

    if (avsync->mode == AV_SYNC_MODE_PCR_MASTER) {
        log_warn("ignore pause in pcr master mode");
        return -1;
    }

     rc = msync_session_get_stat(avsync->fd, false, &avsync->active_mode, NULL,
                &v_active, &a_active, &v_timeout,
                &avsync->in_audio_switch, SRC_A);

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
    if (!avsync->paused && avsync->first_frame_toggled) {
        clock_gettime(CLOCK_MONOTONIC_RAW, &avsync->frame_last_update_time);
        log_info("[%d] resume update new frame time", avsync->session_id);
    }
    return rc;
}

int av_sync_push_frame(void *sync , struct vframe *frame)
{
    int ret;
    struct vframe *prev = NULL;
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync)
        return -1;

    if (avsync->type == AV_SYNC_TYPE_VIDEO &&
            avsync->mode == AV_SYNC_MODE_VIDEO_MONO) {
        return video_mono_push_frame(avsync, frame);
    }

    if (avsync->state == AV_SYNC_STAT_INIT && !queue_size(avsync->frame_q)) {
        /* policy should be final now */
        if (msync_session_get_start_policy(avsync->fd, &avsync->start_policy, &avsync->timeout)) {
            log_error("[%d]get policy", avsync->session_id);
            return -1;
        }
    }

    if (avsync->last_q_pts != -1) {
        if (frame->pts != -1 && avsync->mode == AV_SYNC_MODE_VMASTER) {
            /* Sometimes app will fake PTS for trickplay, video PTS gap
             * is really big depending on the speed. Have to adjust the
             * threshold dynamically.
             */
            int gap = (int)(frame->pts - avsync->last_q_pts);
            if (gap > avsync->disc_thres_min) {
                avsync->disc_thres_min = gap * 6;
                avsync->disc_thres_max = gap * 20;
                msync_session_set_wall_adj_thres(avsync->fd, avsync->disc_thres_min);
                msync_session_set_disc_thres(avsync->session_id,
                        avsync->disc_thres_min, avsync->disc_thres_max);
                log_info("[%d] update disc_thres to %d/%d",avsync->session_id,
                        avsync->disc_thres_min, avsync->disc_thres_max);
            }
        }
        if (avsync->last_q_pts == frame->pts && avsync->mode == AV_SYNC_MODE_AMASTER) {
            /* TODO: wrong, should remove from back of queue */
            pthread_mutex_lock(&avsync->lock);
            dqueue_item(avsync->frame_q, (void **)&prev);
            if (prev) {
                prev->free(prev);
                log_info ("[%d]drop frame with same pts %u", avsync->session_id, frame->pts);
            }
            pthread_mutex_unlock(&avsync->lock);
        } else if (avsync->fps_cnt < 100) {
            int32_t interval = frame->pts - avsync->last_q_pts;

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
    avsync->last_q_pts = frame->pts;
    ret = queue_item(avsync->frame_q, frame);
    if (avsync->state == AV_SYNC_STAT_INIT &&
        queue_size(avsync->frame_q) >= avsync->start_thres) {
        avsync->state = AV_SYNC_STAT_RUNNING;
        log_debug("[%d]state: init --> running", avsync->session_id);
    }

    if (ret)
        log_error("queue fail:%d", ret);
    log_debug("[%d]push %u, QNum=%d", avsync->session_id, frame->pts, queue_size(avsync->frame_q));
    return ret;
}

struct vframe *av_sync_pop_frame(void *sync)
{
    struct vframe *frame = NULL, *enter_last_frame = NULL;
    struct av_sync_session *avsync = (struct av_sync_session *)sync;
    int toggle_cnt = 0;
    uint32_t systime = 0;
    bool pause_pts_reached = false;
    uint32_t interval = 0;

    if (avsync->type == AV_SYNC_TYPE_VIDEO &&
            avsync->mode == AV_SYNC_MODE_VIDEO_MONO)
        return video_mono_pop_frame(avsync);

    pthread_mutex_lock(&avsync->lock);
    if (avsync->state == AV_SYNC_STAT_INIT) {
        log_debug("[%d]in state INIT", avsync->session_id);
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
        log_info("[%d]video start %u frame %u sys %u",
            avsync->session_id, pts, frame->pts, systime);
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
        avsync->phase_adjusted = false;
        avsync->phase = 0;
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
                    avsync->last_holding_peroid)) {
                log_info("[%d] %u break the pattern", avsync->session_id, avsync->last_frame->pts);
                log_info("[%d] cur frame %u sys %u", avsync->session_id, frame->pts, systime);
                if (next_frame)
                    log_info("[%d] next frame %u", avsync->session_id, next_frame->pts);
            }

            if (avsync->last_frame)
                avsync->last_holding_peroid = avsync->last_frame->hold_period;

            dqueue_item(avsync->frame_q, (void **)&frame);
            if (avsync->last_frame) {
                int qsize = queue_size(avsync->frame_q);

                /* free frame that are not for display */
                if (toggle_cnt > 1) {
                    log_info("[%d]free %u cur %u system/d %u/%u queue size %d", avsync->session_id,
                             avsync->last_frame->pts, frame->pts,
                             systime, systime - avsync->last_poptime,
                             qsize);
                    avsync->last_frame->free(avsync->last_frame);
                }
            } else {
                avsync->first_frame_toggled = true;
                log_info("[%d]first frame %u queue size %d", avsync->session_id, frame->pts, queue_size(avsync->frame_q));
            }
            avsync->last_frame = frame;
            avsync->last_pts = frame->pts;
            clock_gettime(CLOCK_MONOTONIC_RAW, &avsync->frame_last_update_time);
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
        /* stay in paused until av_sync_pause(false) */
        uint32_t local_pts = avsync->pause_pts;
        avsync->paused = true;
        log_info ("[%d]reach pause pts: %u",
            avsync->session_id, avsync->pause_pts);
        avsync->pause_pts = AV_SYNC_INVALID_PAUSE_PTS;
        if (avsync->pause_pts_cb)
            avsync->pause_pts_cb(local_pts,
                    avsync->pause_cb_priv);
        log_info ("[%d] reach pause pts: %u handle done",
            avsync->session_id, local_pts);
    }

exit:
    pthread_mutex_unlock(&avsync->lock);

    /* underflow check */
    if (avsync->session_started && avsync->first_frame_toggled &&
        (avsync->paused == false) && (avsync->state >= AV_SYNC_STAT_RUNNING) &&
        avsync->underflow_cb && peek_item(avsync->frame_q, (void **)&frame, 0))
    {/* empty queue in normal play */
        struct timespec now;
        int diff_ms;
        clock_gettime(CLOCK_MONOTONIC_RAW, &now);
        diff_ms = time_diff(&now, &avsync->frame_last_update_time)/1000;
        if(diff_ms >= (avsync->underflow_cfg.time_thresh
                       + avsync->vsync_interval*avsync->last_holding_peroid/90)) {
            log_info ("[%d]underflow detected: %u", avsync->session_id, avsync->last_pts);
            avsync->underflow_cb (avsync->last_pts,
                    avsync->underflow_cb_priv);
            /* update time to control the underflow check call backs */
            avsync->frame_last_update_time = now;
        }
    }

    if (avsync->last_frame) {
        if (enter_last_frame != avsync->last_frame) {
            log_debug("[%d]pop %u", avsync->session_id, avsync->last_frame->pts);
            /* don't update vpts for out_lier */
            if (avsync->last_frame->duration != -1)
                msync_session_update_vpts(avsync->fd, systime,
                  avsync->last_frame->pts + avsync->extra_delay, interval * avsync->delay);
        }
        log_trace("[%d]pop=%u, stc=%u, QNum=%d", avsync->session_id, avsync->last_frame->pts, systime, queue_size(avsync->frame_q));
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

static uint64_t time_diff (struct timespec *b, struct timespec *a)
{
    return (uint64_t)(b->tv_sec - a->tv_sec)*1000000 + (b->tv_nsec/1000 - a->tv_nsec/1000);
}

static bool frame_expire(struct av_sync_session* avsync,
        uint32_t systime,
        uint32_t interval,
        struct vframe * frame,
        struct vframe * next_frame,
        int toggle_cnt)
{
    uint32_t fpts = frame->pts + avsync->extra_delay;
    uint32_t nfpts = -1;
    uint32_t last_pts = avsync->last_pts;
    bool expire = false;
    uint32_t pts_correction = avsync->delay * interval;

    if (!VALID_TS(systime))
        return false;

    if (avsync->paused && avsync->pause_pts == AV_SYNC_INVALID_PAUSE_PTS)
        return false;

    if (avsync->pause_pts == AV_SYNC_STEP_PAUSE_PTS)
        return true;

    if (avsync->pause_pts != AV_SYNC_INVALID_PAUSE_PTS &&
        avsync->pause_pts == frame->pts)
        return true;

    if (systime == AV_SYNC_INVALID_PTS &&
            avsync->mode == AV_SYNC_MODE_AMASTER)
        return false;

    if (next_frame)
        nfpts = next_frame->pts + avsync->extra_delay;

    if (avsync->mode == AV_SYNC_MODE_FREE_RUN) {
        /* We need to ensure that the video outputs smoothly,
        so output video frame by frame hold_period */
        if ((abs_diff(systime, fpts) > AV_PATTERN_RESET_THRES) &&
               avsync->last_frame &&
               (avsync->last_frame->hold_period >= (avsync->fps_interval/interval))) {
            log_debug("[%d]vmaster/freerun  systime:%u --> fpts:%u,last hold_period:%d",
                avsync->session_id, systime, fpts,avsync->last_frame->hold_period);
            return true;
        }
    }

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

    log_trace("[%d]systime:%u phase:%d correct:%u fpts:%u",
            avsync->session_id, systime,
            avsync->phase_set? (int)avsync->phase:0, pts_correction, fpts);
    if (abs_diff(systime, fpts) > avsync->disc_thres_min) {
        /* ignore discontinity under pause */
        if (avsync->paused)
            return false;

        if ((VALID_TS(avsync->last_log_syst) && avsync->last_log_syst != systime) ||
                (VALID_TS(avsync->last_pts) && avsync->last_pts != fpts)) {
            struct timespec now;

            clock_gettime(CLOCK_MONOTONIC_RAW, &now);
            avsync->last_log_syst = systime;
            avsync->last_pts = fpts;
            if (time_diff(&now, &avsync->sync_lost_print_time) >=
                SYNC_LOST_PRINT_THRESHOLD) {
                log_warn("[%d]sync lost systime:%u fpts:%u lost:%u",
                    avsync->session_id, systime, fpts, avsync->sync_lost_cnt);
                avsync->sync_lost_cnt = 0;
                clock_gettime(CLOCK_MONOTONIC_RAW, &avsync->sync_lost_print_time);
            } else
                avsync->sync_lost_cnt++;
        }

        if (avsync->state == AV_SYNC_STAT_SYNC_SETUP &&
                LIVE_MODE(avsync->mode) &&
                VALID_TS(last_pts) &&
                abs_diff(last_pts, fpts) > STREAM_DISC_THRES) {
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
        avsync->phase_adjusted = false;
        avsync->phase = 0;
        reset_pattern(avsync->pattern_detector);

        if (V_DISC_MODE(avsync->mode) && avsync->last_disc_pts != fpts) {
            log_info ("[%d]video disc %u --> %u",
                avsync->session_id, systime, fpts);
            msync_session_set_video_dis(avsync->fd, fpts);
            avsync->last_disc_pts = fpts;
            if (avsync->mode == AV_SYNC_MODE_VMASTER) {
                systime = fpts;
                avsync->last_r_syst = -1;
            }
        }

        if ((int)(systime - fpts) > 0) {
            if ((int)(systime - fpts) < avsync->disc_thres_max) {
                /* catch up PCR */
                avsync->last_r_syst = -1;
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
        log_info("sync pattern reset sys:%u fpts:%u",
                    systime, fpts);
    }

    expire = (int)(systime - fpts) >= 0;

    /* scatter the frame in different vsync whenever possible */
    if (expire && nfpts != -1 && nfpts && toggle_cnt) {
        /* multi frame expired in current vsync but no frame in next vsync */
        if (systime + interval < nfpts) {
            expire = false;
            log_debug("[%d]unset expire systime:%d inter:%d next_pts:%d toggle_cnt:%d",
                    avsync->session_id, systime, interval, nfpts, toggle_cnt);
        }
    } else if (!expire && nfpts != -1 && nfpts && !toggle_cnt
               && avsync->first_frame_toggled) {
        /* next vsync will have at least 2 frame expired */
        if (systime + interval >= nfpts) {
            expire = true;
            log_debug("[%d]set expire systime:%d inter:%d next_pts:%d",
                    avsync->session_id, systime, interval, nfpts);
        }
    }

    if (avsync->state == AV_SYNC_STAT_SYNC_SETUP)
        correct_pattern(avsync->pattern_detector, fpts, nfpts,
                (avsync->last_frame?avsync->last_frame->hold_period:0),
                avsync->last_holding_peroid, systime,
                interval, &expire);

    if (expire) {
        avsync->vpts = fpts;
        /* phase adjustment */
        if (!avsync->phase_set) {
            //always adjust to the half v-sync to give most pts tolerace and unify behavior
            if ((int)(systime - fpts) >= 0 &&
                    (int)(fpts + interval - systime) > 0) {
                avsync->phase = interval / 2 + fpts - systime;
                avsync->phase_set = true;
                log_debug("[%d]adjust phase to %d", avsync->session_id, (int)avsync->phase);
            }
        } else if (get_pattern(avsync->pattern_detector) < 0 && !avsync->phase_adjusted) {
            if ((int)(systime - fpts) >= 0 &&
                    (int)(fpts + interval - systime) > 0) {
                int vsync_pts_delta = (int)(systime - fpts);

                if (vsync_pts_delta < 10 || vsync_pts_delta > (interval - 10)) {
                    avsync->phase += interval / 8;
                    avsync->phase_adjusted = true;
                    log_info("[%d] too aligned adjust phase to %d",
                            avsync->session_id, (int)avsync->phase);
                }
            }
        }
        if (avsync->state != AV_SYNC_STAT_SYNC_SETUP)
            log_info("[%d]sync setup on frame %u", avsync->session_id, fpts);
        avsync->state = AV_SYNC_STAT_SYNC_SETUP;
        avsync->sync_lost_cnt = 0;
    }
    return expire;
}

static bool pattern_detect(struct av_sync_session* avsync, int cur_period, int last_period)
{
    log_trace("[%d]cur_period: %d last_period: %d",
            avsync->session_id, cur_period, last_period);
    return detect_pattern(avsync->pattern_detector, cur_period, last_period);
}

int av_sync_set_speed(void *sync, float speed)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (speed < 0.001f || speed > 100) {
        log_error("[%d]wrong speed %f [0.0001, 100]", avsync->session_id, speed);
        return -1;
    }

    if (LIVE_MODE(avsync->mode)) {
        log_info("[%d]ignore set speed in mode %d", avsync->session_id, avsync->mode);
        return 0;
    }

    avsync->speed = speed;

    if (avsync->type == AV_SYNC_TYPE_AUDIO) {
        if (speed == 1.0)
            msync_session_set_wall_adj_thres(avsync->fd, DEFAULT_WALL_ADJ_THRES);
        else
            msync_session_set_wall_adj_thres(avsync->fd, avsync->disc_thres_min);
        log_info("[%d]adjust wall adj threshold to %d", avsync->session_id,
            (speed == 1.0) ? DEFAULT_WALL_ADJ_THRES : avsync->disc_thres_min);
    }

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

int av_sync_change_mode_by_id(int id, enum sync_mode mode)
{
    int fd;
    char dev_name[20];

    snprintf(dev_name, sizeof(dev_name), "/dev/%s%d", SESSION_DEV, id);
    fd = open(dev_name, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        log_error("open %s errno %d", dev_name, errno);
        return -1;
    }

    if (msync_session_set_mode(fd, mode)) {
        log_error("[%d]fail to set mode %d", id, mode);
        close(fd);
        return -1;
    }

    close(fd);
    log_info("session[%d] set mode %d", id, mode);
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

int av_sync_set_underflow_check_cb(void *sync, underflow_detected cb, void *priv, struct underflow_config *cfg)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync)
      return -1;

    avsync->underflow_cb = cb;
    avsync->underflow_cb_priv = priv;

    if (cfg)
        avsync->underflow_cfg.time_thresh = cfg->time_thresh;
    else
        avsync->underflow_cfg.time_thresh = UNDERFLOW_CHECK_THRESH_MS;

    log_info("[%d] av_sync_set_underflow_check_cb %p priv %p time %d",
             avsync->session_id, avsync->underflow_cb, avsync->underflow_cb_priv,
             avsync->underflow_cfg.time_thresh);
    return 0;
}
static void trigger_audio_start_cb(struct av_sync_session *avsync,
        avs_ascb_reason reason)
{
    if (avsync) {
        log_info("audio start cb");
        pthread_mutex_lock(&avsync->lock);
        if (avsync->audio_start) {
            avsync->audio_start(avsync->audio_start_priv, reason);
            avsync->session_started = true;
            avsync->audio_start = NULL;
            avsync->audio_start_priv = NULL;
            avsync->state = AV_SYNC_STAT_SYNC_SETUP;
        }
        pthread_mutex_unlock(&avsync->lock);
    }
}

static int update_pcr_master_disc_thres(struct av_sync_session * avsync, pts90K pts)
{
  pts90K pcr = -1;

  if (!msync_session_get_pcr(avsync->fd, &pcr, NULL) && pcr != -1) {
      pts90K delta = abs_diff(pcr, pts);

      if (delta * 3 > avsync->disc_thres_min)
          avsync->disc_thres_min = 3 * delta;

      log_info("%d update disc_thres_min to %u delta %u",
          avsync->session_id, avsync->disc_thres_min, delta);
      return 0;
  }
  return -1;
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
    uint32_t systime = 0;
    avs_start_ret ret = AV_SYNC_ASTART_ERR;
    bool create_poll_t = false;

    if (!avsync)
        return ret;

    log_info("%d av_sync_audio_start pts(ms) %d delay %d ms",
             avsync->session_id, (int)pts/90, (int)delay/90);

    if (avsync->in_audio_switch) {
        msync_session_get_wall(avsync->fd, &systime, NULL);
        if (systime == AV_SYNC_INVALID_PTS) {
                log_info("%d Invalid systime could be paused pts %d ms switch_state %d again",
                avsync->session_id, (int) pts/90, avsync->audio_switch_state);
                avsync->audio_switch_state = AUDIO_SWITCH_STAT_RESET;
                ret = AV_SYNC_ASTART_AGAIN;
                goto exit;
        }
    }

    if (avsync->in_audio_switch &&
        avsync->audio_switch_state == AUDIO_SWITCH_STAT_AGAIN)
    {
        start_mode = AVS_START_SYNC;
        log_info("%d AUDIO_SWITCH_STAT_AGAIN", avsync->session_id);
    } else {
        if (msync_session_set_audio_start(avsync->fd, pts, delay, &start_mode))
            log_error("[%d]fail to set audio start", avsync->session_id);
    }
    if (avsync->in_audio_switch &&
        (avsync->audio_switch_state == AUDIO_SWITCH_STAT_RESET ||
         avsync->audio_switch_state == AUDIO_SWITCH_STAT_AGAIN)) {
        if ((int)(systime - pts) > A_ADJ_THREDHOLD_LB
            && start_mode == AVS_START_SYNC) {
            log_info("%d audio_switch audio need drop first.ahead %d ms",
                avsync->session_id, (int)(systime - pts)/90);
            ret = AV_SYNC_ASTART_AGAIN;
            avsync->audio_switch_state = AUDIO_SWITCH_STAT_AGAIN;
            goto exit;
        }
        else {
            int diff = (int)(pts - systime);
            log_info("%d audio_switch_state to start mode %d diff %d ms",
                avsync->session_id, start_mode, diff/90);
            if (diff < A_ADJ_THREDHOLD_LB) {
                log_info("%d orig mode %d already close enough direct start",
                               avsync->session_id, start_mode);
                start_mode = AVS_START_SYNC;
            } else if (start_mode != AVS_START_ASYNC) {
                log_info("%d drop too far mode %d need to try ASYNC",
                               avsync->session_id, start_mode);
                msync_session_set_audio_stop(avsync->fd);
                if (msync_session_set_audio_start(avsync->fd, pts, delay, &start_mode))
                    log_error("[%d]fail to set audio start", avsync->session_id);
                log_info("%d New start mode %d",
                               avsync->session_id, start_mode);
            }
            avsync->audio_switch_state = AUDIO_SWITCH_STAT_START;
        }
    }

    if (start_mode == AVS_START_SYNC) {
        ret = AV_SYNC_ASTART_SYNC;
        avsync->session_started = true;
        avsync->state = AV_SYNC_STAT_RUNNING;

        /* for DTG stream, initial delta between apts and pcr is big */
        if (avsync->mode == AV_SYNC_MODE_PCR_MASTER)
            update_pcr_master_disc_thres(avsync, pts);

    } else if (start_mode == AVS_START_ASYNC) {
        ret = AV_SYNC_ASTART_ASYNC;
        avsync->state = AV_SYNC_STAT_RUNNING;

        /* for DTG stream, initial delta between apts and pcr is big */
        if (avsync->mode == AV_SYNC_MODE_PCR_MASTER)
          update_pcr_master_disc_thres(avsync, pts);
    } else if (start_mode == AVS_START_AGAIN) {
        ret = AV_SYNC_ASTART_AGAIN;
    }

    avsync->last_pts = pts;
    if (ret == AV_SYNC_ASTART_AGAIN)
        goto exit;

    if (avsync->mode == AV_SYNC_MODE_AMASTER ||
            avsync->in_audio_switch || LIVE_MODE(avsync->mode))
        create_poll_t = true;

    if (start_mode == AVS_START_ASYNC) {
        if (!cb) {
            log_error("[%d]invalid cb", avsync->session_id);
            return AV_SYNC_ASTART_ERR;
        }
        avsync->audio_start = cb;
        avsync->audio_start_priv = priv;
    }

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
    bool send_disc = false;
    uint32_t systime;
    struct av_sync_session *avsync = (struct av_sync_session *)sync;
    avs_audio_action action = AA_SYNC_AA_MAX;

    if (!avsync || !policy)
        return -1;

    msync_session_get_wall(avsync->fd, &systime, NULL);
    avsync->last_pts = pts;

    log_trace("audio render pts %u, systime %u, mode %u diff ms %d",
         pts, systime, avsync->mode, (int)(pts-systime)/90);

    if (avsync->in_audio_switch
         && avsync->audio_switch_state == AUDIO_SWITCH_STAT_START) {
        if (abs_diff(systime, pts) < A_ADJ_THREDHOLD_MB) {
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
    if (LIVE_MODE(avsync->mode) &&
            avsync->active_mode == AV_SYNC_MODE_FREE_RUN) {
        action = AV_SYNC_AA_DROP;
        goto done;
    }

    if (avsync->mode == AV_SYNC_MODE_FREE_RUN ||
            avsync->mode == AV_SYNC_MODE_AMASTER ||
            avsync->active_mode == AV_SYNC_MODE_AMASTER) {
        action = AV_SYNC_AA_RENDER;
        goto done;
    }

    if (LIVE_MODE(avsync->mode) &&
            VALID_TS(avsync->apts) &&
            abs_diff(avsync->apts, pts) > STREAM_DISC_THRES) {
        /* outlier by stream error */
        avsync->outlier_cnt++;
        if (avsync->outlier_cnt > OUTLIER_MAX_CNT) {
            /* treat as disc */
            send_disc = true;
        } else {
            log_info("[%d]ignore outlier %u vs %u sys %u", avsync->session_id, pts, avsync->apts, systime);
            pts = systime;
            action = AV_SYNC_AA_RENDER;
            out_lier = true;
            goto done;
        }
    }

    /* low bound from sync_lost to sync_setup */
    if (abs_diff(systime, pts) < A_ADJ_THREDHOLD_LB) {
        avsync->outlier_cnt = 0;
        avsync->state = AV_SYNC_STAT_SYNC_SETUP;
        action = AV_SYNC_AA_RENDER;
        goto done;
    }

    /* high bound of sync_setup */
    if (abs_diff(systime, pts) < A_ADJ_THREDHOLD_HB &&
            avsync->state != AV_SYNC_STAT_SYNC_LOST) {
        avsync->outlier_cnt = 0;
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
        if (!out_lier)
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
        if (!avsync->in_audio_switch && avsync->last_disc_pts != pts &&
             (abs_diff(systime, pts) > avsync->disc_thres_min ||
              (action == AV_SYNC_AA_INSERT && send_disc))) {
            log_info ("[%d]audio disc %u --> %u",
                    avsync->session_id, systime, pts);
            msync_session_set_audio_dis(avsync->fd, pts);
            avsync->last_disc_pts = pts;
        } else if (action == AV_SYNC_AA_DROP) {
            struct timespec now;

            avsync->apts = pts;
            /* dropping recovery */
            clock_gettime(CLOCK_MONOTONIC_RAW, &now);
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

int av_sync_get_pos(void *sync, pts90K *pts, uint64_t *mono_clock)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync || !pts)
        return -1;

    if (avsync->type != AV_SYNC_TYPE_AUDIO &&
        avsync->type != AV_SYNC_TYPE_VIDEO)
        return -2;
    return msync_session_get_pts(avsync->fd, pts,
        mono_clock, avsync->type == AV_SYNC_TYPE_VIDEO);
}

int av_sync_get_clock(void *sync, pts90K *pts)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync || !pts)
        return -1;
    return msync_session_get_wall(avsync->fd, pts, NULL);
}

static void handle_mode_change_a(struct av_sync_session* avsync,
    enum internal_sync_stat stat,
    bool v_active, bool a_active, bool v_timeout)
{
    log_info("[%d]av_sync amode %d mode %d v/a/vt %d/%d/%d stat %d",
            avsync->session_id, avsync->active_mode, avsync->mode,
            v_active, a_active, v_timeout, stat);

    /* iptv delayed start */
    if (avsync->mode == AV_SYNC_MODE_IPTV && avsync->audio_start)
        trigger_audio_start_cb(avsync, AV_SYNC_ASCB_OK);

    if (avsync->active_mode == AV_SYNC_MODE_AMASTER) {
        float speed;
        if (a_active && avsync->audio_start) {
            if (v_active || v_timeout || avsync->in_audio_switch)
                trigger_audio_start_cb(avsync, AV_SYNC_ASCB_OK);
        } else if (!a_active && !avsync->session_started) {
            /* quit waiting ASAP */
            trigger_audio_start_cb(avsync, AV_SYNC_ASCB_STOP);
        }

        if (!msync_session_get_rate(avsync->fd, &speed)) {
            /* speed change is triggered by asink,
             * attached audio HAL will handle it
             */
            if (speed != avsync->speed)
                log_info("[%d]new rate %f", avsync->session_id, speed);
            avsync->speed = speed;
        }
    } else if (avsync->mode == AV_SYNC_MODE_PCR_MASTER &&
        avsync->active_mode == AV_SYNC_MODE_FREE_RUN) {
        /* pcr master stopping procedure */
        if (a_active && avsync->audio_start) {
            if (v_active || v_timeout) {
                log_info("audio start cb");
                trigger_audio_start_cb(avsync, AV_SYNC_ASCB_OK);
            }
        }
    } else if (avsync->active_mode == AV_SYNC_MODE_PCR_MASTER) {
        struct session_debug debug;

        if (a_active && avsync->audio_start) {
            if (v_active || v_timeout) {
                log_info("audio start cb");
                trigger_audio_start_cb(avsync, AV_SYNC_ASCB_OK);
            }
        }

        if (!msync_session_get_debug_mode(avsync->fd, &debug)) {
            if (debug.debug_freerun && !avsync->debug_freerun) {
                avsync->backup_mode = avsync->mode;
                avsync->mode = AV_SYNC_MODE_FREE_RUN;
                avsync->debug_freerun = true;
                log_warn("[%d]audio to freerun mode", avsync->session_id);
            } else if (!debug.debug_freerun && avsync->debug_freerun) {
                avsync->mode = avsync->backup_mode;
                avsync->debug_freerun = false;
                log_warn("[%d]audio back to mode %d",
                        avsync->session_id, avsync->mode);
            }
        }
    } else if (avsync->active_mode == AV_SYNC_MODE_VMASTER) {
        log_info("[%d]running in vmaster mode", avsync->session_id);
        if (a_active && avsync->audio_start) {
            if (v_active || v_timeout) {
                log_info("audio start cb");
                trigger_audio_start_cb(avsync, AV_SYNC_ASCB_OK);
            }
        }

    }
}

static void handle_mode_change_v(struct av_sync_session* avsync,
    enum internal_sync_stat stat,
    bool v_active, bool a_active, bool v_timeout)
{
    struct session_debug debug;

    log_info("[%d]av_sync amode mode %d %d v/a %d/%d stat %d", avsync->session_id,
            avsync->active_mode, avsync->mode, v_active, a_active, stat);
    if (!msync_session_get_debug_mode(avsync->fd, &debug)) {
        if (debug.debug_freerun && !avsync->debug_freerun) {
            avsync->backup_mode = avsync->mode;
            avsync->mode = AV_SYNC_MODE_FREE_RUN;
            avsync->debug_freerun = true;
            log_warn("[%d]video to freerun mode", avsync->session_id);
        } else if (!debug.debug_freerun && avsync->debug_freerun) {
            avsync->mode = avsync->backup_mode;
            avsync->debug_freerun = false;
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
    int poll_timeout = 10;
    struct pollfd pfd = {
      /* default blocking capture */
      .events =  POLLPRI,
      .fd = avsync->fd,
    };
    enum src_flag sflag = SRC_A;

    log_info("[%d]enter", avsync->session_id);

    if (avsync->type == AV_SYNC_TYPE_AUDIO) {
        prctl (PR_SET_NAME, "avs_apoll");
        sflag = SRC_A;
    } else if (avsync->type == AV_SYNC_TYPE_VIDEO) {
        prctl (PR_SET_NAME, "avs_vpoll");
        sflag = SRC_V;
        poll_timeout = 100;
    }

    while (!avsync->quit_poll) {
        for (;;) {
          ret = poll(&pfd, 1, poll_timeout);
          if (ret > 0)
              break;
          if (avsync->quit_poll)
              goto exit;
          if (errno == EINTR) {
              log_debug("[%d] poll interrupted", avsync->session_id);
              continue;
          }
          if (errno == EAGAIN || errno == ENOMEM) {
              log_info("[%d] poll error %d", avsync->session_id, errno);
              continue;
          }
        }

        /* error handling */
        if (pfd.revents & POLLERR) {
            usleep(poll_timeout * 1000);
            continue;
        }

        if (pfd.revents & POLLNVAL) {
            log_warn("[%d] fd closed", avsync->session_id);
            goto exit;
        }
        /* mode change. Non-exclusive wait so all the processes
         * shall be woken up
         */
        if (pfd.revents & POLLPRI) {
            bool v_active, a_active, v_timeout;
            enum internal_sync_stat stat;

            msync_session_get_stat(fd, true, &avsync->active_mode, &stat,
                &v_active, &a_active, &v_timeout, &avsync->in_audio_switch, sflag);

            if (avsync->type == AV_SYNC_TYPE_AUDIO)
                handle_mode_change_a(avsync, stat, v_active, a_active, v_timeout);
            else if (avsync->type == AV_SYNC_TYPE_VIDEO)
                handle_mode_change_v(avsync, stat, v_active, a_active, v_timeout);
            usleep(10000);
        } else {
            log_debug("[%d] unexpected revents %x", avsync->session_id, pfd.revents);
            usleep(poll_timeout * 1000);
        }
    }
exit:
    log_info("[%d]quit", avsync->session_id);
    return NULL;
}

#define DEMOD_NODE "/sys/class/dtvdemod/atsc_para"
/* return ppm between demod and PCR clock */
int32_t static dmod_get_sfo_dev(struct av_sync_session *avsync)
{
    int fd = -1, ppm = 0, nread;
    char buf[128];
    uint32_t reg_v, lock;
    float val;

    fd = open(DEMOD_NODE, O_RDWR);
    if (fd < 0) {
        log_warn("node not found %s", DEMOD_NODE);
        /* do not retry */
        avsync->ppm_adjusted = true;
        return 0;
    }
    snprintf(buf, sizeof(buf), "%d", 5);
    write(fd, buf, 2);

    lseek(fd, 0, SEEK_SET);

    nread = read(fd, buf, sizeof(buf)-1);
    if (nread <= 0) {
        log_error("read error");
        goto err;
    }
    buf[nread] = 0;
    if (sscanf(buf, "ck=0x%x lock=%d", &reg_v, &lock) != 2) {
        log_error("wrong format %s", buf);
        goto err;
    }
    if (lock != 0x1f) {
        log_info("demod not locked");
        goto err;
    }
    if (reg_v > ((2 << 20) - 1))
        reg_v -= (2 << 21);
    val = reg_v * 10.762238f / 12 * 1000000 / (2 << 25);
    ppm = val;
    log_info("ppm from SFO %d", ppm);
    avsync->ppm_adjusted = true;

err:
    if (fd >= 0)
      close(fd);
    return ppm;
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

    /* initial estimation from Demod SFO HW */
    if (!avsync->ppm_adjusted) {
      ppm = dmod_get_sfo_dev(avsync);
      if (ppm != 0) {
          /* ppm > 0 means board clock is faster */
          msync_session_set_clock_dev(avsync->fd, -ppm);
      }
    }
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
            else
                avsync->ppm_adjusted = true;
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
    if (msync_session_get_stat(avsync->fd, false, &avsync->active_mode, NULL,
                &v_active, &a_active,
                &v_timeout, &avsync->in_audio_switch, SRC_A)) {
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
    if (msync_session_get_stat(avsync->fd, false, &avsync->active_mode, NULL,
                NULL, NULL, NULL, &avsync->in_audio_switch, SRC_A)) {
        log_error("[%d] can not audio seamless switch state",
                avsync->session_id);
        return -1;
    }
    if (start) *start =  avsync->in_audio_switch;
    return 0;
}

enum  clock_recovery_stat av_sync_get_clock_deviation(void *sync, int32_t *ppm)
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

static int video_mono_push_frame(struct av_sync_session *avsync, struct vframe *frame)
{
    int ret;

    if (!avsync->frame_q) {
        avsync->frame_q = create_q(MAX_FRAME_NUM);
        if (!avsync->frame_q) {
            log_error("[%d]create queue fail", avsync->session_id);

            return -1;
        }
    }

    ret = queue_item(avsync->frame_q, frame);
    if (ret)
        log_error("queue fail:%d", ret);
    log_debug("[%d]push %llu, QNum=%d", avsync->session_id, frame->mts, queue_size(avsync->frame_q));
    return ret;
}

int av_sync_set_vsync_mono_time(void *sync , uint64_t msys)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync)
        return -1;
    avsync->msys = msys;
    return 0;
}

static struct vframe * video_mono_pop_frame(struct av_sync_session *avsync)
{
    struct vframe *frame = NULL, *enter_last_frame = NULL;
    uint64_t systime;
    int toggle_cnt = 0;

    enter_last_frame = avsync->last_frame;
    systime = avsync->msys;
    log_debug("[%d]sys %llu", avsync->session_id, systime);
    while (!peek_item(avsync->frame_q, (void **)&frame, 0)) {
        if (systime >= frame->mts) {
            log_debug("[%d]cur_f %llu expire", avsync->session_id, frame->mts);
            toggle_cnt++;

            if (avsync->last_frame)
                avsync->last_holding_peroid = avsync->last_frame->hold_period;

            dqueue_item(avsync->frame_q, (void **)&frame);
            if (avsync->last_frame) {
                /* free frame that are not for display */
                if (toggle_cnt > 1) {
                    log_debug("[%d]free %llu cur %llu system %llu", avsync->session_id,
                             avsync->last_frame->mts, frame->mts, systime);
                    avsync->last_frame->free(avsync->last_frame);
                }
            } else {
                avsync->first_frame_toggled = true;
                log_info("[%d]first frame %llu", avsync->session_id, frame->mts);
            }
            avsync->last_frame = frame;
        } else
            break;
    }

    if (avsync->last_frame) {
        if (enter_last_frame != avsync->last_frame)
            log_debug("[%d]pop %lu", avsync->session_id, avsync->last_frame->pts);
        log_trace("[%d]pop=%llu, system=%llu, diff %llu(ms), QNum=%d", avsync->session_id,
                avsync->last_frame->mts,
                systime, (systime - avsync->last_frame->mts) / 1000000,
                queue_size(avsync->frame_q));
    } else
        if (enter_last_frame != avsync->last_frame)
            log_debug("[%d]pop (nil)", avsync->session_id);

    if (avsync->last_frame)
        avsync->last_frame->hold_period++;
    return avsync->last_frame;
}

int avs_sync_stop_audio(void *sync)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync)
        return -1;

    return msync_session_stop_audio(avsync->fd);
}

int avs_sync_set_eos(void *sync)
{
    struct av_sync_session *avsync = (struct av_sync_session *)sync;

    if (!avsync)
        return -1;

    if (avsync->type == AV_SYNC_TYPE_VIDEO) {
        if (avsync->state == AV_SYNC_STAT_INIT) {
            avsync->state = AV_SYNC_STAT_RUNNING;
            log_debug("[%d]eos trigger state change: init --> running", avsync->session_id);
        }
    }

    return 0;
}
