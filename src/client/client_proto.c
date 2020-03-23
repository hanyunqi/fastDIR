#include <sys/stat.h>
#include <limits.h>
#include "fastcommon/ini_file_reader.h"
#include "fastcommon/shared_func.h"
#include "fastcommon/logger.h"
#include "fastcommon/sockopt.h"
#include "fastcommon/connection_pool.h"
#include "fdir_proto.h"
#include "client_global.h"
#include "client_proto.h"

static inline void init_client_buffer(FDIRClientBuffer *buffer)
{
    buffer->buff = buffer->fixed;
    buffer->size = sizeof(buffer->fixed);
}

int fdir_client_dentry_array_init(FDIRClientDentryArray *array)
{
    array->alloc = array->count = 0;
    array->entries = NULL;
    init_client_buffer(&array->buffer);
    array->name_allocator.used = array->name_allocator.inited = false;
    return 0;
}

void fdir_client_dentry_array_free(FDIRClientDentryArray *array)
{
    if (array->buffer.buff != array->buffer.fixed) {
        free(array->buffer.buff);
        init_client_buffer(&array->buffer);
    }

    if (array->entries != NULL) {
        free(array->entries);
        array->entries = NULL;
        array->alloc = array->count = 0;
    }

    if (array->name_allocator.inited) {
        array->name_allocator.inited = false;
        fast_mpool_destroy(&array->name_allocator.mpool);
    }
}

static int client_check_set_proto_dentry(const FDIRDEntryFullName *entry_info,
        FDIRProtoDEntryInfo *entry_proto)
{
    if (entry_info->ns.len <= 0 || entry_info->ns.len > NAME_MAX) {
        logError("file: "__FILE__", line: %d, "
                "invalid namespace length: %d, which <= 0 or > %d",
                __LINE__, entry_info->ns.len, NAME_MAX);
        return EINVAL;
    }

    if (entry_info->path.len <= 0 || entry_info->path.len > PATH_MAX) {
        logError("file: "__FILE__", line: %d, "
                "invalid path length: %d, which <= 0 or > %d",
                __LINE__, entry_info->path.len, PATH_MAX);
        return EINVAL;
    }

    entry_proto->ns_len = entry_info->ns.len;
    short2buff(entry_info->path.len, entry_proto->path_len);
    memcpy(entry_proto->ns_str, entry_info->ns.str, entry_info->ns.len);
    memcpy(entry_proto->ns_str + entry_info->ns.len,
            entry_info->path.str, entry_info->path.len);
    return 0;
}

static inline int make_connection(ConnectionInfo *conn)
{
    if (conn->sock >= 0) {
        return 0;
    }

    return conn_pool_connect_server(conn, g_client_global_vars.
            network_timeout);
}

static ConnectionInfo *get_connection(FDIRClientContext *client_ctx,
        int *err_no)
{
    int index;
    int i;
    ConnectionInfo *conn;

    index = rand() % client_ctx->server_group.count;
    conn = client_ctx->server_group.servers + index;
    if ((*err_no=make_connection(conn)) == 0) {
        return conn;
    }

    i = (index + 1) % client_ctx->server_group.count;
    while (i != index) {
        conn = client_ctx->server_group.servers + i;
        if ((*err_no=make_connection(conn)) == 0) {
            return conn;
        }

        i = (i + 1) % client_ctx->server_group.count;
    }
    return NULL;
}

static ConnectionInfo *match_connection(FDIRClientContext *client_ctx,
        FDIRClientServerEntry *server)
{
    ConnectionInfo *conn;
    ConnectionInfo *end;

    end = client_ctx->server_group.servers +
        client_ctx->server_group.count;
    for (conn=client_ctx->server_group.servers; conn<end; conn++) {
        if (strcmp(conn->ip_addr, server->ip_addr) == 0 &&
                conn->port == server->port)
        {
            return conn;
        }
    }

    return NULL;
}

