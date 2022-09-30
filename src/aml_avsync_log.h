/**
 * Copyright (c) 2017 rxi
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the MIT license. See `log.c` for details.
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
