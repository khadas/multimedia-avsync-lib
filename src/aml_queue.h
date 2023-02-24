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