static ConnectionInfo *get_master_connection(FDIRClientContext *client_ctx,
        int *err_no)
{
    ConnectionInfo *conn; 
    ConnectionInfo *mconn; 
    FDIRClientServerEntry master;

    if ((conn=get_connection(client_ctx, err_no)) == NULL) {
        return NULL;
    }

    if ((*err_no=fdir_client_get_master(client_ctx, &master)) != 0) {
        return NULL;
    }

    if ((mconn=match_connection(client_ctx, &master)) == NULL) {
        *err_no = ENOENT;
        return NULL;
    }
    if (mconn == conn) {
        return mconn;
    }

    if ((*err_no=make_connection(mconn)) != 0) {
        return NULL;
    }

    return mconn;
}

static ConnectionInfo *get_readable_connection(
        FDIRClientContext *client_ctx, int *err_no)
{
    ConnectionInfo *conn; 
    ConnectionInfo *sconn; 
    FDIRClientServerEntry server;

    if ((conn=get_connection(client_ctx, err_no)) == NULL) {
        return NULL;
    }

    if ((*err_no=fdir_client_get_readable_server(
                    client_ctx, &server)) != 0)
    {
        return NULL;
    }

    if ((sconn=match_connection(client_ctx, &server)) == NULL) {
        *err_no = ENOENT;
        return NULL;
    }
    if (sconn == conn) {
        return sconn;
    }

    if ((*err_no=make_connection(sconn)) != 0) {
        return NULL;
    }

    return sconn;
}

static inline void log_network_error_ex(FDIRResponseInfo *response,
        const ConnectionInfo *conn, const int result, const int line)
{
    if (response->error.length > 0) {
        logError("file: "__FILE__", line: %d, "
                "server %s:%d, %s", line,
                conn->ip_addr, conn->port,
                response->error.message);
    } else {
        logError("file: "__FILE__", line: %d, "
                "communicate with dir server %s:%d fail, "
                "errno: %d, error info: %s", line,
                conn->ip_addr, conn->port,
                result, STRERROR(result));
    }
}

#define log_network_error(response, conn, result) \
        log_network_error_ex(response, conn, result, __LINE__)

int fdir_client_create_dentry(FDIRClientContext *client_ctx,
        const FDIRDEntryFullName *entry_info, const int flags,
        const mode_t mode)
{
    FDIRProtoHeader *header;
    FDIRProtoCreateDEntryBody *entry_body;
    int out_bytes;
    ConnectionInfo *conn;
    char out_buff[sizeof(FDIRProtoHeader) + sizeof(FDIRProtoCreateDEntryBody)
        + NAME_MAX + PATH_MAX];
    FDIRResponseInfo response;
    int result;

    header = (FDIRProtoHeader *)out_buff;
    entry_body = (FDIRProtoCreateDEntryBody *)(out_buff +
            sizeof(FDIRProtoHeader));
    if ((result=client_check_set_proto_dentry(entry_info,
                    &entry_body->dentry)) != 0)
    {
        return result;
    }

    if ((conn=get_master_connection(client_ctx, &result)) == NULL) {
        return result;
    }

    int2buff(flags, entry_body->front.flags);
    int2buff(mode, entry_body->front.mode);
    out_bytes = sizeof(FDIRProtoHeader) + sizeof(FDIRProtoCreateDEntryBody)
        + entry_info->ns.len + entry_info->path.len;
    FDIR_PROTO_SET_HEADER(header, FDIR_SERVICE_PROTO_CREATE_DENTRY,
            out_bytes - sizeof(FDIRProtoHeader));

    response.error.length = 0;
    response.error.message[0] = '\0';
    if ((result=fdir_send_and_recv_none_body_response(conn, out_buff,
                    out_bytes, &response, g_client_global_vars.
                    network_timeout, FDIR_PROTO_ACK)) != 0)
    {
        log_network_error(&response, conn, result);
    }

    if ((result != 0) && is_network_error(result)) {
        conn_pool_disconnect_server(conn);
    }

    return result;
}

