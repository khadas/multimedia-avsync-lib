/*
 * Copyright (c) 2020 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description: test for avsync
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

#include "aml_avsync.h"
#include "aml_avsync_log.h"

#define FRAME_NUM 32
#define PATTERN_32_DURATION 3750
#define PATTERN_22_DURATION 3000
#define AUDIO_FRAME_DURATION 1800 //20ms
#define AUDIO_DELAY 5400 //60ms
#define PTS_START 0x12345678

static int frame_received;
static bool audio_can_start;

static int audio_start(void *priv, avs_ascb_reason reason)
{
    log_info("received");
    audio_can_start = true;
    return 0;
}

static void test_a(bool sync)
{
    int i = 0;
    void *handle = NULL, *attach_handle = NULL;
    int session, session_id;
    avs_start_ret ret;
    int adjust = 0;

    session = av_sync_open_session(&session_id);
    if (session < 0) {
        log_error("open fail");
        exit(1);
    }

    handle = av_sync_create(session_id, AV_SYNC_MODE_AMASTER, AV_SYNC_TYPE_AUDIO, 0);
    if (!handle) {
        log_error("create fail");
        goto exit;
    }

    if (!sync) {
        /* let it timeout */
        ret = avs_sync_set_start_policy(handle, AV_SYNC_START_ALIGN);
        if (ret) {
            log_error("policy fail");
            goto exit2;
        }
    }

    attach_handle = av_sync_attach(session_id, AV_SYNC_TYPE_AUDIO);
    if (!attach_handle) {
        log_error("attach fail");
        goto exit;
    }

    ret = av_sync_audio_start(attach_handle, PTS_START, 4500, audio_start, NULL);

    if (ret == AV_SYNC_ASTART_ASYNC) {
        log_info("begin wait audio start");
        while (!audio_can_start)
            usleep(10000);
        log_info("finish wait audio start");
    }

    //3s, asusme each package is 10ms
    for (i = 0 ; i < 300 ; i++) {
        int ret;
        struct audio_policy policy;

        ret = av_sync_audio_render(attach_handle, adjust + PTS_START + i * 900, &policy);
        if (ret) {
            log_error("fail");
            goto exit3;
        }

        switch (policy.action) {
            case AV_SYNC_AA_RENDER:
                log_info("render %d ms", i * 10);
                usleep(10 * 1000);
                break;
            case AV_SYNC_AA_DROP:
                log_info("drop %d ms @ %d ms", policy.delta/90, i * 10);
                adjust += policy.delta;
                break;
            case AV_SYNC_AA_INSERT:
                log_info("insert %d ms @ %d ms", -policy.delta/90, i * 10);
                usleep(-policy.delta/90 * 1000 + 10 * 1000);
                break;
            default:
                log_error("should not happen");
                break;
        }
    }

exit3:
    av_sync_destroy(attach_handle);
exit2:
    av_sync_destroy(handle);
exit:
    av_sync_close_session(session);
}

static void frame_free(struct vframe * frame)
{
    log_info("free %d\n", (int)frame->private);
    frame_received++;
}

static void test_v(int refresh_rate, int pts_interval)
{
    struct vframe* frame;
    int i = 0, rc;
    void* handle;
    int sleep_us = 1000000/refresh_rate;
    struct vframe *last_frame = NULL, *pop_frame;
    int session, session_id;
    struct video_config config;

    session = av_sync_open_session(&session_id);
    if (session < 0) {
        log_error("alloc fail");
        exit(1);
    }
    handle = av_sync_create(session_id, AV_SYNC_MODE_VMASTER, AV_SYNC_TYPE_VIDEO, 2);
    if (handle == 0) {
        log_error("create fail");
        av_sync_close_session(session);
        exit(1);
    }

    config.delay = 2;
    rc = av_sync_video_config(handle, &config);
    if (rc) {
        log_error("config fail");
        av_sync_close_session(session_id);
        exit(1);
    }

    frame = (struct vframe*)calloc(FRAME_NUM, sizeof(*frame));
    if (!frame) {
        log_error("oom");
        exit(1);
    }

    /* push max frames */
    while (i < FRAME_NUM) {
        frame[i].private = (void *)i;
        frame[i].pts = pts_interval * i;
        frame[i].duration = pts_interval;
        frame[i].free = frame_free;
        if (av_sync_push_frame(handle, &frame[i])) {
            log_error("queue %d fail", i);
            break;
        }
        log_info("queue %d", i);
        i++;
    }

    i = 0;
    while (frame_received < FRAME_NUM) {
        usleep(sleep_us);
        pop_frame = av_sync_pop_frame(handle);
        if (pop_frame)
            log_info("pop frame %02d", (int)pop_frame->private);
        if (pop_frame != last_frame) {
            i++;
            last_frame = pop_frame;
            frame_received++;
        }
    }

    frame_received = 0;
    av_sync_destroy(handle);
    av_sync_close_session(session);
    free(frame);
}

int main(int argc, const char** argv)
{
    log_set_level(LOG_TRACE);
    int test_case = 0;

    if (argc == 2)
        test_case = atoi(argv[1]);

    if (test_case == 0) {
        log_info("\n----------------22 start------------\n");
        test_v(60, PATTERN_22_DURATION);
        log_info("\n----------------22 end--------------\n");
        log_info("\n----------------32 start------------\n");
        test_v(60, PATTERN_32_DURATION);
        log_info("\n----------------32 start------------\n");
        log_info("\n----------------41 start------------\n");
        test_v(30, PATTERN_32_DURATION);
        log_info("\n----------------41 end--------------\n");
        log_info("\n----------------11 start------------\n");
        test_v(30, PATTERN_22_DURATION);
        log_info("\n----------------11 end--------------\n");
    } else if (test_case == 1) {
        log_info("\n----------------audio start------------\n");
        test_a(true);
        log_info("\n----------------audio end--------------\n");
    } else if (test_case == 2) {
        log_info("\n----------------audio async start------------\n");
        test_a(false);
        log_info("\n----------------audio end--------------\n");
    }

    return 0;
}
