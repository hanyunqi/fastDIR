
#include <sys/stat.h>
#include <limits.h>
#include "fastcommon/ini_file_reader.h"
#include "fastcommon/shared_func.h"
#include "fastcommon/logger.h"
#include "fastcommon/sockopt.h"
#include "fastcommon/md5.h"
#include "fastcommon/local_ip_func.h"
#include "sf/sf_global.h"
#include "sf/sf_service.h"
#include "server_global.h"
#include "cluster_topology.h"
#include "server_func.h"

static int server_load_admin_config(IniContext *ini_context)
{
#define ADMIN_SECTION_NAME "admin"

    char *username;
    char *secret_key;
    char *buff;
    char *p;
    struct {
        int username;
        int secret_key;
    } lengths;
    int bytes;

    //TODO
    return 0;

    if ((username=iniGetRequiredStrValue(ADMIN_SECTION_NAME, "username",
                    ini_context)) == NULL)
    {
        return ENOENT;
    }

    if ((secret_key=iniGetRequiredStrValue(ADMIN_SECTION_NAME, "secret_key",
                    ini_context)) == NULL)
    {
        return ENOENT;
    }

    g_server_global_vars.admin.username.len = strlen(username);
    g_server_global_vars.admin.secret_key.len = strlen(secret_key);

    lengths.username = g_server_global_vars.admin.username.len + 1;
    lengths.secret_key = g_server_global_vars.admin.secret_key.len + 1;

    bytes = lengths.username + lengths.secret_key;
    buff = (char *)malloc(bytes);
    if (buff == NULL) {
        logError("file: "__FILE__", line: %d, "
                "malloc %d bytes fail", __LINE__, bytes);
        return ENOMEM;
    }

    p = buff;
    g_server_global_vars.admin.username.str = p;
    p += lengths.username;

    g_server_global_vars.admin.secret_key.str = p;
    p += lengths.secret_key;

    memcpy(g_server_global_vars.admin.username.str, username, lengths.username);
    memcpy(g_server_global_vars.admin.secret_key.str, secret_key, lengths.secret_key);
    return 0;
}

static int find_myself_in_cluster_config(const char *filename)
{
    const char *local_ip;
    struct {
        const char *ip_addr;
        int port;
    } found;
    FCServerInfo *server;
    FDIRClusterServerInfo *myself;
    int ports[2];
    int count;
    int i;

    count = 0;
    ports[count++] = g_sf_global_vars.inner_port;
    if (g_sf_global_vars.outer_port != g_sf_global_vars.inner_port) {
        ports[count++] = g_sf_global_vars.outer_port;
    }

    found.ip_addr = NULL;
    found.port = 0;
    local_ip = get_first_local_ip();
    while (local_ip != NULL) {
        for (i=0; i<count; i++) {
            server = fc_server_get_by_ip_port(&CLUSTER_CONFIG_CTX,
                    local_ip, ports[i]);
            if (server != NULL) {
                myself = CLUSTER_SERVER_ARRAY.servers +
                    (server - FC_SID_SERVERS(CLUSTER_CONFIG_CTX));
                if (CLUSTER_MYSELF_PTR == NULL) {
                    CLUSTER_MYSELF_PTR = myself;
                } else if (myself != CLUSTER_MYSELF_PTR) {
                    logError("file: "__FILE__", line: %d, "
                            "cluster config file: %s, my ip and port "
                            "in more than one servers, %s:%d in "
                            "server id %d, and %s:%d in server id %d",
                            __LINE__, filename, found.ip_addr, found.port,
                            CLUSTER_MY_SERVER_ID, local_ip,
                            ports[i], myself->server->id);
                    return EEXIST;
                }

                found.ip_addr = local_ip;
                found.port = ports[i];
            }
        }

        local_ip = get_next_local_ip(local_ip);
    }

    if (CLUSTER_MYSELF_PTR == NULL) {
        logError("file: "__FILE__", line: %d, "
                "cluster config file: %s, can't find myself "
                "by my local ip and listen port", __LINE__, filename);
        return ENOENT;
    }

    return 0;
}

static void log_cluster_server_config()
{
    FastBuffer buffer;

    if (fast_buffer_init_ex(&buffer, 1024) != 0) {
        return;
    }
    fc_server_to_config_string(&CLUSTER_CONFIG_CTX, &buffer);
    log_it1(LOG_INFO, buffer.data, buffer.length);
    fast_buffer_destroy(&buffer);

    fc_server_to_log(&CLUSTER_CONFIG_CTX);
}

