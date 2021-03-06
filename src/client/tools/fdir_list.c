#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "fastcommon/logger.h"
#include "fastdir/fdir_client.h"

static void usage(char *argv[])
{
    fprintf(stderr, "Usage: %s [-c config_filename] "
            "<-n namespace> <path>\n", argv[0]);
}

static void output_dentry_array(FDIRClientDentryArray *array)
{
    FDIRClientDentry *dentry;
    FDIRClientDentry *end;

    printf("count: %d\n", array->count);
    end = array->entries + array->count;
    for (dentry=array->entries; dentry<end; dentry++) {
        printf("%.*s\n", dentry->name.len, dentry->name.str);
    }
}

int main(int argc, char *argv[])
{
	int ch;
    const char *config_filename = "/etc/fdir/client.conf";
    char *ns;
    char *path;
    FDIRDEntryFullName entry_info;
    FDIRClientDentryArray array;
	int result;

    if (argc < 2) {
        usage(argv);
        return 1;
    }

    ns = NULL;
    while ((ch=getopt(argc, argv, "hc:n:")) != -1) {
        switch (ch) {
            case 'h':
                usage(argv);
                break;
            case 'n':
                ns = optarg;
                break;
            case 'c':
                config_filename = optarg;
                break;
            default:
                usage(argv);
                return 1;
        }
    }

    if (ns == NULL || optind >= argc) {
        usage(argv);
        return 1;
    }

    log_init();
    //g_log_context.log_level = LOG_DEBUG;

    path = argv[optind];
    if ((result=fdir_client_init(config_filename)) != 0) {
        return result;
    }

    FC_SET_STRING(entry_info.ns, ns);
    FC_SET_STRING(entry_info.path, path);

    if ((result=fdir_client_dentry_array_init(&array)) != 0) {
        return result;
    }

    if ((result=fdir_client_list_dentry(&g_client_global_vars.server_cluster,
                    &entry_info, &array)) != 0)
    {
        return result;
    }
    output_dentry_array(&array);
    fdir_client_dentry_array_free(&array);
    return 0;
}