int fdir_client_remove_dentry(FDIRClientContext *client_ctx,
        const FDIRDEntryFullName *entry_info)
{
    FDIRProtoHeader *header;
    FDIRProtoRemoveDEntry *entry_body;
    int out_bytes;
    ConnectionInfo *conn;
    char out_buff[sizeof(FDIRProtoHeader) + sizeof(FDIRProtoRemoveDEntry)
        + NAME_MAX + PATH_MAX];
    FDIRResponseInfo response;
    int result;

    header = (FDIRProtoHeader *)out_buff;
    entry_body = (FDIRProtoRemoveDEntry *)(out_buff +
            sizeof(FDIRProtoHeader));
    if ((result=client_check_set_proto_dentry(entry_info,
                    &entry_body->dentry)) != 0)
    {
        return result;
    }

    if ((conn=get_master_connection(client_ctx, &result)) == NULL) {
        return result;
    }

    out_bytes = sizeof(FDIRProtoHeader) + sizeof(FDIRProtoRemoveDEntry)
        + entry_info->ns.len + entry_info->path.len;
    FDIR_PROTO_SET_HEADER(header, FDIR_SERVICE_PROTO_REMOVE_DENTRY,
            out_bytes - sizeof(FDIRProtoHeader));

    response.error.length = 0;
    response.error.message[0] = '\0';
    if ((result=fdir_send_and_recv_none_body_response(conn, out_buff,
                    out_bytes, &response, g_client_global_vars.
                    network_timeout, FDIR_PROTO_ACK)) != 0)
    {
        log_network_error(&response, conn, result);
    }

    if ((result != 0) && is_network_error(result)) {
        conn_pool_disconnect_server(conn);
    }

    return result;
}

static int check_realloc_client_buffer(FDIRResponseInfo *response,
        FDIRClientBuffer *buffer)
{
    char *new_buff;
    int alloc_size;

    if (response->header.body_len <= buffer->size) {
        return 0;
    }

    alloc_size = 2 * buffer->size;
    while (alloc_size < response->header.body_len) {
        alloc_size *= 2;
    }
    new_buff = (char *)malloc(alloc_size);
    if (new_buff == NULL) {
        response->error.length = sprintf(response->error.message,
                "malloc %d bytes fail", alloc_size);
        return ENOMEM;
    }

    if (buffer->buff != buffer->fixed) {
        free(buffer->buff);
    }

    buffer->buff = new_buff;
    buffer->size = alloc_size;
    return 0;
}

static int check_realloc_dentry_array(FDIRResponseInfo *response,
        FDIRClientDentryArray *array, const int inc_count)
{
    FDIRClientDentry *new_entries;
    int target_count;
    int new_alloc;
    int bytes;

    target_count = array->count + inc_count;
    if (target_count <= array->alloc) {
        return 0;
    }

    if (array->alloc == 0) {
        new_alloc = 256;
    } else {
        new_alloc = 2 * array->alloc;
    }
    while (new_alloc < target_count) {
        new_alloc *= 2;
    }

    bytes = sizeof(FDIRClientDentry) * new_alloc;
    new_entries = (FDIRClientDentry *)malloc(bytes);
    if (new_entries == NULL) {
        response->error.length = sprintf(response->error.message,
                "malloc %d bytes fail", bytes);
        return ENOMEM;
    }

    if (array->count > 0) {
        memcpy(new_entries, array->entries,
                sizeof(FDIRClientDentry) * array->count);
        free(array->entries);
    }
    array->entries = new_entries;
    array->alloc = new_alloc;
    return 0;
}