static int calc_cluster_config_sign()
{
    FastBuffer buffer;
    int result;

    if ((result=fast_buffer_init_ex(&buffer, 1024)) != 0) {
        return result;
    }
    fc_server_to_config_string(&CLUSTER_CONFIG_CTX, &buffer);
    my_md5_buffer(buffer.data, buffer.length, CLUSTER_CONFIG_SIGN_BUF);

    {
    char hex_buff[2 * CLUSTER_CONFIG_SIGN_LEN + 1];
    logInfo("cluster config length: %d, sign: %s", buffer.length,
            bin2hex((const char *)CLUSTER_CONFIG_SIGN_BUF,
                CLUSTER_CONFIG_SIGN_LEN, hex_buff));
    }
    fast_buffer_destroy(&buffer);
    return 0;
}

static int find_group_indexes_in_cluster_config(const char *filename)
{
    CLUSTER_GROUP_INDEX = fc_server_get_group_index(&CLUSTER_CONFIG_CTX,
            "cluster");
    if (CLUSTER_GROUP_INDEX < 0) {
        logError("file: "__FILE__", line: %d, "
                "cluster config file: %s, cluster group not configurated",
                __LINE__, filename);
        return ENOENT;
    }

    SERVICE_GROUP_INDEX = fc_server_get_group_index(&CLUSTER_CONFIG_CTX,
            "service");
    if (SERVICE_GROUP_INDEX < 0) {
        logError("file: "__FILE__", line: %d, "
                "cluster config file: %s, service group not configurated",
                __LINE__, filename);
        return ENOENT;
    }

    return 0;
}

static int init_cluster_server_array()
{
    int bytes;
    FDIRClusterServerInfo *cs;
    FCServerInfo *server;
    FCServerInfo *end;

    bytes = sizeof(FDIRClusterServerInfo) *
        FC_SID_SERVER_COUNT(CLUSTER_CONFIG_CTX);
    CLUSTER_SERVER_ARRAY.servers = (FDIRClusterServerInfo *)malloc(bytes);
    if (CLUSTER_SERVER_ARRAY.servers == NULL) {
        logError("file: "__FILE__", line: %d, "
                "malloc %d bytes fail", __LINE__, bytes);
        return ENOMEM;
    }
    memset(CLUSTER_SERVER_ARRAY.servers, 0, bytes);

    end = FC_SID_SERVERS(CLUSTER_CONFIG_CTX) +
        FC_SID_SERVER_COUNT(CLUSTER_CONFIG_CTX);
    for (server=FC_SID_SERVERS(CLUSTER_CONFIG_CTX),
            cs=CLUSTER_SERVER_ARRAY.servers; server<end; server++, cs++)
    {
        cs->server = server;
    }

    CLUSTER_SERVER_ARRAY.count = FC_SID_SERVER_COUNT(CLUSTER_CONFIG_CTX);
    return 0;
}

static int server_load_cluster_id(IniContext *ini_context, const char *filename)
{
    char *cluster_id;
    char *endptr = NULL;

    cluster_id = iniGetStrValue(NULL, "cluster_id", ini_context);
    if (cluster_id == NULL || *cluster_id == '\0') {
        logError("file: "__FILE__", line: %d, "
                "config file: %s, item \"cluster_id\" not exist or empty",
                __LINE__, filename);
        return ENOENT;
    }

    CLUSTER_ID = strtol(cluster_id, &endptr, 10);
    if (CLUSTER_ID <= 0 || (endptr != NULL && *endptr != '\0')) {
        logError("file: "__FILE__", line: %d, "
                "config file: %s, cluster_id: %s is invalid, "
                "it must be a natural number!", __LINE__,
                filename, cluster_id);
        return EINVAL;
    }

    if (CLUSTER_ID > FDIR_CLUSTER_ID_MAX) {
        logError("file: "__FILE__", line: %d, "
                "config file: %s, cluster_id: %s is too large, "
                "exceeds %d", __LINE__, filename,
                cluster_id, FDIR_CLUSTER_ID_MAX);
        return EINVAL;
    }

    return 0;
}

