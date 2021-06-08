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
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "aml_avsync_log.h"
#include "pcr_monitor.h"

#define PCR_MODULE_NAME "[PCR_MONITOR]"
#define log_pcr_trace(fmt, ...) log_trace(PCR_MODULE_NAME#fmt, ##__VA_ARGS__)
#define log_pcr_debug(fmt, ...) log_debug(PCR_MODULE_NAME#fmt, ##__VA_ARGS__)
#define log_pcr_info(fmt, ...)  log_info(PCR_MODULE_NAME#fmt, ##__VA_ARGS__)
#define log_pcr_warn(fmt, ...)  log_warn(PCR_MODULE_NAME#fmt, ##__VA_ARGS__)
#define log_pcr_error(fmt, ...) log_error(PCR_MODULE_NAME#fmt, ##__VA_ARGS__)
#define log_pcr_fatal(fmt, ...) log_fatal(PCR_MODULE_NAME#fmt, ##__VA_ARGS__)

//unit as 1 PPM : 1/(1000*1000)

#define PPM_SCALE (1000*1000)
#define CLOCK_RECORD_NUM (1000)
#define PROBE_DEVIATION_RANGE (10)
#define PROBE_DEVIATION_HIGH_RANGE (100)
#define ADJUST_DEVIATION_HIGH_RANGE (30)
#define MONITOR_START_STEP (10)
#define MONITOR_GROUP_NUM (100)
#define MONITOR_GROUP_LONG_TERM_LEVEL (MONITOR_GROUP_NUM*8/10)
#define RECORD_BIAS_MAX (30*1000)
#define WAIT_DEVIATION_STABLE_COUNT (10)
#define WAIT_DEVIATION_RANGE (20)
#define MONITOR_PCR_BIG_GAP (60*1000*1000) //60s
#define SKIP_START_GROUP_NUM (3)
//#define DUMP_TO_FILE

enum error_return {
    INVALID_PARAMETER = -1,
    INVALID_STATUS = -2,
};

struct pcr_group {
    int invalid_count;
    long long total_pts;
    long long total_monoclk;
    struct pcr_info start_pcr;
    struct pcr_info last_pcr;
    struct pcr_info avg_pcr;
    struct pcr_info adjust_avg_pcr;
};

struct clock_record {
    int pcr_index;
    int group_start;
    int group_next;
    int new_group_arrived;
    struct pcr_group group[MONITOR_GROUP_NUM];
    struct pcr_info pcr[CLOCK_RECORD_NUM];
};

struct monitor_info {
    enum pcr_monitor_status status;
    int probe_step;
    int skip_start_group;
    int deviation_short_term;   //caculate by serval group avg
    int deviation_long_term;    //caculate by all group
    int deviation;             //return to caller
    int wait_count;
    struct clock_record record;
};

static int pcr_monitor_record(struct monitor_info * info, struct pcr_info *pcr);
static int contruct_monitor(struct monitor_info *monitor_info, struct clock_record * record);
static int probe_monitor_step(struct clock_record * record, enum pcr_monitor_status *status, int *step);
static int get_group_count(struct clock_record * record, int *count);
static int get_record_deviation(struct clock_record * record, int start, int end, int *deviation);
static int adjust_deviation(struct monitor_info *monitor_info, struct clock_record * record);
static int output_deviation(struct monitor_info *monitor_info, struct clock_record * record);

#ifdef DUMP_TO_FILE
static int file_index = 0;

static void dump(const char* path, long long pts, long long mono) {
    FILE* fd;
    char name[50];

    sprintf(name, "%s%d.txt", path, file_index);

    fd = fopen(name, "a");
    if (!fd)
        return;
    fprintf(fd, "%llu, %llu\n", pts, mono);
    fclose(fd);
}
#endif


