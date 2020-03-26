#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <pthread.h>
#include "fastcommon/logger.h"
#include "fastcommon/sockopt.h"
#include "fastcommon/shared_func.h"
#include "fastcommon/pthread_func.h"
#include "fastcommon/ioevent_loop.h"
#include "sf/sf_global.h"
#include "sf/sf_nio.h"
#include "common/fdir_proto.h"
#include "../server_global.h"
#include "binlog_func.h"
#include "binlog_reader.h"
#include "binlog_producer.h"
#include "binlog_write_thread.h"
#include "replica_consumer_thread.h"

static void *deal_binlog_thread_func(void *arg);

static void release_record_buffer(ServerBinlogRecordBuffer *rbuffer)
{
    if (__sync_sub_and_fetch(&rbuffer->reffer_count, 1) == 0) {
        /*
        logInfo("file: "__FILE__", line: %d, "
                "free record buffer: %p", __LINE__, rbuffer);
                */

        common_blocked_queue_push((struct common_blocked_queue *)
                rbuffer->args, rbuffer);
    }
}

static int alloc_record_buffer(ServerBinlogRecordBuffer *rb,
        const int buffer_size)
{
    rb->release_func = release_record_buffer;
    return fast_buffer_init_ex(&rb->buffer, buffer_size);
}

static void replay_done_callback(const int result,
        FDIRBinlogRecord *record, void *args)
{
    ReplicaConsumerThreadContext *ctx;
    RecordProcessResult *r;
    bool notify;

    ctx = (ReplicaConsumerThreadContext *)args;
    r = fast_mblock_alloc_object(&ctx->result_allocater);
    if (r != NULL) {
        r->err_no = result;
        r->data_version = record->data_version;
        common_blocked_queue_push_ex(&ctx->queues.result, r, &notify);
        if (notify) {
            iovent_notify_thread(ctx->task->thread_data);
        }
    }
}
   
ReplicaConsumerThreadContext *replica_consumer_thread_init(
        struct fast_task_info *task, const int buffer_size, int *err_no)
{
#define BINLOG_REPLAY_BATCH_SIZE  32

    ReplicaConsumerThreadContext *ctx;
    ServerBinlogRecordBuffer *rbuffer;
    int i;

    ctx = (ReplicaConsumerThreadContext *)malloc(
            sizeof(ReplicaConsumerThreadContext));
    if (ctx == NULL) {

        logError("file: "__FILE__", line: %d, "
                "malloc %d bytes fail", __LINE__, (int)
                sizeof(ReplicaConsumerThreadContext));
        *err_no = ENOMEM;
        return NULL;
    }

    memset(ctx, 0, sizeof(ReplicaConsumerThreadContext));
    if ((*err_no=fast_mblock_init_ex(&ctx->result_allocater,
                    sizeof(RecordProcessResult), 8192,
                    NULL, NULL, true)) != 0)
    {
        return NULL;
    }

    ctx->runnings[0] = ctx->runnings[1] = false;
    ctx->continue_flag = true;
    ctx->task = task;

    if ((*err_no=binlog_replay_init_ex(&ctx->replay_ctx,
                    replay_done_callback, ctx,
                    BINLOG_REPLAY_BATCH_SIZE)) != 0)
    {
        return NULL;
    }

    if ((*err_no=common_blocked_queue_init_ex(&ctx->queues.free,
                    REPLICA_CONSUMER_THREAD_INPUT_BUFFER_COUNT)) != 0)
    {
        return NULL;
    }

    if ((*err_no=common_blocked_queue_init_ex(&ctx->queues.input,
                    REPLICA_CONSUMER_THREAD_INPUT_BUFFER_COUNT)) != 0)
    {
        return NULL;
    }
    if ((*err_no=common_blocked_queue_init_ex(&ctx->queues.result,
                    8192)) != 0)
    {
        return NULL;
    }