static int parse_list_dentry_response_body(ConnectionInfo *conn,
        FDIRResponseInfo *response, FDIRClientDentryArray *array,
        string_t *next_token)
{
    FDIRProtoListDEntryRespBodyHeader *body_header;
    FDIRProtoListDEntryRespBodyPart *part;
    FDIRClientDentry *dentry;
    FDIRClientDentry *start;
    FDIRClientDentry *end;
    char *p;
    int result;
    int entry_len;
    int count;

    if (response->header.body_len < sizeof(FDIRProtoListDEntryRespBodyHeader)) {
        response->error.length = snprintf(response->error.message,
                sizeof(response->error.message),
                "server %s:%d response body length: %d < expected: %d",
                conn->ip_addr, conn->port, response->header.body_len,
                (int)sizeof(FDIRProtoListDEntryRespBodyHeader));
        return EINVAL;
    }

    body_header = (FDIRProtoListDEntryRespBodyHeader *)array->buffer.buff;
    count = buff2int(body_header->count);
    next_token->str = body_header->token;
    if (body_header->is_last) {
        next_token->len = 0;
    } else {
        next_token->len = sizeof(body_header->token);

        if (!array->name_allocator.inited) {
            if ((result=fast_mpool_init(&array->name_allocator.mpool,
                            64 * 1024, 8)) != 0)
            {
                response->error.length = sprintf(response->error.message,
                        "fast_mpool_init fail");
                return result;
            }
            array->name_allocator.inited = true;
        }
        array->name_allocator.used = true;
    }

    if ((result=check_realloc_dentry_array(response, array, count)) != 0) {
        return result;
    }

    p = array->buffer.buff + sizeof(FDIRProtoListDEntryRespBodyHeader);
    start = array->entries + array->count;
    end = start + count;
    for (dentry=start; dentry<end; dentry++) {
        part = (FDIRProtoListDEntryRespBodyPart *)p;
        entry_len = sizeof(FDIRProtoListDEntryRespBodyPart) + part->name_len;
        if ((p - array->buffer.buff) + entry_len > response->header.body_len) {
            response->error.length = snprintf(response->error.message,
                    sizeof(response->error.message),
                    "server %s:%d response body length exceeds header's %d",
                    conn->ip_addr, conn->port, response->header.body_len);
            return EINVAL;
        }


        if (body_header->is_last) {
            FC_SET_STRING_EX(dentry->name, part->name_str, part->name_len);
        } else if ((result=fast_mpool_alloc_string_ex(&array->name_allocator.mpool,
                        &dentry->name, part->name_str, part->name_len)) != 0)
        {
            response->error.length = sprintf(response->error.message,
                    "strdup %d bytes fail", part->name_len);
            return result;
        }

        p += entry_len;
    }

    if ((int)(p - array->buffer.buff) != response->header.body_len) {
        response->error.length = snprintf(response->error.message,
                sizeof(response->error.message),
                "server %s:%d response body length: %d != header's %d",
                conn->ip_addr, conn->port, (int)(p - array->buffer.buff),
                response->header.body_len);
        return EINVAL;
    }

    array->count += count;
    return 0;
}

static int deal_list_dentry_response_body(ConnectionInfo *conn,
        FDIRResponseInfo *response, FDIRClientDentryArray *array,
        string_t *next_token)
{
    int result;
    if ((result=check_realloc_client_buffer(response, &array->buffer)) != 0) {
        return result;
    }

    if ((result=tcprecvdata_nb(conn->sock, array->buffer.buff,
                    response->header.body_len, g_client_global_vars.
                    network_timeout)) != 0)
    {
        response->error.length = snprintf(response->error.message,
                sizeof(response->error.message),
                "recv from server %s:%d fail, "
                "errno: %d, error info: %s",
                conn->ip_addr, conn->port,
                result, STRERROR(result));
        return result;
    }

    return parse_list_dentry_response_body(conn, response, array, next_token);
}

