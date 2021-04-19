/*
 * Copyright (c) 2020 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 * User space AV sync module.
 *
 * Author: song.zhao@amlogic.com
 */
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/types.h>
//#include <linux/amlogic/msync.h>
#include "msync.h"
#include "aml_avsync_log.h"
#include "msync_util.h"

#define MSYNC_DEV "/dev/aml_msync"

int msync_create_session()
{
    int fd;

    fd = open(MSYNC_DEV, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        log_error("%s errno:%d", MSYNC_DEV, errno);
        return -1;
    }
    return fd;
}

void msync_destory_session(int fd)
{
    close(fd);
}

int msync_session_set_mode(int fd, enum sync_mode mode)
{
    int rc;
    uint32_t kmode;

    if (mode == AV_SYNC_MODE_VMASTER)
        kmode = AVS_MODE_V_MASTER;
    else if (mode == AV_SYNC_MODE_AMASTER)
        kmode = AVS_MODE_A_MASTER;
    else if (mode == AV_SYNC_MODE_PCR_MASTER)
        kmode = AVS_MODE_PCR_MASTER;
    else if (mode == AV_SYNC_MODE_IPTV)
        kmode = AVS_MODE_IPTV;
    else if (mode == AV_SYNC_MODE_FREE_RUN)
        kmode = AVS_MODE_FREE_RUN;

    rc = ioctl(fd, AMSYNCS_IOC_SET_MODE, &kmode);
    if (rc)
        log_error("session[%d] set mode errno:%d", fd, errno);
    return rc;
}

int msync_session_get_mode(int fd, enum sync_mode *mode)
{
    int rc;
    uint32_t kmode;

    rc = ioctl(fd, AMSYNCS_IOC_GET_MODE, &kmode);
    if (rc) {
        log_error("session[%d] set mode errno:%d", fd, errno);
        return rc;
    }

    if (kmode == AVS_MODE_V_MASTER)
        *mode = AV_SYNC_MODE_VMASTER;
    else if (kmode == AVS_MODE_A_MASTER)
        *mode = AV_SYNC_MODE_AMASTER;
    else if (kmode == AVS_MODE_PCR_MASTER)
        *mode = AV_SYNC_MODE_PCR_MASTER;
    else if (kmode == AVS_MODE_IPTV)
        *mode = AV_SYNC_MODE_IPTV;
    else if (kmode == AVS_MODE_FREE_RUN)
        *mode = AV_SYNC_MODE_FREE_RUN;

    return rc;
}

int msync_session_get_start_policy(int fd, uint32_t *policy)
{
    int rc;
    uint32_t kpolicy;

    rc = ioctl(fd, AMSYNCS_IOC_GET_START_POLICY, &kpolicy);
    if (rc)
        log_error("session[%d] get start policy errno:%d", fd, errno);

    if (kpolicy == AMSYNC_START_V_FIRST)
        *policy = AV_SYNC_START_V_FIRST;
    else if (kpolicy == AMSYNC_START_A_FIRST)
        *policy = AV_SYNC_START_A_FIRST;
    else if (kpolicy == AMSYNC_START_ASAP)
        *policy = AV_SYNC_START_ASAP;
    else if (kpolicy == AMSYNC_START_ALIGN)
        *policy = AV_SYNC_START_ALIGN;
    else
        *policy = AV_SYNC_START_NONE;
    return rc;
}
int msync_session_set_start_policy(int fd, uint32_t policy)
{
    int rc;
    uint32_t kpolicy;

    if (policy == AV_SYNC_START_V_FIRST)
        kpolicy = AMSYNC_START_V_FIRST;
    else if (policy == AV_SYNC_START_A_FIRST)
        kpolicy = AMSYNC_START_A_FIRST;
    else if (policy == AV_SYNC_START_ASAP)
        kpolicy = AMSYNC_START_ASAP;
    else if (policy == AV_SYNC_START_ALIGN)
        kpolicy = AMSYNC_START_ALIGN;
    else
        return -1;

    rc = ioctl(fd, AMSYNCS_IOC_SET_START_POLICY, &kpolicy);
    if (rc)
        log_error("session[%d] set start policy errno:%d", fd, errno);
    return rc;
}

static int msync_session_set_event(int fd, enum avs_event event, uint32_t value)
{
    struct session_event sevent;
    int rc;

    sevent.event = event;
    sevent.value = value;

    rc = ioctl(fd, AMSYNCS_IOC_SEND_EVENT, &sevent);
    if (rc)
        log_error("session[%d] send %d errno:%d", fd, event, errno);
    return rc;
}


int msync_session_set_pause(int fd, bool pause)
{
    if (pause)
        return msync_session_set_event(fd, AVS_PAUSE, 0);
    else
        return msync_session_set_event(fd, AVS_RESUME, 0);
}

int msync_session_get_wall(int fd, uint32_t *wall, uint32_t *interval)
{
    int rc;
    struct pts_wall pwall;

    rc = ioctl(fd, AMSYNCS_IOC_GET_WALL, &pwall);
    if (rc)
        log_error("session[%d] get wall errno:%d", fd, errno);

    *wall = pwall.wall_clock;
    if (interval)
        *interval = pwall.interval;
    return rc;
}