    rbuffer = ctx->binlog_buffers;
    for (i=0; i<REPLICA_CONSUMER_THREAD_INPUT_BUFFER_COUNT; i++, rbuffer++) {
        if ((*err_no=alloc_record_buffer(rbuffer, buffer_size)) != 0) {
            return NULL;
        }
        rbuffer->args = &ctx->queues.free;
        common_blocked_queue_push(&ctx->queues.free, rbuffer);
    }

    if ((*err_no=fc_create_thread(&ctx->tids[0], deal_binlog_thread_func,
        ctx, SF_G_THREAD_STACK_SIZE)) != 0)
    {
        return NULL;
    }

    return ctx;
}

void replica_consumer_thread_terminate(ReplicaConsumerThreadContext *ctx)
{
    int count;
    int i;

    ctx->continue_flag = false;
    common_blocked_queue_terminate(&ctx->queues.free);
    common_blocked_queue_terminate(&ctx->queues.input);
    common_blocked_queue_terminate(&ctx->queues.result);

    count = 0;
    while ((ctx->runnings[0] || ctx->runnings[1]) && count++ < 10) {
        usleep(200);
    }

    if (ctx->runnings[0] || ctx->runnings[1]) {
        logWarning("file: "__FILE__", line: %d, "
                "wait thread exit timeout", __LINE__);
    }
    for (i=0; i<REPLICA_CONSUMER_THREAD_BUFFER_COUNT; i++) {
        fast_buffer_destroy(&ctx->binlog_buffers[i].buffer);
    }

    common_blocked_queue_destroy(&ctx->queues.free);
    common_blocked_queue_destroy(&ctx->queues.input);
    common_blocked_queue_destroy(&ctx->queues.result);

    binlog_replay_destroy(&ctx->replay_ctx);

    free(ctx);
    logInfo("file: "__FILE__", line: %d, "
            "replica_consumer_thread_terminated", __LINE__);
}

static inline int push_to_replica_consumer_queues(
        ReplicaConsumerThreadContext *ctx,
        ServerBinlogRecordBuffer *rbuffer)
{
    int result;

    __sync_add_and_fetch(&rbuffer->reffer_count, 2);
    if ((result=push_to_binlog_write_queue(rbuffer)) != 0) {
        logCrit("file: "__FILE__", line: %d, "
                "push_to_binlog_write_queue fail, program exit!",
                __LINE__);
        SF_G_CONTINUE_FLAG = false;
        return result;
    }

    return common_blocked_queue_push(&ctx->queues.input, rbuffer);
}

int deal_replica_push_request(ReplicaConsumerThreadContext *ctx,
        char *binlog_buff, const int length,
        const uint64_t last_data_version)
{
    ServerBinlogRecordBuffer *rb = NULL;
    int result;
    int count;

    count = 0;
    while (ctx->continue_flag) {
        rb = (ServerBinlogRecordBuffer *)common_blocked_queue_pop_ex(
                &ctx->queues.free, false);
        if (rb != NULL) {
            break;
        }

        ++count;
        usleep(100);
    }

    if (count > 0) {
        logWarning("file: "__FILE__", line: %d, "
                "alloc record buffer after %d times sleep",
                __LINE__, count);
    }
    if (rb == NULL) {
        return EAGAIN;
    }

    if (rb->buffer.alloc_size < length) {
        rb->buffer.length = 0;
        if ((result=fast_buffer_set_capacity(&rb->buffer, length)) != 0) {
            return result;
        }
        logDebug("file: "__FILE__", line: %d, "
                "data length: %d, expand buffer size to %d",
                __LINE__, length, rb->buffer.alloc_size);
    } else if ((rb->buffer.alloc_size > 2 * BINLOG_BUFFER_INIT_SIZE) &&
            (length * 10 < rb->buffer.alloc_size))
    {
        rb->buffer.length = 0;
        if ((result=fast_buffer_set_capacity(&rb->buffer,
                        length > BINLOG_BUFFER_INIT_SIZE ?
                        length : BINLOG_BUFFER_INIT_SIZE)) != 0) {
            return result;
        }
        logDebug("file: "__FILE__", line: %d, "
                "data length: %d, shrink buffer size to %d",
                __LINE__, length, rb->buffer.alloc_size);
    }

    rb->data_version = last_data_version;
    rb->buffer.length = length;
    memcpy(rb->buffer.data, binlog_buff, rb->buffer.length);
    return push_to_replica_consumer_queues(ctx, rb);
}