static int do_list_dentry_next(ConnectionInfo *conn, string_t *next_token,
        FDIRResponseInfo *response, FDIRClientDentryArray *array)
{
    FDIRProtoHeader *header;
    FDIRProtoListDEntryNextBody *entry_body;
    char out_buff[sizeof(FDIRProtoHeader) + sizeof(FDIRProtoListDEntryNextBody)];
    int out_bytes;
    int result;

    memset(out_buff, 0, sizeof(out_buff));
    header = (FDIRProtoHeader *)out_buff;
    entry_body = (FDIRProtoListDEntryNextBody *)
        (out_buff + sizeof(FDIRProtoHeader));
    out_bytes = sizeof(FDIRProtoHeader) +
        sizeof(FDIRProtoListDEntryNextBody);
    FDIR_PROTO_SET_HEADER(header, FDIR_SERVICE_PROTO_LIST_DENTRY_NEXT_REQ,
            out_bytes - sizeof(FDIRProtoHeader));
    memcpy(entry_body->token, next_token->str, next_token->len);
    int2buff(array->count, entry_body->offset);
    if ((result=fdir_send_and_check_response_header(conn, out_buff,
                    out_bytes, response, g_client_global_vars.
                    network_timeout, FDIR_SERVICE_PROTO_LIST_DENTRY_RESP)) == 0)
    {
        return deal_list_dentry_response_body(conn, response,
                    array, next_token);
    }

    return result;
}

static int deal_list_dentry_response(ConnectionInfo *conn,
        FDIRResponseInfo *response, FDIRClientDentryArray *array)
{
    string_t next_token;
    int result;

    if ((result=deal_list_dentry_response_body(conn, response,
                    array, &next_token)) != 0)
    {
        return result;
    }

    while (next_token.len > 0) {
        if ((result=do_list_dentry_next(conn, &next_token,
                        response, array)) != 0) {
            break;
        }
    }

    return result;
}

int fdir_client_list_dentry(FDIRClientContext *client_ctx,
        const FDIRDEntryFullName *entry_info, FDIRClientDentryArray *array)
{
    FDIRProtoHeader *header;
    FDIRProtoListDEntryFirstBody *entry_body;
    int out_bytes;
    ConnectionInfo *conn;
    char out_buff[sizeof(FDIRProtoHeader) + sizeof(FDIRProtoListDEntryFirstBody)
        + NAME_MAX + PATH_MAX];
    FDIRResponseInfo response;
    int result;

    array->count = 0;
    header = (FDIRProtoHeader *)out_buff;
    entry_body = (FDIRProtoListDEntryFirstBody *)(out_buff +
            sizeof(FDIRProtoHeader));
    if ((result=client_check_set_proto_dentry(entry_info,
                    &entry_body->dentry)) != 0)
    {
        return result;
    }

    if ((conn=get_readable_connection(client_ctx, &result)) == NULL) {
        return result;
    }

    out_bytes = sizeof(FDIRProtoHeader) + sizeof(FDIRProtoListDEntryFirstBody)
        + entry_info->ns.len + entry_info->path.len;
    FDIR_PROTO_SET_HEADER(header, FDIR_SERVICE_PROTO_LIST_DENTRY_FIRST_REQ,
            out_bytes - sizeof(FDIRProtoHeader));

    if (array->name_allocator.used) {
        fast_mpool_reset(&array->name_allocator.mpool);  //buffer recycle
        array->name_allocator.used = false;
    }
    response.error.length = 0;
    response.error.message[0] = '\0';
    if ((result=fdir_send_and_check_response_header(conn, out_buff,
                    out_bytes, &response, g_client_global_vars.
                    network_timeout, FDIR_SERVICE_PROTO_LIST_DENTRY_RESP)) == 0)
    {
        result = deal_list_dentry_response(conn, &response, array);
    }

    if (result != 0) {
        log_network_error(&response, conn, result);
        if (is_network_error(result)) {
            conn_pool_disconnect_server(conn);
        }
    }

    return result;
}