static int get_record_deviation(struct clock_record * record, int start, int end, int *deviation)
{
    long long mono_clock_diff;
    long long pts_diff;
    long long pts_monoclk_diff;
    struct pcr_info *start_avg;
    struct pcr_info *end_avg;
    if (record == NULL || deviation == NULL)
        return -1;

    if (start >= MONITOR_GROUP_NUM || start < 0)
        return -1;

    if (end >= MONITOR_GROUP_NUM || end < 0)
        return -1;

    start_avg = &record->group[start].avg_pcr;
    end_avg = &record->group[end].avg_pcr;
    mono_clock_diff = end_avg->monoclk - start_avg->monoclk;
    pts_diff = end_avg->pts - start_avg->pts;

    pts_monoclk_diff = pts_diff - mono_clock_diff;
    *deviation = (long long)(pts_monoclk_diff * PPM_SCALE) / mono_clock_diff;

    log_pcr_info("start:%d, end:%d, deviation:%d, pts_monoclk_diff:%lld, mono_clock_diff:%lld, pts_diff:%lld, start monoclk:%lld, pts:%lld, end monoclk:%lld, pts:%lld",
        start, end, *deviation, pts_monoclk_diff, mono_clock_diff, pts_diff,
        start_avg->monoclk, start_avg->pts,
        end_avg->monoclk, end_avg->pts);

    return 0;
}

#if 0
static int get_record_adjsut_deviation(struct clock_record * record, int start, int end, int *deviation)
{
    long long mono_clock_diff;
    long long pts_diff;
    long long pts_monoclk_diff;
    struct pcr_info *start_avg;
    struct pcr_info *end_avg;
    if (record == NULL || deviation == NULL)
        return -1;

    if (start >= MONITOR_GROUP_NUM || start < 0)
        return -1;

    if (end >= MONITOR_GROUP_NUM || end < 0)
        return -1;

    start_avg = &record->group[start].adjust_avg_pcr;
    end_avg = &record->group[end].adjust_avg_pcr;
    mono_clock_diff = end_avg->monoclk - start_avg->monoclk;
    pts_diff = end_avg->pts - start_avg->pts;

    pts_monoclk_diff = pts_diff - mono_clock_diff;
    *deviation = (long long)(pts_monoclk_diff * PPM_SCALE) / mono_clock_diff;

    log_pcr_info("start:%d, end:%d, deviation:%d, pts_monoclk_diff:%lld, mono_clock_diff:%lld, pts_diff:%lld, start monoclk:%lld, pts:%lld, end monoclk:%lld, pts:%lld",
        start, end, *deviation, pts_monoclk_diff, mono_clock_diff, pts_diff,
        start_avg->monoclk, start_avg->pts,
        end_avg->monoclk, end_avg->pts);

    return 0;
}
#endif

static int get_group_count(struct clock_record * record, int *count)
{

    if (record == NULL || count == NULL)
        return -1;

    if (record->group_start == record->group_next) {
        *count = 0;
    } else {
        *count = (record->group_next + MONITOR_GROUP_NUM - record->group_start) % MONITOR_GROUP_NUM;
    }

    return 0;
}

static int probe_monitor_step(struct clock_record * record, enum pcr_monitor_status *status, int *step)
{
    int start1, start2;
    int end1, end2;
    int deviation1, deviation2;
    if (record == NULL || status == NULL  || step == NULL)
        return -1;

    start1 = record->group_start;
    end1 = (start1 + *step - 1) % MONITOR_GROUP_NUM;

    start2 = record->group_start + 1;
    end2 = (start2 + *step - 1) % MONITOR_GROUP_NUM;

    get_record_deviation(record, start1, end1, &deviation1);
    get_record_deviation(record, start2, end2, &deviation2);

    if (abs(deviation1 - deviation2) < PROBE_DEVIATION_RANGE) {
        *status = CONSTRUCT_MONITOR;
    } else if (abs(deviation1 - deviation2) > PROBE_DEVIATION_HIGH_RANGE) {
        //it means we should drop some group
        if ((abs(deviation1) > abs(deviation2) && deviation1 * deviation2 > 0) || (deviation1 * deviation2 < 0)) {
            log_pcr_info("we drop group[%d], since it introduces much unstable!", start1);
            memset(&record->group[start1], 0, sizeof(struct pcr_group));
            record->group_start = (++ start1) % MONITOR_GROUP_NUM;
        }
        else {
            record->group_next = end2;
            memset(&record->group[end2], 0, sizeof(struct pcr_group));
            log_pcr_info("we drop group[%d], since it introduces much unstable!", end2);
        }
    } else {
        (*step) ++;
    }

    return 0;
}