int deal_replica_push_result(ReplicaConsumerThreadContext *ctx)
{
    struct common_blocked_node *node;
    struct common_blocked_node *current;
    struct common_blocked_node *last;
    RecordProcessResult *r;
    char *p;
    int count;

    if (!(ctx->task->offset == 0 && ctx->task->length == 0)) {
        return 0;
    }

    if ((node=common_blocked_queue_try_pop_all_nodes(
                    &ctx->queues.result)) == NULL)
    {
        return EAGAIN;
    }

    count = 0;
    p = ctx->task->data + sizeof(FDIRProtoHeader) +
        sizeof(FDIRProtoPushBinlogRespBodyHeader);

    current = node;
    do {
        r = (RecordProcessResult *)current->data;

        long2buff(r->data_version, ((FDIRProtoPushBinlogRespBodyPart *)
                    p)->data_version);
        short2buff(r->err_no, ((FDIRProtoPushBinlogRespBodyPart *)p)->
                err_no);
        p += sizeof(FDIRProtoPushBinlogRespBodyPart);

        fast_mblock_free_object(&ctx->result_allocater, r);
        ++count;

        if ((p - ctx->task->data) + sizeof(FDIRProtoPushBinlogRespBodyPart) >
                ctx->task->size)
        {
            last = current;
            current = current->next;

            last->next = NULL;
            if (current != NULL) {
                common_blocked_queue_return_nodes(
                        &ctx->queues.result, current);
            }
            break;
        }

        current = current->next;
    } while (current != NULL);
    common_blocked_queue_free_all_nodes(&ctx->queues.result, node);

    int2buff(count, ((FDIRProtoPushBinlogRespBodyHeader *)
                (ctx->task->data + sizeof(FDIRProtoHeader)))->count);

    ctx->task->length = p - ctx->task->data;
    FDIR_PROTO_SET_HEADER((FDIRProtoHeader *)ctx->task->data,
            FDIR_REPLICA_PROTO_PUSH_BINLOG_RESP,
            ctx->task->length - sizeof(FDIRProtoHeader));
    sf_send_add_event(ctx->task);
    return 0;
}

static void *deal_binlog_thread_func(void *arg)
{
    ReplicaConsumerThreadContext *ctx;
    struct common_blocked_node *node;
    struct common_blocked_node *current;
    ServerBinlogRecordBuffer *rb;

    logInfo("file: "__FILE__", line: %d, "
            "deal_binlog_thread_func start", __LINE__);

    ctx = (ReplicaConsumerThreadContext *)arg;
    ctx->runnings[0] = true;
    while (ctx->continue_flag) {
        node = common_blocked_queue_pop_all_nodes(&ctx->queues.input);
        if (node == NULL) {
            continue;
        }

        current = node;
        do {
            /*
               logInfo("file: "__FILE__", line: %d, "
               "replay binlog buffer length: %d", __LINE__, rb->buffer.length);
             */

            rb = (ServerBinlogRecordBuffer *)current->data;
            binlog_replay_deal_buffer(&ctx->replay_ctx,
                    rb->buffer.data, rb->buffer.length);

            rb->release_func(rb);
            current = current->next;
        } while (current != NULL);
        common_blocked_queue_free_all_nodes(&ctx->queues.input, node);
    }

    ctx->runnings[0] = false;
    return NULL;
}