int fdir_client_service_stat(ConnectionInfo *conn, FDIRClientServiceStat *stat)
{
    FDIRProtoHeader *header;
    char out_buff[sizeof(FDIRProtoHeader)];
    FDIRResponseInfo response;
    FDIRProtoServiceStatResp stat_resp;
    int result;

    if ((result=make_connection(conn)) != 0) {
        return result;
    }

    header = (FDIRProtoHeader *)out_buff;
    FDIR_PROTO_SET_HEADER(header, FDIR_SERVICE_PROTO_SERVICE_STAT_REQ,
            sizeof(out_buff) - sizeof(FDIRProtoHeader));
    if ((result=fdir_send_and_recv_response(conn, out_buff, sizeof(out_buff),
                    &response, g_client_global_vars.network_timeout,
                    FDIR_SERVICE_PROTO_SERVICE_STAT_RESP,
                    (char *)&stat_resp, sizeof(FDIRProtoServiceStatResp))) != 0)
    {
        log_network_error(&response, conn, result);
        if (is_network_error(result)) {
            conn_pool_disconnect_server(conn);
        }
        return result;
    }

    stat->is_master = stat_resp.is_master;
    stat->status = stat_resp.status;
    stat->server_id = buff2int(stat_resp.server_id);
    stat->connection.current_count = buff2int(
            stat_resp.connection.current_count);
    stat->connection.max_count = buff2int(stat_resp.connection.max_count);

    stat->dentry.current_data_version = buff2long(
            stat_resp.dentry.current_data_version);
    stat->dentry.current_inode_sn = buff2long(
            stat_resp.dentry.current_inode_sn);
    stat->dentry.counters.ns = buff2long(stat_resp.dentry.counters.ns);
    stat->dentry.counters.dir = buff2long(stat_resp.dentry.counters.dir);
    stat->dentry.counters.file = buff2long(stat_resp.dentry.counters.file);

    return 0;
}

int fdir_client_cluster_stat(FDIRClientContext *client_ctx,
        FDIRClientClusterStatEntry *stats, const int size, int *count)
{
    FDIRProtoHeader *header;
    FDIRProtoClusterStatRespBodyHeader *body_header;
    FDIRProtoClusterStatRespBodyPart *body_part;
    FDIRProtoClusterStatRespBodyPart *body_end;
    FDIRClientClusterStatEntry *stat;
    ConnectionInfo *conn;
    char out_buff[sizeof(FDIRProtoHeader)];
    char fixed_buff[8 * 1024];
    char *in_buff;
    FDIRResponseInfo response;
    int result;
    int calc_size;

    if ((conn=get_master_connection(client_ctx, &result)) == NULL) {
        return result;
    }

    header = (FDIRProtoHeader *)out_buff;
    FDIR_PROTO_SET_HEADER(header, FDIR_SERVICE_PROTO_CLUSTER_STAT_REQ,
            sizeof(out_buff) - sizeof(FDIRProtoHeader));

    in_buff = fixed_buff;
    if ((result=fdir_send_and_check_response_header(conn, out_buff,
                    sizeof(out_buff), &response, g_client_global_vars.
                    network_timeout, FDIR_SERVICE_PROTO_CLUSTER_STAT_RESP)) == 0)
    {
        if (response.header.body_len > sizeof(fixed_buff)) {
            in_buff = (char *)malloc(response.header.body_len);
            if (in_buff == NULL) {
                response.error.length = sprintf(response.error.message,
                        "malloc %d bytes fail", response.header.body_len);
                result = ENOMEM;
            }
        }

        if (result == 0) {
            result = tcprecvdata_nb(conn->sock, in_buff,
                    response.header.body_len, g_client_global_vars.
                    network_timeout);
        }
    }

    body_header = (FDIRProtoClusterStatRespBodyHeader *)in_buff;
    body_part = (FDIRProtoClusterStatRespBodyPart *)(in_buff +
            sizeof(FDIRProtoClusterStatRespBodyHeader));
    if (result == 0) {
        *count = buff2int(body_header->count);

        calc_size = sizeof(FDIRProtoClusterStatRespBodyHeader) +
            (*count) * sizeof(FDIRProtoClusterStatRespBodyPart);
        if (calc_size != response.header.body_len) {
            response.error.length = sprintf(response.error.message,
                    "response body length: %d != calculate size: %d, "
                    "server count: %d", response.header.body_len,
                    calc_size, *count);
            result = EINVAL;
        } else if (size < *count) {
            response.error.length = sprintf(response.error.message,
                    "entry size %d too small < %d", size, *count);
            *count = 0;
            result = ENOSPC;
        }
    } else {
        *count = 0;
    }

    if (result != 0) {
        log_network_error(&response, conn, result);
        if (is_network_error(result)) {
            conn_pool_disconnect_server(conn);
        }
        if (in_buff != fixed_buff) {
            if (in_buff != NULL) {
                free(in_buff);
            }
        }
        return result;
    }

    body_end = body_part + (*count);
    for (stat=stats; body_part<body_end; body_part++, stat++) {
        stat->is_master = body_part->is_master;
        stat->status = body_part->status;
        stat->server_id = buff2int(body_part->server_id);
        memcpy(stat->ip_addr, body_part->ip_addr, IP_ADDRESS_SIZE);
        *(stat->ip_addr + IP_ADDRESS_SIZE - 1) = '\0';
        stat->port = buff2short(body_part->port);
    }

    if (in_buff != fixed_buff) {
        free(in_buff);
    }

    return 0;
}

