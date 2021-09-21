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
#ifndef AML_AVSYNC_H__
#define AML_AVSYNC_H__

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

enum sync_mode {
    AV_SYNC_MODE_VMASTER = 0,
    AV_SYNC_MODE_AMASTER = 1,
    AV_SYNC_MODE_PCR_MASTER = 2,
    AV_SYNC_MODE_IPTV = 3,
    AV_SYNC_MODE_FREE_RUN = 4,
    AV_SYNC_MODE_VIDEO_MONO = 5, /* video render by system mono time */
    AV_SYNC_MODE_MAX
};

enum sync_type {
    AV_SYNC_TYPE_AUDIO,
    AV_SYNC_TYPE_VIDEO,
    AV_SYNC_TYPE_PCR,
    AV_SYNC_TYPE_MAX
};

enum sync_start_policy {
    AV_SYNC_START_NONE = 0,
    AV_SYNC_START_V_FIRST = 1,
    AV_SYNC_START_A_FIRST = 2,
    AV_SYNC_START_ASAP = 3,
    AV_SYNC_START_ALIGN = 4,
    AV_SYNC_START_V_PEEK = 5,
    AV_SYNC_START_MAX
};

enum clock_recovery_stat {
    CLK_RECOVERY_NOT_RUNNING = 0,
    CLK_RECOVERY_ONGOING = 1,
    CLK_RECOVERY_READY = 2,
    CLK_RECOVERY_ERR,
    CLK_RECOVERY_MAX
};

#define AV_SYNC_INVALID_PAUSE_PTS 0xFFFFFFFF
#define AV_SYNC_STEP_PAUSE_PTS 0xFFFFFFFE
#define AV_SYNC_SESSION_V_MONO 64

typedef uint32_t pts90K;
struct vframe;
typedef void (*free_frame)(struct vframe * frame);
typedef void (*pause_pts_done)(uint32_t pts, void* priv);

typedef enum {
    /* good to render */
    AV_SYNC_ASCB_OK,
    /* triggered by av_sync_destroy */
    AV_SYNC_ASCB_STOP,
} avs_ascb_reason;

typedef int (*audio_start_cb)(void *priv, avs_ascb_reason reason);

typedef enum {
    AV_SYNC_ASTART_SYNC = 0,
    AV_SYNC_ASTART_ASYNC,
    AV_SYNC_ASTART_AGAIN,
    AV_SYNC_ASTART_ERR
} avs_start_ret;

typedef enum {
    /* render ASAP */
    AV_SYNC_AA_RENDER,
    /* late, drop to catch up */
    AV_SYNC_AA_DROP,
    /* early, insert to hold */
    AV_SYNC_AA_INSERT,
    AA_SYNC_AA_MAX
} avs_audio_action;

struct audio_policy {
    avs_audio_action action;
    /* delta between apts and ideal render position
     * positive means apts is behind wall clock
     * negative means apts is ahead of wall clock
     * 0 is only valid for AV_SYNC_AA_DROP, drop current chunk
     */
    int delta;
};

struct vframe {
    /* private user data */
    void *private;
    pts90K pts;
    /* mono render time in nanosecdons only use in VIDEO_MONO */
    uint64_t mts;
    /* duration of this frame.  0 for invalid value */
    pts90K duration;
    /* free function, will be called when multi frames are
     * toggled in a single VSYNC, on frames not for display.
     * For the last toggled frame, free won't be called. Caller
     * of av_sync_pop_frame() are responsible for free poped frame.
     * For example, if frame 1/2/3 are toggled in a single VSYCN,
     * free() of 1/2 will be called, but free() of 3 won't.
     */
    free_frame free;

    //For internal usage under this line
    /*holding period */
    int hold_period;
};

struct video_config {
    /* AV sync delay. The delay of render.
     * For video in VSYNC number unit:
     * 2 for video planes
     * 1 for osd planes
     */
    int delay;
};

struct start_policy {
    enum sync_start_policy policy;
    /*timeout in ms */
    int timeout;
};

/* Open a new session and create the ID
 * Params:
 *   @session_id: session ID allocated if success
 * Return:
 *   >= 0 session handle. session_id is valid
 *   < 0 error, session_id is invalid
 */
int av_sync_open_session(int *session_id);

/* Close session, will also decrease the ref cnt of session ID.
 * Params:
 *   @session: Av sync session handle
 */
void av_sync_close_session(int session);