static int load_cluster_config(IniContext *ini_context, const char *filename)
{
    int result;
    char *cluster_config_filename;
    char full_cluster_filename[PATH_MAX];
    const int min_hosts_each_group = 1;
    const bool share_between_groups = true;

    if ((result=server_load_cluster_id(ini_context, filename)) != 0) {
        return result;
    }

    cluster_config_filename = iniGetStrValue(NULL,
            "cluster_config_filename", ini_context);
    if (cluster_config_filename == NULL || *cluster_config_filename == '\0') {
        logError("file: "__FILE__", line: %d, "
                "item \"cluster_config_filename\" not exist or empty",
                __LINE__);
        return ENOENT;
    }

    resolve_path(filename, cluster_config_filename,
            full_cluster_filename, sizeof(full_cluster_filename));
    if ((result=fc_server_load_from_file_ex(&CLUSTER_CONFIG_CTX,
                    full_cluster_filename, FDIR_SERVER_DEFAULT_CLUSTER_PORT,
                    min_hosts_each_group, share_between_groups)) != 0)
    {
        return result;
    }

    if ((result=init_cluster_server_array()) != 0) {
        return result;
    }
    if ((result=find_group_indexes_in_cluster_config(filename)) != 0) {
        return result;
    }
    if ((result=calc_cluster_config_sign()) != 0) {
        return result;
    }

    if ((result=find_myself_in_cluster_config(filename)) != 0) {
        return result;
    }

    if ((result=ct_init_slave_array(&CLUSTER_ACTIVE_SLAVES)) != 0) {
        return result;
    }
    if ((result=ct_init_slave_array(&CLUSTER_INACTIVE_SLAVES)) != 0) {
        return result;
    }

    return 0;
}

static int load_data_path_config(IniContext *ini_context, const char *filename)
{
    char *data_path;

    data_path = iniGetStrValue(NULL, "data_path", ini_context);
    if (data_path == NULL) {
        data_path = "data";
    } else if (*data_path == '\0') {
        logError("file: "__FILE__", line: %d, "
                "config file: %s, empty data_path! "
                "please set data_path correctly.",
                __LINE__, filename);
        return EINVAL;
    }

    if (*data_path == '/') {
        DATA_PATH_LEN = strlen(data_path);
        DATA_PATH_STR = strdup(data_path);
        if (DATA_PATH_STR == NULL) {
            logError("file: "__FILE__", line: %d, "
                    "malloc %d bytes fail", __LINE__, DATA_PATH_LEN + 1);
            return ENOMEM;
        }
    } else {
        DATA_PATH_LEN = strlen(SF_G_BASE_PATH) + strlen(data_path) + 1;
        DATA_PATH_STR = (char *)malloc(DATA_PATH_LEN + 1);
        if (DATA_PATH_STR == NULL) {
            logError("file: "__FILE__", line: %d, "
                    "malloc %d bytes fail", __LINE__, DATA_PATH_LEN + 1);
            return ENOMEM;
        }
        DATA_PATH_LEN = sprintf(DATA_PATH_STR, "%s/%s",
                SF_G_BASE_PATH, data_path);
    }
    chopPath(DATA_PATH_STR);

    if (access(DATA_PATH_STR, F_OK) != 0) {
        if (errno != ENOENT) {
            logError("file: "__FILE__", line: %d, "
                    "access %s fail, errno: %d, error info: %s",
                    __LINE__, DATA_PATH_STR, errno, STRERROR(errno));
            return errno != 0 ? errno : EPERM;
        }

        if (mkdir(DATA_PATH_STR, 0775) != 0) {
            logError("file: "__FILE__", line: %d, "
                    "mkdir %s fail, errno: %d, error info: %s",
                    __LINE__, DATA_PATH_STR, errno, STRERROR(errno));
            return errno != 0 ? errno : EPERM;
        }
        
        SF_CHOWN_RETURN_ON_ERROR(DATA_PATH_STR, geteuid(), getegid());
    }

    return 0;
}

static int load_dentry_max_data_size(IniContext *ini_context,
        const char *filename)
{
    char *dentry_max_data_size;
    int64_t bytes;
    int result;

    dentry_max_data_size = iniGetStrValue(NULL,
            "dentry_max_data_size", ini_context);
    if (dentry_max_data_size == NULL) {
        bytes = 256;
    } else if ((result=parse_bytes(dentry_max_data_size, 1, &bytes)) != 0) {
        return result;
    }

    DENTRY_MAX_DATA_SIZE = bytes;
    if (DENTRY_MAX_DATA_SIZE < 0) {
        logError("file: "__FILE__", line: %d, "
                "config file: %s , dentry_max_data_size: %d < 0",
                __LINE__, filename, DENTRY_MAX_DATA_SIZE);
        return EINVAL;
    }

    if (DENTRY_MAX_DATA_SIZE > 4096) {
        logError("file: "__FILE__", line: %d, "
                "config file: %s , dentry_max_data_size: %d > 4KB",
                __LINE__, filename, DENTRY_MAX_DATA_SIZE);
        return EOVERFLOW;
    }

    return 0;
}

FDIRClusterServerInfo *fdir_get_server_by_id(const int server_id)
{
    FCServerInfo *server;
    server = fc_server_get_by_id(&CLUSTER_CONFIG_CTX, server_id);
    if (server == NULL) {
        return NULL;
    }

    return CLUSTER_SERVER_ARRAY.servers + (server -
            FC_SID_SERVERS(CLUSTER_CONFIG_CTX));
}

