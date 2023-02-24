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

#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <stdarg.h>

#define LOG_VERSION "0.1.0"

typedef void (*log_LockFn)(void *udata, int lock);

enum { AVS_LOG_TRACE, AVS_LOG_DEBUG, AVS_LOG_INFO, AVS_LOG_WARN, AVS_LOG_ERROR, AVS_LOG_FATAL };

#define log_trace(...) log_log(AVS_LOG_TRACE, __func__, __LINE__, __VA_ARGS__)
#define log_debug(...) log_log(AVS_LOG_DEBUG, __func__, __LINE__, __VA_ARGS__)
#define log_info(...)  log_log(AVS_LOG_INFO,  __func__, __LINE__, __VA_ARGS__)
#define log_warn(...)  log_log(AVS_LOG_WARN,  __func__, __LINE__, __VA_ARGS__)
#define log_error(...) log_log(AVS_LOG_ERROR, __func__, __LINE__, __VA_ARGS__)
#define log_fatal(...) log_log(AVS_LOG_FATAL, __func__, __LINE__, __VA_ARGS__)

void log_set_udata(void *udata);
void log_set_lock(log_LockFn fn);
void log_set_fp(FILE *fp);
void log_set_level(int level);
void log_set_quiet(int enable);

void log_log(int level, const char *file, int line, const char *fmt, ...);

#endif
