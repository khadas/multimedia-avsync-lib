/*
 * Copyright (c) 2020 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description: fifo implementation for single reader single writer
 * Author: song.zhao@amlogic.com
 */
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include "aml_avsync.h"
#include "queue.h"

struct queue {
    int max_len;
    int ri; //read index
    int wi; //write index
    int total_num;
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
    q->total_num = 0;
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

    if (!q || q->total_num == q->max_len)
        return -1;
    q->items[q->wi] = item;
    if (q->wi == q->max_len - 1)
        q->wi = 0;
    else
        q->wi++;
    q->total_num++;

    return 0;
}

int peek_item(void *queue, void** p_item, uint32_t cnt)
{
    struct queue *q = queue;
    int32_t index;

    if (!q || !q->total_num || q->total_num <= cnt)
        return -1;

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

    if (!q || !q->total_num)
        return -1;
    *p_item = q->items[q->ri];
    if (q->ri == q->max_len - 1)
        q->ri = 0;
    else
        q->ri++;
    q->total_num--;

    return 0;
}

int queue_size(void *queue)
{
    struct queue *q = queue;

    if (!q)
        return -1;
    return q->total_num;
}