int msync_session_set_video_start(int fd, pts90K pts)
{
    return msync_session_set_event(fd, AVS_VIDEO_START, pts);
}

int msync_session_set_audio_start(int fd, pts90K pts, pts90K delay, uint32_t *mode)
{
    struct audio_start start;
    int rc;

    if (!mode)
        return -EINVAL;

    start.pts = pts;
    start.delay = delay;
    start.mode = 0;

    rc = ioctl(fd, AMSYNCS_IOC_AUDIO_START, &start);
    if (rc)
        log_error("session[%d] audio start errno:%d", fd, errno);
    else
        *mode = start.mode;

    return rc;
}

int msync_session_set_video_dis(int fd, pts90K pts)
{
    return msync_session_set_event(fd, AVS_VIDEO_TSTAMP_DISCONTINUITY, pts);
}

int msync_session_set_audio_dis(int fd, pts90K pts)
{
    return msync_session_set_event(fd, AVS_AUDIO_TSTAMP_DISCONTINUITY, pts);
}

int msync_session_set_rate(int fd, float speed)
{
    uint32_t krate = 1000*speed;
    int rc;


    rc = ioctl(fd, AMSYNCS_IOC_SET_RATE, &krate);
    if (rc)
        log_error("fd[%d] set rate errno:%d", fd, errno);
    return rc;
}

int msync_session_get_rate(int fd, float *speed)
{
    uint32_t krate;
    int rc;


    rc = ioctl(fd, AMSYNCS_IOC_GET_RATE, &krate);
    if (rc) {
        log_error("fd[%d] get rate errno:%d", fd, errno);
        return rc;
    }
    *speed = (float)krate/1000;
    return 0;
}

int msync_session_set_name(int fd, const char* name)
{
    int rc;

    rc = ioctl(fd, AMSYNCS_IOC_SET_NAME, name);
    if (rc)
        log_error("session[%d] set name errno:%d", fd, errno);
    return rc;
}

int msync_session_update_vpts(int fd, uint32_t system, uint32_t pts, uint32_t delay)
{
    int rc;
    struct pts_tri ts;

    ts.wall_clock = system;
    ts.pts = pts;
    ts.delay = delay;

    rc = ioctl(fd, AMSYNCS_IOC_SET_V_TS, &ts);
    if (rc)
        log_error("session[%d] set vts errno:%d", fd, errno);
    return rc;
}

int msync_session_update_apts(int fd, uint32_t system, uint32_t pts, uint32_t delay)
{
    int rc;
    struct pts_tri ts;

    ts.wall_clock = system;
    ts.pts = pts;
    ts.delay = delay;

    rc = ioctl(fd, AMSYNCS_IOC_SET_A_TS, &ts);
    if (rc)
        log_error("session[%d] set ats errno:%d", fd, errno);
    return rc;
}

int msync_session_set_audio_stop(int fd)
{
    return msync_session_set_event(fd, AVS_AUDIO_STOP, 0);
}

int msync_session_set_video_stop(int fd)
{
    return msync_session_set_event(fd, AVS_VIDEO_STOP, 0);
}

int msync_session_get_stat (int fd, enum sync_mode *mode,
        bool *v_active, bool *a_active, bool *v_timeout)
{
    int rc;
    struct session_sync_stat stat;

    rc = ioctl(fd, AMSYNCS_IOC_GET_SYNC_STAT, &stat);
    if (rc) {
        log_error("fd[%d] get state errno:%d", fd, errno);
        return rc;
    }

    switch (stat.mode) {
    case AVS_MODE_A_MASTER:
        *mode = AV_SYNC_MODE_AMASTER;
        break;
    case AVS_MODE_V_MASTER:
        *mode = AV_SYNC_MODE_VMASTER;
        break;
    case AVS_MODE_PCR_MASTER:
        *mode = AV_SYNC_MODE_PCR_MASTER;
        break;
    case AVS_MODE_IPTV:
        *mode = AV_SYNC_MODE_IPTV;
        break;
    case AVS_MODE_FREE_RUN:
        *mode = AV_SYNC_MODE_FREE_RUN;
        break;
    }
    if (v_active)
        *v_active = stat.v_active;
    if (a_active)
        *a_active = stat.a_active;
    if (v_timeout)
        *v_timeout = stat.v_timeout;

    return rc;
}

bool msync_clock_started(int fd)
{
    uint32_t start = 0;
    int rc;


    rc = ioctl(fd, AMSYNCS_IOC_GET_CLOCK_START, &start);
    if (rc)
        log_error("session[%d] set clock start errno:%d", fd, errno);
    return start != 0;
}

int msync_session_set_pcr(int fd, pts90K pts)
{
    int rc;
    uint32_t pcr = pts;

    rc = ioctl(fd, AMSYNCS_IOC_SET_PCR, &pcr);
    if (rc)
        log_error("session[%d] set pcr %u errno:%d", fd, pcr, errno);

    return rc;
}

int msync_session_get_pcr(int fd, pts90K *pts)
{
    int rc;
    uint32_t pcr;

    rc = ioctl(fd, AMSYNCS_IOC_GET_PCR, &pcr);
    if (rc)
        log_error("session[%d] set pcr %u errno:%d", fd, pcr, errno);
    else
        *pts = pcr;

    return rc;
}