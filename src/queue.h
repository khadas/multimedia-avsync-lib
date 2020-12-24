/*
 * Copyright (c) 2020 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description: fifo implementation
 * Author: song.zhao@amlogic.com
 */

#ifndef _AML_QUEUE_H_
#define _AML_QUEUE_H_

#include <stdint.h>

void* create_q(int max_len);
void destroy_q(void * queue);
int queue_item(void *queue, void * item);
/*  cnt 0 for frist one in fifo, cnt 1 for 2nd one in fifo, etc */
int peek_item(void *queue, void** p_item, uint32_t cnt);
int dqueue_item(void *queue, void** p_item);
int queue_size(void *queue);

#endif
