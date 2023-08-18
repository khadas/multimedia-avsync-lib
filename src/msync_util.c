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
#include <string.h>
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

int msync_session_get_start_policy(int fd, uint32_t *policy, int *timeout)
{
    int rc;
    struct ker_start_policy kpolicy;

    rc = ioctl(fd, AMSYNCS_IOC_GET_START_POLICY, &kpolicy);
    if (rc)
        log_error("session[%d] get start policy errno:%d", fd, errno);

    if (kpolicy.policy == AMSYNC_START_V_FIRST)
        *policy = AV_SYNC_START_V_FIRST;
    else if (kpolicy.policy == AMSYNC_START_A_FIRST)
        *policy = AV_SYNC_START_A_FIRST;
    else if (kpolicy.policy == AMSYNC_START_ASAP)
        *policy = AV_SYNC_START_ASAP;
    else if (kpolicy.policy == AMSYNC_START_ALIGN)
        *policy = AV_SYNC_START_ALIGN;
    else
        *policy = AV_SYNC_START_NONE;

    if (0 == rc)
        *timeout = kpolicy.timeout;

    return rc;
}
int msync_session_set_start_policy(int fd, uint32_t policy, int timeout)
{
    int rc;
    struct ker_start_policy kpolicy;

    if (policy == AV_SYNC_START_V_FIRST)
        kpolicy.policy = AMSYNC_START_V_FIRST;
    else if (policy == AV_SYNC_START_A_FIRST)
        kpolicy.policy = AMSYNC_START_A_FIRST;
    else if (policy == AV_SYNC_START_ASAP)
        kpolicy.policy = AMSYNC_START_ASAP;
    else if (policy == AV_SYNC_START_ALIGN)
        kpolicy.policy = AMSYNC_START_ALIGN;
    else
        return -1;

    kpolicy.timeout = timeout;

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

int msync_session_get_pts(int fd, pts90K *p_pts, uint64_t *mono_ts, bool is_video)
{
    int rc;
    struct pts_tri pts;

    if (is_video)
        rc = ioctl(fd, AMSYNCS_IOC_GET_V_TS, &pts);
    else
        rc = ioctl(fd, AMSYNCS_IOC_GET_A_TS, &pts);

    if (rc) {
        log_error("session[%d] get ts errno:%d", fd, errno);
        return rc;
    }

    if (p_pts)
        *p_pts = pts.pts;
    if (mono_ts)
        *mono_ts = pts.mono_ts;
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
    struct timespec now;
    uint64_t mono_ns;

    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    mono_ns = now.tv_sec * 1000000000LL + now.tv_nsec;
    mono_ns += delay / 9 * 100000;

    ts.wall_clock = system;
    ts.pts = pts;
    ts.delay = delay;
    ts.mono_ts = mono_ns;

    rc = ioctl(fd, AMSYNCS_IOC_SET_V_TS, &ts);
    if (rc)
        log_error("session[%d] set vts errno:%d", fd, errno);
    return rc;
}

int msync_session_update_apts(int fd, uint32_t system, uint32_t pts, uint32_t delay)
{
    int rc;
    struct pts_tri ts;
    struct timespec now;
    uint64_t mono_ns;

    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    mono_ns = now.tv_sec * 1000000000LL + now.tv_nsec;
    mono_ns += delay / 9 * 100000;

    ts.wall_clock = system;
    ts.pts = pts;
    ts.delay = delay;
    ts.mono_ts = mono_ns;

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

int msync_session_get_stat (int fd,
        bool clean_poll, enum sync_mode *mode,
        enum internal_sync_stat *state,
        bool *v_active, bool *a_active, bool *v_timeout,
        bool *a_switch, enum src_flag flag)
{
    int rc;
    struct session_sync_stat stat;

    memset(&stat, 0, sizeof(stat));
    stat.flag = flag;
    stat.clean_poll = clean_poll;
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
    if (a_switch)
        *a_switch = stat.audio_switch;
    if (state)
        *state = stat.stat;
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

int msync_session_set_pcr(int fd, pts90K pts, uint64_t mono_clock)
{
    int rc;
    struct pcr_pair pcr;

    pcr.pts = pts;
    pcr.mono_clock = mono_clock;
    rc = ioctl(fd, AMSYNCS_IOC_SET_PCR, &pcr);
    if (rc)
        log_error("session[%d] set pcr.pts %u errno:%d", fd, pcr.pts, errno);

    return rc;
}

int msync_session_get_pcr(int fd, pts90K *pts, uint64_t *mono_clock)
{
    int rc;
    struct pcr_pair pcr;

    rc = ioctl(fd, AMSYNCS_IOC_GET_PCR, &pcr);
    if (rc)
        log_error("session[%d] get pcr.pts %u errno:%d", fd, pcr.pts, errno);
    else {
        *pts = pcr.pts;
        *mono_clock = pcr.mono_clock;
    }

    return rc;
}

int msync_session_get_debug_mode(int fd, struct session_debug *debug)
{
    int rc;

    rc = ioctl(fd, AMSYNCS_IOC_GET_DEBUG_MODE, debug);
    if (rc)
        log_error("session[%d] set debug mode errno:%d", fd, errno);

    return rc;
}

int msync_session_set_audio_switch(int fd, bool start)
{
    log_info("audio switch set to %d", start);
    return msync_session_set_event(fd, AVS_AUDIO_SWITCH, start);
}

int msync_session_set_clock_dev(int fd, int32_t ppm)
{
    int rc;

    rc = ioctl(fd, AMSYNCS_IOC_SET_CLK_DEV, &ppm);
    if (rc)
        log_error("session[%d] set clk dev errno:%d", fd, errno);
    return rc;
}

int msync_session_get_clock_dev(int fd, int32_t *ppm)
{
    int rc;
    int dev;

    rc = ioctl(fd, AMSYNCS_IOC_GET_CLK_DEV, &dev);
    if (rc)
        log_error("session[%d] get clk dev errno:%d", fd, errno);
    else
        *ppm = dev;
    return rc;
}

int msync_session_set_wall_adj_thres(int fd, int32_t thres)
{
    int rc;

    rc = ioctl(fd, AMSYNCS_IOC_SET_WALL_ADJ_THRES, &thres);
    if (rc)
        log_error("session[%d] set wall adj thres errno:%d", fd, errno);
    return rc;
}

static int get_sysfs_uint32(const char *path, uint32_t *value)
{
    int fd;
    char valstr[64];
    uint32_t val = 0;

    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        ssize_t rn = 0;
        memset(valstr, 0, 64);
        rn = read(fd, valstr, 64 - 1);
        if (rn > 0)
            valstr[strlen(valstr)] = '\0';

        close(fd);
        if (rn < 0)
            return -1;
    } else {
        log_error("unable to open file %s\n", path);
        return -1;
    }
    if (sscanf(valstr, "%u", &val) < 1) {
        log_error("unable to get pts from: %s", valstr);
        return -1;
    }
    *value = val;
    return 0;
}

int msync_session_get_disc_thres(int session_id, uint32_t *min, uint32_t *max)
{
    char name[64];

    if (snprintf(name, sizeof(name),
            "/sys/class/avsync_session%d/disc_thres_min", session_id) < 0)
        return -1;
    if (get_sysfs_uint32(name, min))
        return -1;

    if (snprintf(name, sizeof(name),
            "/sys/class/avsync_session%d/disc_thres_max", session_id) < 0)
        return -1;
    if (get_sysfs_uint32(name, max))
        return -1;

    return 0;
}

static int set_sysfs_uint32(const char *path, uint32_t value)
{
    int fd, ret = 0;
    char valstr[64];

    fd = open(path, O_RDWR);
    snprintf(valstr, sizeof(valstr), "%d", value);
    if (fd >= 0) {
        ret = write(fd, valstr, strnlen(valstr, sizeof(valstr)));
        if (ret >= 0)
            ret = 0;
        close(fd);
    } else {
        log_error("unable to open file %s\n", path);
        return -1;
    }
    return ret;
}

int msync_session_set_disc_thres(int session_id, uint32_t min, uint32_t max)
{
    char name[64];

    if (snprintf(name, sizeof(name),
            "/sys/class/avsync_session%d/disc_thres_min", session_id) < 0)
        return -1;
    if (set_sysfs_uint32(name, min))
        return -1;

    if (snprintf(name, sizeof(name),
            "/sys/class/avsync_session%d/disc_thres_max", session_id) < 0)
        return -1;
    if (set_sysfs_uint32(name, max))
        return -1;

    return 0;
}

int msync_session_stop_audio(int fd)
{
    int rc, nouse;

    rc = ioctl(fd, AMSYNCS_IOC_SET_STOP_AUDIO_WAIT, &nouse);
    if (rc)
        log_error("session[%d] set stop audio errno:%d", fd, errno);
    return rc;
}

int msync_session_set_start_thres(int fd, uint32_t thres)
{
    if (set_sysfs_uint32("/sys/class/aml_msync/start_buf_thres", thres * 90))
        return -1;
    return 0;
}
