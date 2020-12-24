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

static struct vframe frame[FRAME_NUM];
static int frame_received;

#if 0
static int sysfs_set_sysfs_str(const char *path, const char *val)
{
    int fd;
    fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd >= 0) {
        if(write(fd, val, strlen(val)) != strlen(val))
            log_error("write fail");
        close(fd);
        return 0;
    } else {
        log_error("test: unable to open file %s,err: %s", path, strerror(errno));
    }
    return -1;
}
#endif

static void frame_free(struct vframe * frame)
{
    log_info("free %d\n", (int)frame->private);
    frame_received++;
}

static void test(int refresh_rate, int pts_interval, struct vframe* frame)
{
    int i = 0;
    void* handle;
    int sleep_us = 1000000/refresh_rate;
    struct vframe *last_frame = NULL, *pop_frame;

    handle = av_sync_create(0, AV_SYNC_MODE_VMASTER, 2, 2, 90000/refresh_rate);
    frame = (struct vframe*)calloc(FRAME_NUM, sizeof(*frame));
    if (!frame) {
        log_error("oom");
        exit(1);
    }

    /* push max frames */
    while (i < FRAME_NUM) {
        frame[i].private = (void *)i;
        frame[i].pts = PATTERN_22_DURATION * i;
        frame[i].duration = PATTERN_22_DURATION;
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
}

int main(int argc, const char** argv)
{
    log_set_level(LOG_TRACE);

    log_info("\n----------------22 start------------\n");
    test(60, PATTERN_22_DURATION, frame);
    log_info("\n----------------22 end--------------\n");
    log_info("\n----------------32 start------------\n");
    test(60, PATTERN_32_DURATION, frame);
    log_info("\n----------------32 start------------\n");
    log_info("\n----------------41 start------------\n");
    test(30, PATTERN_32_DURATION, frame);
    log_info("\n----------------41 end--------------\n");
    log_info("\n----------------11 start------------\n");
    test(30, PATTERN_22_DURATION, frame);
    log_info("\n----------------11 end--------------\n");

    return 0;
}
