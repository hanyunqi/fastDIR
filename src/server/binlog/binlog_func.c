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
#include "sf/sf_global.h"
#include "../server_global.h"
#include "binlog_func.h"

int binlog_buffer_init(ServerBinlogBuffer *buffer)
{
    buffer->buff = (char *)malloc(BINLOG_BUFFER_SIZE);
    if (buffer->buff == NULL) {
        logError("file: "__FILE__", line: %d, "
                "malloc %d bytes fail", __LINE__,
                BINLOG_BUFFER_SIZE);
        return ENOMEM;
    }

    buffer->current = buffer->buff;
    buffer->size = BINLOG_BUFFER_SIZE;
    buffer->length = 0;
    return 0;
}