static int contruct_monitor(struct monitor_info *monitor_info, struct clock_record * record)
{
    int start;
    int end;
    int step;
    int deviation;

    if (record == NULL || monitor_info == NULL)
        return -1;

    step = monitor_info->probe_step;
    start = record->group_start;

    end = (start + step - 1) % MONITOR_GROUP_NUM;

    get_record_deviation(record, start, end, &deviation);
    monitor_info->deviation_short_term = deviation;
    monitor_info->status = WAIT_DEVIATION_STABLE;
    monitor_info->wait_count = WAIT_DEVIATION_STABLE_COUNT;

    return 0;
}

static int output_deviation(struct monitor_info *monitor_info, struct clock_record * record)
{
    int start, end;
    int probe_step;
    int deviation[WAIT_DEVIATION_STABLE_COUNT + 1];
    int i, j, size;
    int dev_temp;

    if (record == NULL || monitor_info == NULL)
        return -1;

    probe_step = monitor_info->probe_step;
    size = WAIT_DEVIATION_STABLE_COUNT + 1;
    for (i = 0; i < size; i++) {
        start = record->group_start;
        end = (record->group_start + probe_step + i) % MONITOR_GROUP_NUM;
        get_record_deviation(record, start, end, &deviation[i]);
    }

    for(i = 0; i < size; i++) {
        for(j = i + 1; j < size; ++j) {
            if(deviation[j] < deviation[i]) {
                dev_temp = deviation[i];
                deviation[i] = deviation[j];
                deviation[j] = dev_temp;
            }
        }
    }

    for(i = 0; i < size; i++)
        log_pcr_debug("deviation[%d]:%d", i, deviation[i]);

    monitor_info->deviation_short_term = deviation[size/2];
    monitor_info->status = DEVIATION_READY;
    monitor_info->deviation = monitor_info->deviation_short_term;

    return 0;
}

static int adjust_deviation(struct monitor_info *monitor_info, struct clock_record * record)
{
    int start;
    int end;
    int adjustment;
    int group_count;
    if (record == NULL || monitor_info == NULL)
        return -1;

    start = record->group_start;
    end = (record->group_next + MONITOR_GROUP_NUM - 1) % MONITOR_GROUP_NUM;

    get_record_deviation(record, start, end, &monitor_info->deviation_long_term);

    adjustment = monitor_info->deviation_long_term - monitor_info->deviation;

    get_group_count(record, &group_count);
    if (abs(adjustment) > ADJUST_DEVIATION_HIGH_RANGE) {
        log_pcr_info("we need consider to adjust average window, long term deviation:%d, short term deviation: %d",
            monitor_info->deviation_long_term, monitor_info->deviation_short_term);
        monitor_info->probe_step = group_count;
        monitor_info->deviation_short_term = monitor_info->deviation_long_term;
        monitor_info->status = CONSTRUCT_MONITOR;
    }

    return 0;
}

static int adjust_group_avg(struct clock_record *record, struct pcr_group * group)
{
    int i;
    struct pcr_info * current = &record->pcr[0];
    long long total_pts = 0;
    long long total_mono_clk = 0;
    long long pts_diff;
    long long clk_diff;
    int validcount = 0;
    struct pcr_info * valid_pcr = &record->pcr[0];

    for (i=0; i <(CLOCK_RECORD_NUM); i++) {

        pts_diff = current->pts - group->avg_pcr.pts;
        clk_diff = current->monoclk - group->avg_pcr.monoclk;
        if(abs(pts_diff - clk_diff) > RECORD_BIAS_MAX) {
            group->invalid_count ++;
            log_pcr_trace("[%d] drop, invalid count: %d, pts_diff:%lld, clk_diff:%lld", i, group->invalid_count, pts_diff, clk_diff);
        } else {
            valid_pcr->monoclk = current->monoclk;
            valid_pcr->pts = current->pts;
            total_pts += current->pts;
            total_mono_clk += current->monoclk;
            validcount ++;
            valid_pcr ++;
        }
        current ++;
    }
    group->total_monoclk = total_mono_clk;
    group->total_pts = total_pts;
    record->pcr_index = validcount;

    log_pcr_debug("valid count:%d, monoclk:%lld, pts: %lld", validcount, total_mono_clk, total_pts);

    if (validcount < 980)
        return -1;

    group->adjust_avg_pcr.monoclk = total_mono_clk / validcount;
    group->adjust_avg_pcr.pts = total_pts / validcount;

    return 0;
}


