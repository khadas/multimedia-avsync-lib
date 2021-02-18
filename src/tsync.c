/*
 * Copyright (c) 2020 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description: tsync warrper. Single instance ONLY. Session will be ignored
 * Author: song.zhao@amlogic.com
 */

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include "tsync.h"
#include "aml_avsync_log.h"

#define VIDEO_DEVICE "/dev/amvideo"
#define TSYNC_ENABLE "/sys/class/tsync/enable"
#define TSYNC_PCRSCR "/sys/class/tsync/pts_pcrscr"
#define TSYNC_EVENT  "/sys/class/tsync/event"
#define TSYNC_MODE   "/sys/class/tsync/mode"
#define TSYNC_VPTS   "/sys/class/tsync/pts_video"

#define _A_M 'S'
#define AMSTREAM_IOC_SYNCTHRESH _IOW((_A_M), 0x19, int)
#define AMSTREAM_IOC_SET_VSYNC_UPINT _IOW((_A_M), 0x89, int)
#define AMSTREAM_IOC_SET_VIDEOPEEK   _IOW(_A_M, 0xbf, unsigned int)
#define AMSTREAM_IOC_SET_NO_VIDEO_STOP _IOW(_A_M, 0xf5, unsigned int)
#define AMSTREAM_IOC_SET_VSYNC_SLOW_FACTOR _IOW((_A_M), 0x8b, int)

static int config_sys_node(const char* path, const char* value)
{
    int fd;
    fd = open(path, O_RDWR);
    if (fd < 0) {
        log_error("fail to open %s\n", path);
        return -1;
    }
    if (write(fd, value, strlen(value)) != strlen(value)) {
        log_error("fail to write %s to %s\n", value, path);
        close(fd);
        return -1;
    }
    close(fd);

    return 0;
}

static int get_sysfs_uint32(const char *path, uint32_t *value)
{
    int fd;
    char valstr[64];
    uint32_t val = 0;

    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        memset(valstr, 0, 64);
        read(fd, valstr, 64 - 1);
        valstr[strlen(valstr)] = '\0';
        close(fd);
    } else {
        log_error("unable to open file %s\n", path);
        return -1;
    }
    if (sscanf(valstr, "0x%x", &val) < 1) {
        log_error("unable to get pts from: %s", valstr);
        return -1;
    }
    *value = val;
    return 0;
}

int tsync_enable(int session, bool enable)
{
    const char *val;

    if (enable)
        val = "1";
    else
        val = "0";
    log_info("%s", val);
    return config_sys_node(TSYNC_ENABLE, val);
}

uint32_t tsync_get_pcr(int session)
{
    uint32_t pcr = 0;

    get_sysfs_uint32(TSYNC_PCRSCR, &pcr);
    return pcr;
}

//uint32_t tsync_get_vpts(int session);

int tsync_send_video_start(int session, uint32_t vpts)
{
    char val[50];

    snprintf(val, sizeof(val), "VIDEO_START:0x%x", vpts);
    log_info("%s", val);
    return config_sys_node(TSYNC_EVENT, val);
}

int tsync_send_video_stop(int session)
{
    log_info("%s");
    return config_sys_node(TSYNC_EVENT, "VIDEO_STOP");
}

int tsync_send_video_pause(int session, bool pause)
{
    const char *val;

    if (pause)
        val = "VIDEO_PAUSE:0x1";
    else
        val = "VIDEO_PAUSE:0x0";
    log_info("%s", val);
    return config_sys_node(TSYNC_EVENT, val);
}

int tsync_send_video_disc(int session, uint32_t vpts)
{
    char val[50];

    snprintf(val, sizeof(val), "VIDEO_TSTAMP_DISCONTINUITY:0x%x", vpts);
    log_info("%s", val);
    return config_sys_node(TSYNC_EVENT, val);
}

int tsync_set_pcr(int session, uint32_t pcr)
{
    char val[20];

    snprintf(val, sizeof(val), "%u", pcr);
    config_sys_node(TSYNC_PCRSCR, val);
    return 0;
}

static int video_device_ioctl(int ctl, int value)
{
    int video_fd;

    video_fd = open(VIDEO_DEVICE, O_RDWR);
    if (video_fd < 0) {
        return -1;
    }

    ioctl(video_fd, ctl, value);

    close(video_fd);

    return 0;
}

int tsync_set_pts_inc_mode(int session, bool enable)
{
    return video_device_ioctl(AMSTREAM_IOC_SET_VSYNC_UPINT, enable);
}

int tsync_set_video_sync_thres(int session, bool enable)
{
    return video_device_ioctl(AMSTREAM_IOC_SYNCTHRESH, enable);
}

int tsync_set_mode(int session, enum sync_mode mode)
{
    const char* val = NULL;
    if (mode == AV_SYNC_MODE_VMASTER)
        val = "0";
    else if (mode == AV_SYNC_MODE_AMASTER)
        val = "1";
    else if (mode == AV_SYNC_MODE_PCR_MASTER)
        val = "2";
    return config_sys_node(TSYNC_MODE, val);
}

/* do not send VIDEO_START from video.c */
int tsync_set_video_peek_mode(int session)
{
    log_info("set video peek");
    return video_device_ioctl(AMSTREAM_IOC_SET_VIDEOPEEK, 0);
}

/* Control VIDEO_STOP from video.c */
int tsync_disable_video_stop_event(int session, bool disable)
{
    log_info("no_video_stop %d", disable);
    return video_device_ioctl(AMSTREAM_IOC_SET_NO_VIDEO_STOP, disable);
}

int tsync_set_speed(int session, float speed)
{
    if (speed == 1.0f)
        return video_device_ioctl(AMSTREAM_IOC_SET_VSYNC_SLOW_FACTOR, 1);

    return video_device_ioctl(AMSTREAM_IOC_SET_VSYNC_SLOW_FACTOR, 1000000 * speed);
}

int tsync_set_vpts(int session, pts90K pts)
{
    char val[50];

    snprintf(val, sizeof(val), "0x%x", pts);
    return config_sys_node(TSYNC_VPTS, val);
}
