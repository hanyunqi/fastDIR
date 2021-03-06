#ifndef _CLIENT_TYPES_H
#define _CLIENT_TYPES_H

#include "fastcommon/common_define.h"
#include "fastcommon/connection_pool.h"

typedef struct fdir_dstatus {
    int64_t inode;
    mode_t mode;
    int ctime;  /* create time */
    int mtime;  /* modify time */
    int atime;  /* last access time */
    int64_t size;   /* file size in bytes */
} FDIRDStatus;

typedef struct fdir_server_group {
    int count;
    ConnectionInfo *servers;
} FDIRServerGroup;

typedef struct fdir_slave_group {
    int count;
    int index;  //server index for roundrobin
    ConnectionInfo **servers;
} FDIRSlaveGroup;

typedef struct fdir_server_cluster {
    FDIRServerGroup server_group;
    FDIRSlaveGroup slave_group;
    ConnectionInfo *master;
} FDIRServerCluster;

#endif