/* Attach to kernel session. The returned avsync module will
 * associated with @session_id. It is valid to create multi instance
 * with the same session_id. For example, one for video, one for audio,
 * one for PCR. But it is NOT valid to create 2 video session with the
 * same session_id. Create session will increase the ref count of session
 * ID.
 * Params:
 *   @session_id: unique AV sync session ID to bind audio and video
 *               usually get from kernel driver.
 *               If ID AV_SYNC_SESSION_V_MONO or bigger is assigned, only
 *               AV_SYNC_MODE_VIDEO_MONO is accepted.
 *   @mode: AV sync mode of enum sync_mode
 *   @type: AV sync type of enum sync_type. For a stream with both
 *          video and audio, two instances with each type should be created.
 *   @start_thres: The start threshold of AV sync module. Set it to 0 for
 *               a default value. For low latency mode, set it to 1. Bigger
 *               value will increase the delay of the first frame rendered.
 *               AV sync will start when frame number reached threshold.
 * Return:
 *   null for failure, or handle for avsync module.
 */
void* av_sync_create(int session_id,
                     enum sync_mode mode,
                     enum sync_type type,
                     int start_thres);


/* Attach to an existed session. The returned avsync module will
 * associated with @session_id. use av_sync_destroy to destroy it.
 * Designed for audio path for now. Session created by audio client,
 * and attached by audio HAL. Attach sesson will increase the ref count of
 * session ID.
 * Params:
 *   @session_id: unique AV sync session ID to bind audio and video
 *               usually get from kernel driver.
 *   @type: AV sync type of enum sync_type. For a stream with both
 *          video and audio, two instances with each type should be created.
 * Return:
 *   null for failure, or handle for avsync module.
 */

void* av_sync_attach(int session_id,
                     enum sync_type type);

/* Dettach from existed session. Decrease the ref count of session ID
 * Params:
 *   @sync: handle for avsync module
 */
void av_sync_destroy(void *sync);

int av_sync_video_config(void *sync, struct video_config* config);

/* Set start policy
 * Params:
 *   @sync: AV sync module handle
 *   @policy: start policy
 * Return:
 *   0 for OK, or error code
 */
int avs_sync_set_start_policy(void *sync, struct start_policy* st_policy);

/* Pause/Resume AV sync module.
 * It will return last frame in @av_sync_pop_frame() in pause state
 * Params:
 *   @sync: AV sync module handle
 *   @pause: pause for true, or resume.
 * Return:
 *   0 for OK, or error code
 */
int av_sync_pause(void *sync, bool pause);

/* Push a new video frame to AV sync module
 * Params:
 *   @sync: AV sync module handle
 * Return:
 *   0 for OK, or error code
 */
int av_sync_push_frame(void *sync , struct vframe *frame);

/* notify current system mono time for current VSYNC.
 * This API should be VSYNC triggerd. Used only in VIDEO_MONO mode.
 * Params:
 *   @sync: AV sync module handle
 *   @msys: system mono time of current VSYNC interrupt in nanosecond
 *          Usually returned by drmWaitVBlank.
 * Return:
 *   0 for OK, or error code
 * */
int av_sync_set_vsync_mono_time(void *sync , uint64_t msys);

/* Pop video frame for next VSYNC. This API should be VSYNC triggerd.
 * Params:
 *   @sync: AV sync module handle
 * Return:
 *   Old frame if current frame is hold
 *   New frame if it is time for a frame toggle.
 *   null if there is no frame to pop out (underrun).
 * */
struct vframe *av_sync_pop_frame(void *sync);

/* Audio start control. Audio render need to check return value and
 * do sync or async start. NOT thread safe.
 * Params:
 *   @sync: AV sync module handle
 *   @pts: first audio pts
 *   @delay: rendering delay
 *   @cb: callback to notify rendering start.
 *   @priv: parameter for cb
 * Return:
 *   AV_SYNC_ASTART_SYNC, audio can start render ASAP. No need to wait for callback.
 *   AV_SYNC_ASTART_ASYNC, audio need to block until callback is triggered.
 *   AV_SYNC_ASTART_AGAIN, discard current audio chunk and call this API again.
 *   AV_SYNC_ASTART_ERR, something bad happens
 */
avs_start_ret av_sync_audio_start(
    void *sync,
    pts90K pts,
    pts90K delay,
    audio_start_cb cb,
    void *priv);

/* Audio render policy. Indicate render action and the difference from ideal position
 * NOT thread safe.
 * Params:
 *   @sync: AV sync module handle
 *   @pts: curent audio pts (considering delay)
 *   @policy: action field indicates the action.
 *            delta field indicates the diff from ideal position
 * Return:
 *   0 for OK, or error code
 */
int av_sync_audio_render(
    void *sync,
    pts90K pts,
    struct audio_policy *policy);

/* set playback speed
 * Currently only work for VMASTER mode
 * Params:
 *   @speed: 1.0 is normal speed. 2.0 is 2x faster. 0.1 is 10x slower
 *           Minimium speed is 0.001
 *           Max speed is 100
 * Return:
 *   0 for OK, or error code
 */
