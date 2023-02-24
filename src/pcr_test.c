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
 * Description: test for pcr master mode
 *
 * Author: song.zhao@amlogic.com
 */
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "aml_avsync.h"
#include "aml_avsync_log.h"

#define FRAME_NUM 32
#define PATTERN_32_DURATION 3750
#define PATTERN_22_DURATION 3000
#define REFRESH_RATE 60
#define PTS_START 0x12345678

static struct vframe *frame;
static int frame_received;
void *v_h, *a_h, *pcr_h;
static pthread_t vfeed_t;
static pthread_t afeed_t;
static bool quit_a_thread;
static pthread_t pcr_feed_t;
static bool quit_pcr_thread;
static bool audio_can_start;
static int pts_interval = PATTERN_32_DURATION;

static void frame_free(struct vframe * frame)
{
    log_info("free %d\n", (int)frame->private);
    frame_received++;
}


static void * v_thread(void * arg)
{
    int i = 0;
    int sleep_us = 1000000/REFRESH_RATE;
    struct vframe *last_frame = NULL, *pop_frame;

    /* push max frames */
    while (i < FRAME_NUM) {
        frame[i].private = (void *)i;
        frame[i].pts = PTS_START + pts_interval * i;
        frame[i].duration = pts_interval;
        frame[i].free = frame_free;
        if (av_sync_push_frame(v_h, &frame[i])) {
            log_error("queue %d fail", i);
            break;
        }
        log_info("queue %d", i);
        usleep(10000);
        i++;
    }

    i = 0;
    while (frame_received < FRAME_NUM) {
        usleep(sleep_us);
        pop_frame = av_sync_pop_frame(v_h);
        if (pop_frame)
            log_info("pop frame %02d", (int)pop_frame->private);
        if (pop_frame != last_frame) {
            i++;
            last_frame = pop_frame;
            frame_received++;
            log_info("frame received %d", frame_received);
        }
    }

    return NULL;
}

static int start_v_thread()
{
    int ret;

    frame = (struct vframe*)calloc(FRAME_NUM, sizeof(struct vframe));
    if (!frame) {
        log_error("oom");
        exit(1);
    }

    ret = pthread_create(&vfeed_t, NULL, v_thread, NULL);
    if (ret) {
        log_error("fail");
        exit(1);
    }
    return 0;
}

static void stop_v_thread()
{
    pthread_join(vfeed_t, NULL);
    free(frame);
}

static int audio_start(void *priv, avs_ascb_reason reason)
{
    log_info("received");
    audio_can_start = true;
    return 0;
}

static void * a_thread(void * arg)
{
    int i = 0;
    avs_start_ret ret;
    int adjust = 0;

    ret = av_sync_audio_start(a_h, PTS_START, 4500, audio_start, NULL);

    if (ret == AV_SYNC_ASTART_ASYNC) {
        log_info("begin wait audio start");
        while (!audio_can_start)
            usleep(10000);
        log_info("finish wait audio start");
    }

    //5s, assume each package is 10ms
    while (!quit_a_thread) {
        int ret;
        struct audio_policy policy;

        ret = av_sync_audio_render(a_h, adjust + PTS_START + i * 900, &policy);
        if (ret) {
            log_error("fail");
            return NULL;
        }

        switch (policy.action) {
            case AV_SYNC_AA_RENDER:
                log_info("render 10 ms %x", PTS_START + i * 900);
                usleep(10 * 1000);
                break;
            case AV_SYNC_AA_DROP:
                log_info("drop %d ms", policy.delta/90);
                adjust += policy.delta;
                break;
            case AV_SYNC_AA_INSERT:
                log_info("insert %d ms", (-policy.delta)/90);
                usleep(-policy.delta/90 * 1000 + 10 * 1000);
                break;
            default:
                log_error("should not happen");
                break;
        }
	i++;
    }
    return NULL;
}

static int start_a_thread()
{
    int ret;

    ret = pthread_create(&afeed_t, NULL, a_thread, NULL);
    if (ret) {
        log_error("fail");
        exit(1);
    }
    return 0;
}

static void stop_a_thread()
{
    quit_a_thread = true;
    pthread_join(afeed_t, NULL);
}

static void * pcr_thread(void * arg)
{
    int i = 0;
    //5s, 50ms interval
    while (!quit_pcr_thread) {
        int ret;

        ret = av_sync_set_pcr_clock(pcr_h, PTS_START + i * 4500, i * 50000000);
        if (ret)
            log_error("fail");
        log_debug("pcr is %x", PTS_START + i * 4500);
        usleep(50000);
        i++;
    }
    return NULL;
}

static int start_pcr_thread()
{
    int ret;

    ret = pthread_create(&pcr_feed_t, NULL, pcr_thread, NULL);
    if (ret) {
        log_error("fail");
        exit(1);
    }
    return 0;
}

static void stop_pcr_thread()
{
    quit_pcr_thread = true;
    pthread_join(pcr_feed_t, NULL);
}

static void test()
{
    int i = 0;
    int session, session_id;

    session = av_sync_open_session(&session_id);
    if (session < 0) {
        log_error("fail");
        exit(1);
    }
    v_h = av_sync_create(session_id, AV_SYNC_MODE_PCR_MASTER, AV_SYNC_TYPE_VIDEO, 2);
    if (!v_h) {
        log_error("fail");
        exit(1);
    }
    a_h = av_sync_create(session_id, AV_SYNC_MODE_PCR_MASTER, AV_SYNC_TYPE_AUDIO, 0);
    if (!a_h) {
        log_error("fail");
        exit(1);
    }
    pcr_h = av_sync_create(session_id, AV_SYNC_MODE_PCR_MASTER, AV_SYNC_TYPE_PCR, 0);
    if (!pcr_h) {
        log_error("fail");
        exit(1);
    }
    start_v_thread();
    start_a_thread();
    start_pcr_thread();

    //wait for 5s
    while (i < 100) {
        usleep(50 * 1000);
        i++;
    }

    stop_v_thread();
    stop_a_thread();
    stop_pcr_thread();
    av_sync_destroy(v_h);
    av_sync_destroy(a_h);
    av_sync_destroy(pcr_h);
    av_sync_close_session(session);
}

int main(int argc, const char** argv)
{
    log_set_level(LOG_TRACE);

    log_info("\n----------------start------------\n");
    test();
    log_info("\n----------------end--------------\n");

    return 0;
}