int server_load_config(const char *filename)
{
    IniContext ini_context;
    char server_config_str[1024];
    SFCustomConfig cluster_cfg;
    SFCustomConfig service_cfg;
    int result;

    memset(&ini_context, 0, sizeof(IniContext));
    if ((result=iniLoadFromFile(filename, &ini_context)) != 0) {
        logError("file: "__FILE__", line: %d, "
                "load conf file \"%s\" fail, ret code: %d",
                __LINE__, filename, result);
        return result;
    }

    if ((result=load_data_path_config(&ini_context, filename)) != 0) {
        return result;
    }

    if ((result=load_dentry_max_data_size(&ini_context, filename)) != 0) {
        return result;
    }

    SF_SET_CUSTOM_CONFIG(cluster_cfg, "cluster",
            FDIR_SERVER_DEFAULT_CLUSTER_PORT);
    SF_SET_CUSTOM_CONFIG(service_cfg, "service",
            FDIR_SERVER_DEFAULT_SERVICE_PORT);
    if ((result=sf_load_config_ex("fdir_serverd", filename,
                    &ini_context, &cluster_cfg, &service_cfg)) != 0)
    {
        return result;
    }

    if ((result=server_load_admin_config(&ini_context)) != 0) {
        return result;
    }

    BINLOG_BUFFER_SIZE = iniGetIntValue(NULL, "binlog_buffer_size",
            &ini_context, FDIR_DEFAULT_BINLOG_BUFFER_SIZE);
    if (BINLOG_BUFFER_SIZE <= 0) {
        BINLOG_BUFFER_SIZE = FDIR_DEFAULT_BINLOG_BUFFER_SIZE;
    }
    if (BINLOG_BUFFER_SIZE > g_sf_global_vars.max_buff_size) {
        logWarning("file: "__FILE__", line: %d, "
                "config file: %s , binlog_buffer_size: %d > "
                "max_buff_size: %d, set it to max_buff_size",
                __LINE__, filename, BINLOG_BUFFER_SIZE,
                g_sf_global_vars.max_buff_size);
        BINLOG_BUFFER_SIZE = g_sf_global_vars.max_buff_size;
    }

    g_server_global_vars.reload_interval_ms = iniGetIntValue(NULL,
            "reload_interval_ms", &ini_context,
            FDIR_SERVER_DEFAULT_RELOAD_INTERVAL);
    if (g_server_global_vars.reload_interval_ms <= 0) {
        g_server_global_vars.reload_interval_ms =
            FDIR_SERVER_DEFAULT_RELOAD_INTERVAL;
    }

    g_server_global_vars.check_alive_interval = iniGetIntValue(NULL,
            "check_alive_interval", &ini_context,
            FDIR_SERVER_DEFAULT_CHECK_ALIVE_INTERVAL);
    if (g_server_global_vars.check_alive_interval <= 0) {
        g_server_global_vars.check_alive_interval =
            FDIR_SERVER_DEFAULT_CHECK_ALIVE_INTERVAL;
    }

    g_server_global_vars.namespace_hashtable_capacity = iniGetIntValue(NULL,
            "namespace_hashtable_capacity", &ini_context,
            FDIR_NAMESPACE_HASHTABLE_CAPACITY);
    if (g_server_global_vars.namespace_hashtable_capacity <= 0) {
        g_server_global_vars.namespace_hashtable_capacity =
            FDIR_NAMESPACE_HASHTABLE_CAPACITY;
    }

    if ((result=load_cluster_config(&ini_context, filename)) != 0) {
        return result;
    }

    iniFreeContext(&ini_context);

    load_local_host_ip_addrs();
    snprintf(server_config_str, sizeof(server_config_str),
            "cluster_id = %d, my server id = %d, data_path = %s, "
            "dentry_max_data_size = %d, binlog_buffer_size = %d KB, "
            "admin config {username: %s, secret_key: %s}, "
            "reload_interval_ms = %d ms, "
            "check_alive_interval = %d s, "
            "namespace_hashtable_capacity = %d, "
            "cluster server count = %d",
            CLUSTER_ID, CLUSTER_MY_SERVER_ID,
            DATA_PATH_STR, DENTRY_MAX_DATA_SIZE,
            BINLOG_BUFFER_SIZE / 1024,
            g_server_global_vars.admin.username.str,
            g_server_global_vars.admin.secret_key.str,
            g_server_global_vars.reload_interval_ms,
            g_server_global_vars.check_alive_interval,
            g_server_global_vars.namespace_hashtable_capacity,
            FC_SID_SERVER_COUNT(CLUSTER_CONFIG_CTX));
    sf_log_config_ex(server_config_str);
    log_local_host_ip_addrs();
    log_cluster_server_config();

    return 0;
}
