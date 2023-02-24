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
 * Description: fifo implementation for single reader single writer
 * Author: song.zhao@amlogic.com
 */
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include "aml_avsync.h"
#include "aml_queue.h"

struct queue {
    int max_len;
    int ri; //read index
    int wi; //write index
    void **items;
};

void* create_q(int max_len)
{
    struct queue *q;

    if (max_len <= 0) {
        printf("%s %d invalid max_len:%d\n",
                __func__, __LINE__, max_len);
        return NULL;
    }

    q = (struct queue*)calloc(1, sizeof(*q));
    if (!q) {
        printf("%s %d OOM\n", __func__, __LINE__);
        return NULL;
    }
    q->items = (void **)calloc(max_len, sizeof(void *));
    if (!q->items) {
        printf("%s %d OOM\n", __func__, __LINE__);
        free(q);
        return NULL;
    }

    q->max_len = max_len;
    q->ri = q->wi = 0;
    return q;
}

void destroy_q(void * queue)
{
    struct queue *q = queue;

    if (!q)
        return;
    free(q->items);
    free(q);
}

int queue_item(void *queue, void * item)
{
    struct queue *q = queue;
    int fullness;

    if (!q)
        return -1;
    fullness = q->wi - q->ri;
    if (fullness < 0) fullness += q->max_len;
    if (fullness >= q->max_len - 1)
        return -1; // not enough space

    q->items[q->wi] = item;
    if (q->wi == q->max_len - 1)
        q->wi = 0;
    else
        q->wi++;
    return 0;
}

int peek_item(void *queue, void** p_item, uint32_t cnt)
{
    struct queue *q = queue;
    int32_t index;
    int fullness;

    if (!q)
        return -1;

    fullness = q->wi - q->ri;
    if (fullness < 0) fullness += q->max_len;
    if (fullness == 0 || fullness <= cnt)
       return -1; //no enough to peek
    index = q->ri;
    index += cnt;
    if (index >= q->max_len)
        index -= q->max_len;
    *p_item = q->items[index];

    return 0;
}

int dqueue_item(void *queue, void** p_item)
{
    struct queue *q = queue;
    int fullness;

    if (!q)
        return -1;
    fullness = q->wi - q->ri;
    if (fullness < 0) fullness += q->max_len;
    if (fullness == 0)
        return -1; //empty
    *p_item = q->items[q->ri];
    if (q->ri == q->max_len - 1)
        q->ri = 0;
    else
        q->ri++;
    return 0;
}

int queue_size(void *queue)
{
    struct queue *q = queue;
    int fullness;

    if (!q)
        return -1;
    fullness = q->wi - q->ri;
    if (fullness < 0) fullness += q->max_len;
    return fullness;
}
