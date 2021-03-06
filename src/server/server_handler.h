//server_handler.h

#ifndef FDIR_SERVER_HANDLER_H
#define FDIR_SERVER_HANDLER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fastcommon/fast_task_queue.h"
#include "server_types.h"

#ifdef __cplusplus
extern "C" {
#endif

int server_handler_init();
int server_handler_destroy();
int server_deal_task(struct fast_task_info *task);
void server_task_finish_cleanup(struct fast_task_info *task);
void *server_alloc_thread_extra_data(const int thread_index);
int server_thread_loop(struct nio_thread_data *thread_data);

int server_add_to_delay_free_queue(ServerDelayFreeContext *pContext,
        void *ptr, server_free_func free_func, const int delay_seconds);

int server_add_to_delay_free_queue_ex(ServerDelayFreeContext *pContext,
        void *ctx, void *ptr, server_free_func_ex free_func_ex,
        const int delay_seconds);

static inline int server_expect_body_length(
        ServerTaskContext *task_context,
        const int expect_body_length)
{
    if (task_context->request.header.body_len != expect_body_length) {
        task_context->response.error.length = sprintf(
                task_context->response.error.message,
                "request body length: %d != %d",
                task_context->request.header.body_len, expect_body_length);
        return EINVAL;
    }

    return 0;
}

static inline int server_check_min_body_length(
        ServerTaskContext *task_context,
        const int min_body_length)
{
    if (task_context->request.header.body_len < min_body_length) {
        task_context->response.error.length = sprintf(
                task_context->response.error.message,
                "request body length: %d < %d",
                task_context->request.header.body_len, min_body_length);
        return EINVAL;
    }

    return 0;
}

static inline int server_check_max_body_length(
        ServerTaskContext *task_context,
        const int max_body_length)
{
    if (task_context->request.header.body_len > max_body_length) {
        task_context->response.error.length = sprintf(
                task_context->response.error.message,
                "request body length: %d > %d",
                task_context->request.header.body_len, max_body_length);
        return EINVAL;
    }

    return 0;
}

static inline int server_check_body_length(
        ServerTaskContext *task_context,
        const int min_body_length, const int max_body_length)
{
    int result;
    if ((result=server_check_min_body_length(task_context,
            min_body_length)) != 0)
    {
        return result;
    }
    return server_check_max_body_length(task_context, max_body_length);
}

#ifdef __cplusplus
}
#endif

#endif
