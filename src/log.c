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

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#ifdef ENABLE_SYSLOG
#include <syslog.h>
#elif ENABLE_LOGCAT
#define LOG_TAG "avs"
#include <cutils/log.h>
#endif

#include "aml_avsync_log.h"

static struct {
  void *udata;
  log_LockFn lock;
  FILE *fp;
  int level;
  int quiet;
} L = {0};


static const char *level_names[] = {
  "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

#ifdef LOG_USE_COLOR
static const char *level_colors[] = {
  "\x1b[94m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[35m"
};
#endif


static void lock(void)   {
  if (L.lock) {
    L.lock(L.udata, 1);
  }
}


static void unlock(void) {
  if (L.lock) {
    L.lock(L.udata, 0);
  }
}


void log_set_udata(void *udata) {
  L.udata = udata;
}


void log_set_lock(log_LockFn fn) {
  L.lock = fn;
}


void log_set_fp(FILE *fp) {
  L.fp = fp;
}


void log_set_level(int level) {
  L.level = level;
}


void log_set_quiet(int enable) {
  L.quiet = enable ? 1 : 0;
}


void log_log(int level, const char *file, int line, const char *fmt, ...) {
  if (level < L.level) {
    return;
  }

  /* Acquire lock */
  lock();

  /* Get current time */
   struct timespec tm;
   long second, usec;

   clock_gettime( CLOCK_MONOTONIC_RAW, &tm );
   second = tm.tv_sec;
   usec = tm.tv_nsec/1000LL;

#if defined(ENABLE_SYSLOG)
  {
    va_list args;
    int l;
    char content[512];
    switch (level) {
      case AVS_LOG_FATAL:
        l = LOG_CRIT;
        break;
      case AVS_LOG_ERROR:
        l = LOG_ERR;
        break;
      case AVS_LOG_WARN:
        l = LOG_WARNING;
        break;
      case AVS_LOG_INFO:
        l = LOG_NOTICE;
        break;
      case AVS_LOG_DEBUG:
        l = LOG_INFO;
        break;
      case AVS_LOG_TRACE:
      default:
        l = LOG_DEBUG;
        break;
    }
    va_start(args, fmt);
    vsnprintf(content, 512, fmt, args);
    va_end(args);
    syslog(l, "[%ld.%06ld]: %-5s %s:%d: %s", second, usec, level_names[level], file, line, content);

    unlock();
    return;
  }
#elif defined(ENABLE_LOGCAT)
  {
    va_list args;
    int l;
    char content[512];
    switch (level) {
      case AVS_LOG_FATAL:
        l = ANDROID_LOG_ERROR;
        break;
      case AVS_LOG_ERROR:
        l = ANDROID_LOG_ERROR;
        break;
      case AVS_LOG_WARN:
        l = ANDROID_LOG_WARN;
        break;
      case AVS_LOG_INFO:
        l = ANDROID_LOG_INFO;
        break;
      case AVS_LOG_DEBUG:
        l = ANDROID_LOG_DEBUG;
        break;
      case AVS_LOG_TRACE:
      default:
        l = ANDROID_LOG_VERBOSE;
        break;
    }
    va_start(args, fmt);
    vsnprintf(content, 512, fmt, args);
    va_end(args);
    LOG_PRI(l, LOG_TAG, "[%ld.%06ld]: %-5s %s:%d: %s", second, usec, level_names[level], file, line, content);

    unlock();
    return;
  }
#endif
  /* Log to stderr */
  if (!L.quiet) {
    va_list args;
#ifdef LOG_USE_COLOR
    fprintf(
      stderr, "[%ld.%06ld]: %s%-5s\x1b[0m \x1b[90m%s:%d:\x1b[0m ",
     second, usec, level_colors[level], level_names[level], file, line);
#else
    fprintf(stderr, "[%ld.%06ld]: %-5s %s:%d: ", second, usec, level_names[level], file, line);
#endif
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    fflush(stderr);
  }

  /* Log to file */
  if (L.fp) {
    va_list args;
    fprintf(L.fp, "[%ld.%06ld]: %-5s %s:%d: ", second, usec, level_names[level], file, line);
    va_start(args, fmt);
    vfprintf(L.fp, fmt, args);
    va_end(args);
    fprintf(L.fp, "\n");
    fflush(L.fp);
  }
  /* Release lock */
  unlock();
}