static bool pcr_group_is_valid(struct clock_record *record, struct pcr_group * group)
{
    if (adjust_group_avg(record, group) < 0)
        return false;

    return true;
}


static bool pcr_group_has_big_gap(struct clock_record *record)
{
    int i;
    struct pcr_info * current = &record->pcr[0];
    struct pcr_info * next = &record->pcr[1];
    int diff;
    for (i=0; i <(CLOCK_RECORD_NUM-1); i++) {
        diff = next->pts - current->pts;
        if (diff > 0) {
            if (diff > MONITOR_PCR_BIG_GAP) {
                log_pcr_error("pcr may has big jump:%d", diff);
            }
        } else {
            diff = abs(diff);
            if (diff > MONITOR_PCR_BIG_GAP) {
                log_pcr_error("pcr has big back:%d", diff);
                return true;
            }
        }
        current ++;
        next ++;
    }

    return false;
}

static int pcr_monitor_reset(struct monitor_info *monitor_handle)
{
    int ret = 0;
    struct monitor_info * info = (struct monitor_info *)monitor_handle;
    if (monitor_handle == NULL)
        return INVALID_PARAMETER;

    info->status = RECORDING;
    info->probe_step = MONITOR_START_STEP;
    info->wait_count = WAIT_DEVIATION_STABLE_COUNT;
    info->deviation = 0;
    info->skip_start_group = 0;

    memset(&info->record, 0, sizeof(struct clock_record));

    return ret;
}

static int pcr_monitor_record(struct monitor_info * info, struct pcr_info *pcr)
{
    int index;
    int group_next;
    int group_start;
    struct pcr_info *pcr_record;
    struct pcr_group *current_group;
    struct clock_record *record;

    if (info == NULL || pcr == NULL)
        return -1;

    record = &info->record;
    index  = record->pcr_index;

    group_next = record->group_next;
    group_start = record->group_start;

    log_pcr_debug("monoclk:%lld, pts:%lld, group start:%d, end:%d, record index:%d", pcr->monoclk, pcr->pts,
        group_start, group_next, index);

#ifdef DUMP_TO_FILE
    dump("/data/pcr_monitor_record_", pcr->monoclk, pcr->pts);
#endif
    current_group = &record->group[group_next];
    pcr_record = &record->pcr[index];
    if (index < CLOCK_RECORD_NUM) {
        pcr_record->monoclk = pcr->monoclk;
        pcr_record->pts = pcr->pts;
        record->pcr_index ++;

        current_group->total_monoclk += pcr->monoclk;
        current_group->total_pts += pcr->pts;
    }

    if (record->pcr_index == CLOCK_RECORD_NUM) {
        current_group->start_pcr.monoclk = record->pcr[0].monoclk;
        current_group->start_pcr.pts = record->pcr[0].pts;
        current_group->last_pcr.monoclk = record->pcr[CLOCK_RECORD_NUM - 1].monoclk;
        current_group->last_pcr.pts = record->pcr[CLOCK_RECORD_NUM - 1].pts;

        current_group->avg_pcr.monoclk = current_group->total_monoclk / CLOCK_RECORD_NUM;
        current_group->avg_pcr.pts = current_group->total_pts / CLOCK_RECORD_NUM;

        log_pcr_info("[%d]first monoclk:%lld, first pts:%lld, end monoclk:%lld, end pts:%lld, avg monoclk:%lld, avg pts:%lld",
            group_next,
            current_group->start_pcr.monoclk, current_group->start_pcr.pts,
            current_group->last_pcr.monoclk, current_group->last_pcr.pts,
            current_group->avg_pcr.monoclk, current_group->avg_pcr.pts);

        if (pcr_group_has_big_gap(record)) {
            pcr_monitor_reset(info);
            return -1;
        }

        if (pcr_group_is_valid(record, current_group)) {
            record->pcr_index = 0;
            record->new_group_arrived = 1;
            record->group_next = (++ group_next) % MONITOR_GROUP_NUM;
            if (record->group_next == group_start) {
                memset(&record->group[group_start], 0, sizeof(struct pcr_group));
                record->group_start = (++ group_start) % MONITOR_GROUP_NUM;
            }
        }
    }

    return 0;
}