int fdir_client_get_master(FDIRClientContext *client_ctx,
        FDIRClientServerEntry *master)
{
    int result;
    ConnectionInfo *conn;
    FDIRProtoHeader *header;
    FDIRResponseInfo response;
    FDIRProtoGetServerResp server_resp;
    char out_buff[sizeof(FDIRProtoHeader)];

    conn = get_connection(client_ctx, &result);
    if (conn == NULL) {
        return result;
    }

    header = (FDIRProtoHeader *)out_buff;
    FDIR_PROTO_SET_HEADER(header, FDIR_SERVICE_PROTO_GET_MASTER_REQ,
            sizeof(out_buff) - sizeof(FDIRProtoHeader));
    if ((result=fdir_send_and_recv_response(conn, out_buff, sizeof(out_buff),
                    &response, g_client_global_vars.network_timeout,
                    FDIR_SERVICE_PROTO_GET_MASTER_RESP,
                    (char *)&server_resp, sizeof(FDIRProtoGetServerResp))) != 0)
    {
        log_network_error(&response, conn, result);
        if (is_network_error(result)) {
            conn_pool_disconnect_server(conn);
        }
        return result;
    }

    master->server_id = buff2int(server_resp.server_id);
    memcpy(master->ip_addr, server_resp.ip_addr, IP_ADDRESS_SIZE);
    *(master->ip_addr + IP_ADDRESS_SIZE - 1) = '\0';
    master->port = buff2short(server_resp.port);
    return 0;
}

int fdir_client_get_readable_server(FDIRClientContext *client_ctx,
        FDIRClientServerEntry *server)
{
    int result;
    ConnectionInfo *conn;
    FDIRProtoHeader *header;
    FDIRResponseInfo response;
    FDIRProtoGetServerResp server_resp;
    char out_buff[sizeof(FDIRProtoHeader)];

    conn = get_connection(client_ctx, &result);
    if (conn == NULL) {
        return result;
    }

    header = (FDIRProtoHeader *)out_buff;
    FDIR_PROTO_SET_HEADER(header, FDIR_SERVICE_PROTO_GET_READABLE_SERVER_REQ,
            sizeof(out_buff) - sizeof(FDIRProtoHeader));
    if ((result=fdir_send_and_recv_response(conn, out_buff, sizeof(out_buff),
                    &response, g_client_global_vars.network_timeout,
                    FDIR_SERVICE_PROTO_GET_READABLE_SERVER_RESP,
                    (char *)&server_resp, sizeof(FDIRProtoGetServerResp))) != 0)
    {
        log_network_error(&response, conn, result);
        if (is_network_error(result)) {
            conn_pool_disconnect_server(conn);
        }
        return result;
    }

    server->server_id = buff2int(server_resp.server_id);
    memcpy(server->ip_addr, server_resp.ip_addr, IP_ADDRESS_SIZE);
    *(server->ip_addr + IP_ADDRESS_SIZE - 1) = '\0';
    server->port = buff2short(server_resp.port);
    return 0;
}

