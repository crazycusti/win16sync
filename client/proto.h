#ifndef WIN16SYNC_PROTO_H
#define WIN16SYNC_PROTO_H

#include <windows.h>
#include <winsock.h>

#define PROTO_HOST_CAPACITY 64
#define PROTO_PATH_CAPACITY 144
#define PROTO_LOCAL_PATH_CAPACITY 144
#define PROTO_ERROR_CAPACITY 128
#define PROTO_SUMMARY_CAPACITY 160
#define PROTO_LOG_PATH_CAPACITY 144
#define PROTO_MAX_FILES 128
#define PROTO_LINE_CAPACITY 512
#define PROTO_MAX_FILE_SIZE 2147483648UL
#define PROTO_PROGRESS_CAPACITY 160

#ifndef INADDR_NONE
#define INADDR_NONE 0xFFFFFFFFUL
#endif

typedef struct ProtoFileItemTag {
    char path[PROTO_PATH_CAPACITY];
    char local_path[PROTO_LOCAL_PATH_CAPACITY];
    unsigned long size;
    unsigned long crc32;
    unsigned long dos_stamp;
} ProtoFileItem;

typedef struct ProtoCacheItemTag {
    char path[PROTO_PATH_CAPACITY];
    unsigned long size;
    unsigned long crc32;
    unsigned long dos_stamp;
    unsigned long checked_tick;
} ProtoCacheItem;

typedef void (*ProtoProgressCallback)(void *context, const char *status);

typedef struct ProtoClientTag {
    char host[PROTO_HOST_CAPACITY];
    unsigned short port;
    char log_path[PROTO_LOG_PATH_CAPACITY];
    ProtoCacheItem *cache_items;
    int cache_count;
    int cache_capacity;
    int crc_disabled;
    int cache_recheck_enabled;
    ProtoProgressCallback progress_callback;
    void *progress_context;
    SOCKET socket_handle;
    char recv_buffer[1025];
    unsigned int recv_len;
    char last_error[PROTO_ERROR_CAPACITY];
    int wsa_ready;
    WSADATA wsa_data;
} ProtoClient;

void proto_client_init(ProtoClient *client);
void proto_client_disconnect(ProtoClient *client);
void proto_client_set_log_path(ProtoClient *client, const char *path);
void proto_client_set_progress_callback(ProtoClient *client, ProtoProgressCallback callback, void *context);
int proto_sync_directory(
    ProtoClient *client,
    const char *root_dir,
    char *summary,
    unsigned int summary_capacity,
    int *out_actions,
    int *out_conflicts
);
const char *proto_client_last_error(const ProtoClient *client);

#endif