int av_sync_set_speed(void *sync, float speed);

/* switch avsync mode
 * PCR master/IPTV handle mode change automatically
 * Params:
 *   @sync: AV sync module handle
 *   @sync_mode: new mode
 * Return:
 *   0 for OK, or error code
 */
int av_sync_change_mode(void *sync, enum sync_mode mode);

/* get avsync mode
 * Params:
 *   @sync: AV sync module handle
 *   @mode: pointer to return current mode
 * Return:
 *   0 for OK, or error code
 */
int av_sync_get_mode(void *sync, enum sync_mode *mode);

/* set pause PTS
 * av sync will pause after reaching the assigned PTS or do step.
 * Need to call @av_sync_pause(false) to resume a the playback.
 * Params:
 *   @sync: AV sync module handle
 *   @pts: pause pts in uint of 90K.
 *         AV_SYNC_STEP_PAUSE_PTS: step one frame, and the callback
 *           will report pts value of the stepped frame.
 *         AV_SYNC_INVALID_PAUSE_PTS: disable this feature.
 *         Other value: play until the assigned PTS, and the callback
 *           will report pts value of the paused frame.
 * Return:
 *   0 for OK, or error code
 */
int av_sync_set_pause_pts(void *sync, pts90K pts);

/* set pause PTS call back
 * av sync will callback when pause PTS is reached with assigned PTS from
 * @av_sync_set_pause_pts.
 * Params:
 *   @sync: AV sync module handle
 *   @cb: callback function
 *   @priv: callback function parameter
 * Return:
 *   0 for OK, or error code
 */
int av_sync_set_pause_pts_cb(void *sync, pause_pts_done cb, void *priv);

/* Update PCR clock.
 * Use by AV_SYNC_TYPE_PCR only.
 * Params:
 *   @sync: AV sync module handle
 *   @pts: PCR clock
 *   @mono_clock: system monotonic clock receiving the pts
 * Return:
 *   0 for OK, or error code
 */
int av_sync_set_pcr_clock(void *sync, pts90K pts, uint64_t mono_clock);

/* Get PCR clock pair.
 * Use for clock recovery
 * Params:
 *   @sync: AV sync module handle
 *   @pts: PCR clock pointer
 *   @mono_clock: system monotonic clock pointer receiving the pts
 * Return:
 *   0 for OK, or error code
 */
int av_sync_get_pcr_clock(void *sync, pts90K *pts, uint64_t *mono_clock);

/* get wall clock
 * Params:
 *   @sync: AV sync module handle
 *   @pts: return wall clock
 * Return:
 *   0 for OK, or error code
 */
int av_sync_get_clock(void *sync, pts90K *pts);

/* set session name for debugging purpose
 * The session name will be listed from /sys/class/aml_msync/list_session
 * Params:
 *   @sync: AV sync module handle
 *   @name: name of current session.
 * Return:
 *   0 for OK, or error code
 */
int av_sync_set_session_name(void *sync, const char *name);

/* Start/finish audio seamless switch
 * It declaims the coming audio seamless switch will happen.
 * After removing the audio, the video will still play normally.
 * After adding back the audio, it will sync with pipe-line without break the playing,
 * only short audio break/mute on the switch is expected.
 * Params:
 *   @sync: AV sync module handle
 *   @start: start the switch for true, or finish/reset for false
 * Return:
 *   0 for OK, or error code
 */
int av_sync_set_audio_switch(void *sync,  bool start);

/*  Get the current audio seamless switch status.
 *  After setting the seamless switch, upper level can get the result by polling.
 *  The status will become false when the switching has finished or being termintated
 * Params:
 *   @sync: AV sync module handle
 *   @status: start the switch for true, or finish/reset for false
 * Return:
 *   0 for OK, or error code
 */
int av_sync_get_audio_switch(void *sync,  bool *status);

/*  Get the current clock deviation between PCR clock and system monotonic clock.
 * Params:
 *   @sync: AV sync module handle
 *   @ppm: part per million. Bigger than 0 is PCR master clock is faster. Less than 0
 *         if PCR master clock is slower.
 * Return:
 *   CLK_RECOVERY_NOT_RUNNING: current mode doesn't support clock recovery
 *   CLK_RECOVERY_ONGOING: still ongoing, need more time to converge.
 *   CLK_RECOVERY_READY: clock recovery result is ready to be used. The result might
 *                       be updated later for better accuracy.
 *   CLK_RECOVERY_ERR: error happens
 */
enum  clock_recovery_stat av_sync_get_clock_deviation(void *sync, int32_t *ppm);
#endif