int fdir_client_get_slaves(FDIRClientContext *client_ctx,
        FDIRClientServerEntry *slaves, const int size, int *count)
{
    FDIRProtoHeader *header;
    FDIRProtoGetSlavesRespBodyHeader *body_header;
    FDIRProtoGetSlavesRespBodyPart *body_part;
    FDIRProtoGetSlavesRespBodyPart *body_end;
    FDIRClientServerEntry *slave;
    ConnectionInfo *conn;
    char out_buff[sizeof(FDIRProtoHeader)];
    char fixed_buff[8 * 1024];
    char *in_buff;
    FDIRResponseInfo response;
    int result;
    int calc_size;

    if ((conn=get_connection(client_ctx, &result)) == NULL) {
        return result;
    }

    header = (FDIRProtoHeader *)out_buff;
    FDIR_PROTO_SET_HEADER(header, FDIR_SERVICE_PROTO_GET_SLAVES_REQ,
            sizeof(out_buff) - sizeof(FDIRProtoHeader));

    in_buff = fixed_buff;
    if ((result=fdir_send_and_check_response_header(conn, out_buff,
                    sizeof(out_buff), &response, g_client_global_vars.
                    network_timeout, FDIR_SERVICE_PROTO_GET_SLAVES_RESP)) == 0)
    {
        if (response.header.body_len > sizeof(fixed_buff)) {
            in_buff = (char *)malloc(response.header.body_len);
            if (in_buff == NULL) {
                response.error.length = sprintf(response.error.message,
                        "malloc %d bytes fail", response.header.body_len);
                result = ENOMEM;
            }
        }

        if (result == 0) {
            result = tcprecvdata_nb(conn->sock, in_buff,
                    response.header.body_len, g_client_global_vars.
                    network_timeout);
        }
    }

    body_header = (FDIRProtoGetSlavesRespBodyHeader *)in_buff;
    body_part = (FDIRProtoGetSlavesRespBodyPart *)(in_buff +
            sizeof(FDIRProtoGetSlavesRespBodyHeader));
    if (result == 0) {
        *count = buff2short(body_header->count);

        calc_size = sizeof(FDIRProtoGetSlavesRespBodyHeader) +
            (*count) * sizeof(FDIRProtoGetSlavesRespBodyPart);
        if (calc_size != response.header.body_len) {
            response.error.length = sprintf(response.error.message,
                    "response body length: %d != calculate size: %d, "
                    "server count: %d", response.header.body_len,
                    calc_size, *count);
            result = EINVAL;
        } else if (size < *count) {
            response.error.length = sprintf(response.error.message,
                    "entry size %d too small < %d", size, *count);
            *count = 0;
            result = ENOSPC;
        }
    } else {
        *count = 0;
    }

    if (result != 0) {
        log_network_error(&response, conn, result);
        if (is_network_error(result)) {
            conn_pool_disconnect_server(conn);
        }
        if (in_buff != fixed_buff) {
            if (in_buff != NULL) {
                free(in_buff);
            }
        }
        return result;
    }

    body_end = body_part + (*count);
    for (slave=slaves; body_part<body_end; body_part++, slave++) {
        slave->server_id = buff2int(body_part->server_id);
        memcpy(slave->ip_addr, body_part->ip_addr, IP_ADDRESS_SIZE);
        *(slave->ip_addr + IP_ADDRESS_SIZE - 1) = '\0';
        slave->port = buff2short(body_part->port);
        slave->status = body_part->status;
    }

    if (in_buff != fixed_buff) {
        free(in_buff);
    }

    return 0;
}