int pcr_monitor_init(void ** monitor_handle)
{
    struct monitor_info * monitor;
    int ret = 0;
    if (monitor_handle == NULL)
        return -1;

#ifdef DUMP_TO_FILE
    file_index ++;
#endif

    monitor = calloc(1, sizeof(struct monitor_info));
    if (!monitor) {
        ret = -ENOMEM;
        goto err;
    }

    monitor->status = RECORDING;
    monitor->probe_step = MONITOR_START_STEP;
    monitor->wait_count = WAIT_DEVIATION_STABLE_COUNT;
    monitor->deviation = 0;
    monitor->skip_start_group = 0;

    memset(&monitor->record, 0, sizeof(struct clock_record));

    *monitor_handle = (void *) monitor;

err:
    return ret;
}

int pcr_monitor_process(void *monitor_handle, struct pcr_info *pcr)
{
    int status;
    int record_count = 0;
    struct clock_record *record;
    struct monitor_info *info = (struct monitor_info *)monitor_handle;;
    if (monitor_handle == NULL || pcr == NULL)
        return -1;

    record = &info->record;

    pcr_monitor_record(info, pcr);
    status = info->status;

    switch (status) {
    case UNINITIALIZE:
        return INVALID_STATUS;
    case RECORDING:
        if (record->new_group_arrived) {
            get_group_count(record, &record_count);
            record->new_group_arrived = 0;
            if (record_count > info->probe_step) {
                if (info->skip_start_group == 0) {
                    memset(&record->group[record->group_start], 0, sizeof(struct pcr_group) * SKIP_START_GROUP_NUM);
                    record->group_start += SKIP_START_GROUP_NUM;
                    record->group_start %= MONITOR_GROUP_NUM;
                    info->skip_start_group = 1;
                } else
                    probe_monitor_step(record, &info->status, &info->probe_step);
           }
        }
        break;
    case CONSTRUCT_MONITOR:
        contruct_monitor(info, record);
        break;
    case WAIT_DEVIATION_STABLE:
        if (record->new_group_arrived) {
            record->new_group_arrived = 0;
            get_group_count(record, &record_count);
            if (record_count > (info->probe_step + info->wait_count)) {
                output_deviation(info, record);
            }
        }
        break;
    case DEVIATION_READY:
    case DEVIATION_LONG_TERM_READY:
        if (record->new_group_arrived) {
            record->new_group_arrived = 0;
            adjust_deviation(info, record);
        }
    default:
        break;
    };

    return 0;
}

enum pcr_monitor_status  pcr_monitor_get_status(void *monitor_handle)
{
    struct monitor_info * info = (struct monitor_info *)monitor_handle;
    if (monitor_handle == NULL)
        return INVALID_PARAMETER;

    return info->status;
}

int pcr_monitor_get_deviation(void *monitor_handle, int *ppm)
{
    int ret = 0;
    struct monitor_info * info = (struct monitor_info *)monitor_handle;
    if (monitor_handle == NULL || ppm == NULL)
        return INVALID_PARAMETER;

    if (info->status >= DEVIATION_READY) {
        *ppm = info->deviation;
        ret = 0;
    } else {
        *ppm = 0;
        ret = INVALID_STATUS;
    }
    return ret;
}

int pcr_monitor_destroy(void *monitor_handle)
{
    if (monitor_handle == NULL)
         return INVALID_PARAMETER;

    free(monitor_handle);

    return 0;
}
