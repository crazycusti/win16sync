#include "proto.h"

#include <dos.h>
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PROTO_ACTION_CAPACITY 32
#define PROTO_TEMP_NAME "SYNC.$$$"
#define PROTO_CACHE_RECHECK_MS 300000UL

typedef struct ProtoWinsockApiTag {
    HINSTANCE module;
    int (PASCAL FAR *wsa_startup)(WORD, LPWSADATA);
    int (PASCAL FAR *wsa_cleanup)(void);
    SOCKET (PASCAL FAR *socket_create)(int, int, int);
    int (PASCAL FAR *socket_close)(SOCKET);
    int (PASCAL FAR *socket_connect)(SOCKET, const struct sockaddr FAR *, int);
    int (PASCAL FAR *socket_recv)(SOCKET, char FAR *, int, int);
    int (PASCAL FAR *socket_send)(SOCKET, const char FAR *, int, int);
    unsigned long (PASCAL FAR *addr_parse)(const char FAR *);
    unsigned short (PASCAL FAR *port_host_to_net)(unsigned short);
} ProtoWinsockApi;

static ProtoWinsockApi g_winsock;

static void proto_copy_text(char *target, const char *source, unsigned int capacity);
static int proto_append_text(char *target, const char *source, unsigned int capacity);
static int proto_local_join(char *target, unsigned int capacity, const char *left, const char *right);
static int proto_protocol_to_local(char *target, unsigned int capacity, const char *root_dir, const char *path);
static int proto_build_temp_path(char *target, unsigned int capacity, const char *local_path);
static int proto_ensure_parent_dirs(const char *path);
static int proto_is_valid_char(int c);
static int proto_is_valid_component(const char *name, int is_file);
static void proto_report_progress(ProtoClient *client, const char *status);
static const char *proto_path_name(const char *path);
static void proto_report_transfer(
    ProtoClient *client,
    const char *action,
    const char *path,
    unsigned long bytes_done,
    unsigned long bytes_total,
    unsigned long started_tick,
    unsigned long *last_tick
);
static void proto_log(ProtoClient *client, const char *format, ...);
static void proto_set_error(ProtoClient *client, const char *message);
static int proto_winsock_load(ProtoClient *client);
static void proto_winsock_unload(void);
static int proto_connect(ProtoClient *client);
static int proto_send_all(ProtoClient *client, const char *buffer, int length);
static int proto_send_line(ProtoClient *client, const char *line);
static int proto_read_line(ProtoClient *client, char *line, unsigned int capacity);
static int proto_read_exact(ProtoClient *client, char *buffer, unsigned long length);
static void proto_line_kind(const char *line, char *kind, unsigned int capacity);
static int proto_extract_param(const char *line, const char *key, char *value, unsigned int capacity);
static unsigned long proto_extract_ulong_hex(const char *line, const char *key, unsigned long default_value);
static unsigned long proto_extract_ulong_dec(const char *line, const char *key, unsigned long default_value);
static int proto_encode_hex(const char *source, char *target, unsigned int capacity);
static int proto_decode_hex(const char *source, char *target, unsigned int capacity);
static unsigned long proto_crc32_step(unsigned long crc, const unsigned char *data, unsigned int length);
static int proto_file_crc32(const char *path, unsigned long *out_crc32);
static int proto_lookup_cached_crc(
    ProtoClient *client,
    const char *path,
    unsigned long size,
    unsigned long dos_stamp,
    unsigned long *out_crc32
);
static void proto_remember_cached_crc(
    ProtoClient *client,
    const char *path,
    unsigned long size,
    unsigned long dos_stamp,
    unsigned long crc32
);
static void proto_forget_cached_crc(ProtoClient *client, const char *path);
static int proto_add_file_item(
    ProtoClient *client,
    ProtoFileItem *items,
    int *count,
    const char *path,
    const char *local_path,
    unsigned long size,
    unsigned long dos_stamp
);
static int proto_compare_items(const void *left, const void *right);
static int proto_scan_recursive(
    ProtoClient *client,
    const char *root_dir,
    const char *current_dir,
    const char *prefix,
    ProtoFileItem *items,
    int *count
);
static int proto_scan_directory(ProtoClient *client, const char *root_dir, ProtoFileItem *items, int *count);
static int proto_find_item(ProtoFileItem *items, int count, const char *path);
static int proto_write_file_timestamp(const char *path, unsigned long dos_stamp);
static int proto_delete_local_file(const char *path);
static int proto_copy_file(const char *source_path, const char *target_path);
static int proto_receive_download(
    ProtoClient *client,
    const char *root_dir,
    const char *path,
    unsigned long expected_size,
    unsigned long expected_crc,
    unsigned long dos_stamp
);
static int proto_send_upload(ProtoClient *client, ProtoFileItem *item);
static void proto_summary_text(char *summary, unsigned int capacity, int actions, int conflicts, int files);

static void proto_copy_text(char *target, const char *source, unsigned int capacity)
{
    unsigned int index;

    if (target == NULL || capacity == 0) {
        return;
    }
    if (source == NULL) {
        target[0] = '\0';
        return;
    }

    for (index = 0; index + 1 < capacity && source[index] != '\0'; ++index) {
        target[index] = source[index];
    }
    target[index] = '\0';
}

static int proto_append_text(char *target, const char *source, unsigned int capacity)
{
    unsigned int target_length;
    unsigned int index;

    if (target == NULL || source == NULL || capacity == 0) {
        return 0;
    }

    target_length = (unsigned int)strlen(target);
    for (index = 0; source[index] != '\0'; ++index) {
        if (target_length + index + 1 >= capacity) {
            return 0;
        }
        target[target_length + index] = source[index];
    }
    target[target_length + index] = '\0';
    return 1;
}

static int proto_local_join(char *target, unsigned int capacity, const char *left, const char *right)
{
    if (target == NULL || capacity == 0 || left == NULL || right == NULL) {
        return 0;
    }

    target[0] = '\0';
    if (!proto_append_text(target, left, capacity)) {
        return 0;
    }
    if (target[0] != '\0' && target[strlen(target) - 1] != '\\' && target[strlen(target) - 1] != '/') {
        if (!proto_append_text(target, "\\", capacity)) {
            return 0;
        }
    }
    return proto_append_text(target, right, capacity);
}

static int proto_protocol_to_local(char *target, unsigned int capacity, const char *root_dir, const char *path)
{
    unsigned int index;
    unsigned int target_length;

    if (!proto_local_join(target, capacity, root_dir, "")) {
        return 0;
    }
    if (target[0] != '\0' && target[strlen(target) - 1] == '\\') {
        target[strlen(target) - 1] = '\0';
    }
    if (path == NULL || path[0] == '\0') {
        return 1;
    }

    target_length = (unsigned int)strlen(target);
    if (target_length > 0U && target[target_length - 1U] != '\\' && target[target_length - 1U] != '/') {
        if (target_length + 1U >= capacity) {
            return 0;
        }
        target[target_length++] = '\\';
        target[target_length] = '\0';
    }

    for (index = 0; path[index] != '\0'; ++index) {
        char c;

        c = path[index];
        if (target_length + 1U >= capacity) {
            return 0;
        }
        target[target_length++] = c == '/' ? '\\' : c;
        target[target_length] = '\0';
    }
    return 1;
}

static int proto_build_temp_path(char *target, unsigned int capacity, const char *local_path)
{
    int index;

    if (target == NULL || capacity == 0 || local_path == NULL || local_path[0] == '\0') {
        return 0;
    }

    proto_copy_text(target, local_path, capacity);
    for (index = (int)strlen(target) - 1; index >= 0; --index) {
        if (target[index] == '\\' || target[index] == '/') {
            target[index + 1] = '\0';
            return proto_append_text(target, PROTO_TEMP_NAME, capacity);
        }
    }

    proto_copy_text(target, PROTO_TEMP_NAME, capacity);
    return 1;
}

static int proto_ensure_parent_dirs(const char *path)
{
    char buffer[PROTO_LOCAL_PATH_CAPACITY];
    unsigned int index;

    proto_copy_text(buffer, path, sizeof(buffer));
    for (index = 0; buffer[index] != '\0'; ++index) {
        if ((buffer[index] == '\\' || buffer[index] == '/') && index > 2) {
            char saved;

            saved = buffer[index];
            buffer[index] = '\0';
            _mkdir(buffer);
            buffer[index] = saved;
        }
    }
    return 1;
}

static int proto_is_valid_char(int c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return 1;
    }

    switch (c) {
    case '$':
    case '%':
    case '\'':
    case '-':
    case '_':
    case '@':
    case '~':
    case '`':
    case '!':
    case '(':
    case ')':
    case '{':
    case '}':
    case '^':
    case '#':
    case '&':
        return 1;
    }

    return 0;
}

static int proto_is_valid_component(const char *name, int is_file)
{
    unsigned int index;
    unsigned int dot_index;
    unsigned int base_length;
    unsigned int ext_length;
    char upper[16];

    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }

    proto_copy_text(upper, name, sizeof(upper));
    for (index = 0; upper[index] != '\0'; ++index) {
        if ((unsigned char)upper[index] >= 128) {
            return 0;
        }
        if (upper[index] >= 'a' && upper[index] <= 'z') {
            upper[index] = (char)(upper[index] - 'a' + 'A');
        }
    }

    dot_index = (unsigned int)strlen(upper);
    for (index = 0; upper[index] != '\0'; ++index) {
        if (upper[index] == '.') {
            dot_index = index;
            break;
        }
    }
    if (!is_file) {
        if (dot_index != (unsigned int)strlen(upper)) {
            return 0;
        }
    } else {
        for (index = dot_index + 1U; upper[index] != '\0'; ++index) {
            if (upper[index] == '.') {
                return 0;
            }
        }
    }

    base_length = dot_index;
    if (base_length == 0U || base_length > 8U) {
        return 0;
    }
    for (index = 0; index < base_length; ++index) {
        if (!proto_is_valid_char((unsigned char)upper[index])) {
            return 0;
        }
    }

    ext_length = 0U;
    if (upper[dot_index] == '.') {
        ext_length = (unsigned int)strlen(upper + dot_index + 1U);
        if (ext_length == 0U || ext_length > 3U) {
            return 0;
        }
        for (index = dot_index + 1U; upper[index] != '\0'; ++index) {
            if (!proto_is_valid_char((unsigned char)upper[index])) {
                return 0;
            }
        }
    }

    return 1;
}

static void proto_report_progress(ProtoClient *client, const char *status)
{
    if (client == NULL || client->progress_callback == NULL) {
        return;
    }
    client->progress_callback(client->progress_context, status);
}

static const char *proto_path_name(const char *path)
{
    const char *name;
    unsigned int index;

    if (path == NULL || path[0] == '\0') {
        return "";
    }

    name = path;
    for (index = 0; path[index] != '\0'; ++index) {
        if (path[index] == '/' || path[index] == '\\') {
            name = path + index + 1U;
        }
    }
    return name;
}

static void proto_report_transfer(
    ProtoClient *client,
    const char *action,
    const char *path,
    unsigned long bytes_done,
    unsigned long bytes_total,
    unsigned long started_tick,
    unsigned long *last_tick
)
{
    unsigned long now;
    unsigned long elapsed;
    unsigned long bytes_per_second;
    char status[PROTO_PROGRESS_CAPACITY];

    now = GetTickCount();
    if (last_tick != NULL && *last_tick != 0UL && now - *last_tick < 250UL && bytes_done < bytes_total) {
        return;
    }
    if (last_tick != NULL) {
        *last_tick = now;
    }

    elapsed = now >= started_tick ? now - started_tick : 0UL;
    if (elapsed == 0UL) {
        bytes_per_second = 0UL;
    } else {
        bytes_per_second = (bytes_done * 1000UL) / elapsed;
    }

    wsprintf(
        status,
        "%s %s %lu KB/s (%lu/%lu KB)",
        action,
        proto_path_name(path),
        (bytes_per_second + 1023UL) / 1024UL,
        (bytes_done + 1023UL) / 1024UL,
        (bytes_total + 1023UL) / 1024UL
    );
    proto_report_progress(client, status);
}

static void proto_log(ProtoClient *client, const char *format, ...)
{
    FILE *file;
    va_list args;
    char line[256];

    if (client == NULL || client->log_path[0] == '\0') {
        return;
    }

    file = fopen(client->log_path, "a");
    if (file == NULL) {
        return;
    }

    va_start(args, format);
    vsprintf(line, format, args);
    va_end(args);
    fprintf(file, "%s\r\n", line);
    fclose(file);
}

static void proto_set_error(ProtoClient *client, const char *message)
{
    if (client == NULL) {
        return;
    }

    proto_copy_text(client->last_error, message, sizeof(client->last_error));
    proto_log(client, "ERROR: %s", message);
}

static void proto_winsock_unload(void)
{
    if (g_winsock.module != NULL) {
        FreeLibrary(g_winsock.module);
    }
    memset(&g_winsock, 0, sizeof(g_winsock));
}

static int proto_winsock_load(ProtoClient *client)
{
    if (g_winsock.module != NULL) {
        return 1;
    }

    g_winsock.module = LoadLibrary("WINSOCK.DLL");
    if ((UINT)g_winsock.module <= HINSTANCE_ERROR) {
        proto_winsock_unload();
        proto_set_error(client, "WINSOCK.DLL fehlt");
        return 0;
    }

    g_winsock.wsa_startup = (int (PASCAL FAR *)(WORD, LPWSADATA))GetProcAddress(g_winsock.module, "WSAStartup");
    g_winsock.wsa_cleanup = (int (PASCAL FAR *)(void))GetProcAddress(g_winsock.module, "WSACleanup");
    g_winsock.socket_create = (SOCKET (PASCAL FAR *)(int, int, int))GetProcAddress(g_winsock.module, "socket");
    g_winsock.socket_close = (int (PASCAL FAR *)(SOCKET))GetProcAddress(g_winsock.module, "closesocket");
    g_winsock.socket_connect = (int (PASCAL FAR *)(SOCKET, const struct sockaddr FAR *, int))GetProcAddress(g_winsock.module, "connect");
    g_winsock.socket_recv = (int (PASCAL FAR *)(SOCKET, char FAR *, int, int))GetProcAddress(g_winsock.module, "recv");
    g_winsock.socket_send = (int (PASCAL FAR *)(SOCKET, const char FAR *, int, int))GetProcAddress(g_winsock.module, "send");
    g_winsock.addr_parse = (unsigned long (PASCAL FAR *)(const char FAR *))GetProcAddress(g_winsock.module, "inet_addr");
    g_winsock.port_host_to_net = (unsigned short (PASCAL FAR *)(unsigned short))GetProcAddress(g_winsock.module, "htons");

    if (g_winsock.wsa_startup == NULL ||
        g_winsock.wsa_cleanup == NULL ||
        g_winsock.socket_create == NULL ||
        g_winsock.socket_close == NULL ||
        g_winsock.socket_connect == NULL ||
        g_winsock.socket_recv == NULL ||
        g_winsock.socket_send == NULL ||
        g_winsock.addr_parse == NULL ||
        g_winsock.port_host_to_net == NULL) {
        proto_winsock_unload();
        proto_set_error(client, "WINSOCK.DLL unvollstaendig");
        return 0;
    }

    return 1;
}

void proto_client_init(ProtoClient *client)
{
    memset(client, 0, sizeof(*client));
    proto_copy_text(client->host, "127.0.0.1", sizeof(client->host));
    client->port = 9071;
    client->socket_handle = INVALID_SOCKET;
}

void proto_client_disconnect(ProtoClient *client)
{
    if (client->socket_handle != INVALID_SOCKET) {
        if (g_winsock.socket_close != NULL) {
            g_winsock.socket_close(client->socket_handle);
        }
        client->socket_handle = INVALID_SOCKET;
    }
    if (client->wsa_ready) {
        if (g_winsock.wsa_cleanup != NULL) {
            g_winsock.wsa_cleanup();
        }
        client->wsa_ready = 0;
    }
    proto_winsock_unload();
    client->recv_len = 0;
    client->recv_buffer[0] = '\0';
}

void proto_client_set_log_path(ProtoClient *client, const char *path)
{
    if (client == NULL) {
        return;
    }
    proto_copy_text(client->log_path, path, sizeof(client->log_path));
}

void proto_client_set_progress_callback(ProtoClient *client, ProtoProgressCallback callback, void *context)
{
    if (client == NULL) {
        return;
    }

    client->progress_callback = callback;
    client->progress_context = context;
}

static int proto_connect(ProtoClient *client)
{
    struct sockaddr_in address;
    unsigned long ipv4;

    proto_client_disconnect(client);
    client->last_error[0] = '\0';
    proto_report_progress(client, "Verbinde...");

    if (!proto_winsock_load(client)) {
        return 0;
    }
    if (!client->wsa_ready) {
        if (g_winsock.wsa_startup(0x0101, &client->wsa_data) != 0) {
            proto_set_error(client, "WSAStartup fehlgeschlagen");
            return 0;
        }
        client->wsa_ready = 1;
    }

    client->socket_handle = g_winsock.socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client->socket_handle == INVALID_SOCKET) {
        proto_set_error(client, "socket fehlgeschlagen");
        proto_client_disconnect(client);
        return 0;
    }

    ipv4 = g_winsock.addr_parse(client->host);
    if (ipv4 == INADDR_NONE) {
        proto_set_error(client, "Nur numerische IPv4-Adressen sind erlaubt");
        proto_client_disconnect(client);
        return 0;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = g_winsock.port_host_to_net(client->port);
    address.sin_addr.s_addr = ipv4;

    if (g_winsock.socket_connect(client->socket_handle, (struct sockaddr FAR *)&address, sizeof(address)) != 0) {
        proto_set_error(client, "connect fehlgeschlagen");
        proto_client_disconnect(client);
        return 0;
    }

    return 1;
}

static int proto_send_all(ProtoClient *client, const char *buffer, int length)
{
    int sent_total;
    int sent_now;

    sent_total = 0;
    while (sent_total < length) {
        sent_now = g_winsock.socket_send(client->socket_handle, (const char FAR *)(buffer + sent_total), length - sent_total, 0);
        if (sent_now == SOCKET_ERROR || sent_now <= 0) {
            proto_set_error(client, "send fehlgeschlagen");
            return 0;
        }
        sent_total += sent_now;
    }

    return 1;
}

static int proto_send_line(ProtoClient *client, const char *line)
{
    if (!proto_send_all(client, line, (int)strlen(line))) {
        return 0;
    }
    return proto_send_all(client, "\r\n", 2);
}

static int proto_read_line(ProtoClient *client, char *line, unsigned int capacity)
{
    int received;
    unsigned int index;
    unsigned int line_length;
    unsigned int remaining;

    if (line == NULL || capacity == 0) {
        proto_set_error(client, "Linebuffer ungültig");
        return 0;
    }

    line[0] = '\0';
    while (1) {
        for (index = 0; index < client->recv_len; ++index) {
            if (client->recv_buffer[index] == '\n') {
                line_length = index;
                if (line_length > 0 && client->recv_buffer[line_length - 1] == '\r') {
                    line_length -= 1;
                }
                if (line_length + 1 > capacity) {
                    proto_set_error(client, "Antwort zu lang");
                    return 0;
                }

                memcpy(line, client->recv_buffer, line_length);
                line[line_length] = '\0';

                remaining = client->recv_len - (index + 1U);
                if (remaining > 0U) {
                    memmove(client->recv_buffer, client->recv_buffer + index + 1U, remaining);
                }
                client->recv_len = remaining;
                client->recv_buffer[client->recv_len] = '\0';
                return 1;
            }
        }

        if (client->recv_len >= sizeof(client->recv_buffer) - 1U) {
            proto_set_error(client, "Empfangspuffer voll");
            return 0;
        }

        received = g_winsock.socket_recv(
            client->socket_handle,
            client->recv_buffer + client->recv_len,
            sizeof(client->recv_buffer) - 1U - client->recv_len,
            0
        );
        if (received == SOCKET_ERROR || received <= 0) {
            proto_set_error(client, "recv fehlgeschlagen");
            return 0;
        }

        client->recv_len += (unsigned int)received;
        client->recv_buffer[client->recv_len] = '\0';
    }
}

static int proto_read_exact(ProtoClient *client, char *buffer, unsigned long length)
{
    unsigned long copied;
    int received;

    copied = 0UL;
    while (copied < length) {
        if (client->recv_len > 0U) {
            unsigned int chunk;

            chunk = client->recv_len;
            if ((unsigned long)chunk > length - copied) {
                chunk = (unsigned int)(length - copied);
            }
            memcpy(buffer + copied, client->recv_buffer, chunk);
            copied += (unsigned long)chunk;
            if (client->recv_len > chunk) {
                memmove(client->recv_buffer, client->recv_buffer + chunk, client->recv_len - chunk);
            }
            client->recv_len -= chunk;
            client->recv_buffer[client->recv_len] = '\0';
            continue;
        }

        received = g_winsock.socket_recv(
            client->socket_handle,
            buffer + copied,
            (int)((length - copied) > 8192UL ? 8192UL : (length - copied)),
            0
        );
        if (received == SOCKET_ERROR || received <= 0) {
            proto_set_error(client, "Rohdaten-Empfang fehlgeschlagen");
            return 0;
        }
        copied += (unsigned long)received;
    }

    return 1;
}

static void proto_line_kind(const char *line, char *kind, unsigned int capacity)
{
    unsigned int index;

    if (kind == NULL || capacity == 0) {
        return;
    }

    for (index = 0; index + 1 < capacity && line[index] != '\0' && line[index] != ' '; ++index) {
        kind[index] = line[index];
    }
    kind[index] = '\0';
}

static int proto_extract_param(const char *line, const char *key, char *value, unsigned int capacity)
{
    unsigned int key_length;
    const char *cursor;

    if (value == NULL || capacity == 0 || key == NULL) {
        return 0;
    }
    value[0] = '\0';
    key_length = (unsigned int)strlen(key);
    cursor = line;

    while (*cursor != '\0') {
        while (*cursor == ' ') {
            cursor += 1;
        }
        if (*cursor == '\0') {
            break;
        }
        if (strncmp(cursor, key, key_length) == 0 && cursor[key_length] == '=') {
            unsigned int index;

            cursor += key_length + 1U;
            for (index = 0; index + 1 < capacity && cursor[index] != '\0' && cursor[index] != ' '; ++index) {
                value[index] = cursor[index];
            }
            value[index] = '\0';
            return value[0] != '\0';
        }
        while (*cursor != '\0' && *cursor != ' ') {
            cursor += 1;
        }
    }

    return 0;
}

static unsigned long proto_extract_ulong_hex(const char *line, const char *key, unsigned long default_value)
{
    char value[32];

    if (!proto_extract_param(line, key, value, sizeof(value))) {
        return default_value;
    }
    return strtoul(value, NULL, 16);
}

static unsigned long proto_extract_ulong_dec(const char *line, const char *key, unsigned long default_value)
{
    char value[32];

    if (!proto_extract_param(line, key, value, sizeof(value))) {
        return default_value;
    }
    return strtoul(value, NULL, 10);
}

static int proto_encode_hex(const char *source, char *target, unsigned int capacity)
{
    static const char hex[] = "0123456789ABCDEF";
    unsigned int source_index;
    unsigned int target_index;
    unsigned char current;

    if (source == NULL || target == NULL || capacity == 0) {
        return 0;
    }

    target_index = 0;
    for (source_index = 0; source[source_index] != '\0'; ++source_index) {
        if (target_index + 3 >= capacity) {
            return 0;
        }
        current = (unsigned char)source[source_index];
        target[target_index++] = hex[(current >> 4) & 0x0F];
        target[target_index++] = hex[current & 0x0F];
    }
    target[target_index] = '\0';
    return 1;
}

static int proto_hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static int proto_decode_hex(const char *source, char *target, unsigned int capacity)
{
    unsigned int index;
    unsigned int out_index;
    int high;
    int low;

    if (source == NULL || target == NULL || capacity == 0) {
        return 0;
    }
    if ((strlen(source) % 2U) != 0U) {
        return 0;
    }

    out_index = 0U;
    for (index = 0U; source[index] != '\0'; index += 2U) {
        if (out_index + 1U >= capacity) {
            return 0;
        }
        high = proto_hex_value(source[index]);
        low = proto_hex_value(source[index + 1U]);
        if (high < 0 || low < 0) {
            return 0;
        }
        target[out_index++] = (char)((high << 4) | low);
    }
    target[out_index] = '\0';
    return 1;
}

static unsigned long proto_crc32_step(unsigned long crc, const unsigned char *data, unsigned int length)
{
    unsigned int index;
    unsigned int bit;
    unsigned long value;

    for (index = 0U; index < length; ++index) {
        value = (crc ^ data[index]) & 0xFFUL;
        for (bit = 0U; bit < 8U; ++bit) {
            if ((value & 1UL) != 0UL) {
                value = 0xEDB88320UL ^ (value >> 1);
            } else {
                value >>= 1;
            }
        }
        crc = (crc >> 8) ^ value;
    }

    return crc;
}

static int proto_file_crc32(const char *path, unsigned long *out_crc32)
{
    FILE *file;
    unsigned char buffer[1024];
    size_t read_now;
    unsigned long crc;

    if (out_crc32 != NULL) {
        *out_crc32 = 0UL;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }

    crc = 0xFFFFFFFFUL;
    while ((read_now = fread(buffer, 1, sizeof(buffer), file)) > 0U) {
        crc = proto_crc32_step(crc, buffer, (unsigned int)read_now);
    }

    fclose(file);
    if (out_crc32 != NULL) {
        *out_crc32 = ~crc;
    }
    return 1;
}

static int proto_lookup_cached_crc(
    ProtoClient *client,
    const char *path,
    unsigned long size,
    unsigned long dos_stamp,
    unsigned long *out_crc32
)
{
    int index;
    unsigned long now;

    if (client == NULL || client->cache_items == NULL || path == NULL) {
        return 0;
    }

    now = GetTickCount();
    for (index = 0; index < client->cache_count; ++index) {
        ProtoCacheItem *item;

        item = &client->cache_items[index];
        if (strcmp(item->path, path) != 0) {
            continue;
        }
        if (item->size != size || item->dos_stamp != dos_stamp) {
            return 0;
        }
        if (client->cache_recheck_enabled && (now - item->checked_tick) >= PROTO_CACHE_RECHECK_MS) {
            return 0;
        }
        if (out_crc32 != NULL) {
            *out_crc32 = item->crc32;
        }
        return 1;
    }

    return 0;
}

static void proto_remember_cached_crc(
    ProtoClient *client,
    const char *path,
    unsigned long size,
    unsigned long dos_stamp,
    unsigned long crc32
)
{
    int index;
    ProtoCacheItem *item;

    if (client == NULL || path == NULL || path[0] == '\0') {
        return;
    }

    for (index = 0; index < client->cache_count; ++index) {
        if (strcmp(client->cache_items[index].path, path) == 0) {
            item = &client->cache_items[index];
            proto_copy_text(item->path, path, sizeof(item->path));
            item->size = size;
            item->dos_stamp = dos_stamp;
            item->crc32 = crc32;
            item->checked_tick = GetTickCount();
            return;
        }
    }

    if (client->cache_items == NULL) {
        client->cache_items = (ProtoCacheItem *)malloc(sizeof(ProtoCacheItem) * PROTO_MAX_FILES);
        if (client->cache_items == NULL) {
            return;
        }
        client->cache_capacity = PROTO_MAX_FILES;
        client->cache_count = 0;
    }

    if (client->cache_count >= client->cache_capacity) {
        memmove(
            client->cache_items,
            client->cache_items + 1,
            sizeof(ProtoCacheItem) * (client->cache_capacity - 1)
        );
        client->cache_count = client->cache_capacity - 1;
    }

    item = &client->cache_items[client->cache_count++];
    memset(item, 0, sizeof(*item));
    proto_copy_text(item->path, path, sizeof(item->path));
    item->size = size;
    item->dos_stamp = dos_stamp;
    item->crc32 = crc32;
    item->checked_tick = GetTickCount();
}

static void proto_forget_cached_crc(ProtoClient *client, const char *path)
{
    int index;

    if (client == NULL || client->cache_items == NULL || path == NULL || path[0] == '\0') {
        return;
    }

    for (index = 0; index < client->cache_count; ++index) {
        if (strcmp(client->cache_items[index].path, path) == 0) {
            if (index + 1 < client->cache_count) {
                memmove(
                    client->cache_items + index,
                    client->cache_items + index + 1,
                    sizeof(ProtoCacheItem) * (client->cache_count - index - 1)
                );
            }
            client->cache_count -= 1;
            return;
        }
    }
}

static int proto_add_file_item(
    ProtoClient *client,
    ProtoFileItem *items,
    int *count,
    const char *path,
    const char *local_path,
    unsigned long size,
    unsigned long dos_stamp
)
{
    unsigned long crc32;

    if (*count >= PROTO_MAX_FILES) {
        proto_set_error(client, "Zu viele Dateien im Sync-Ordner");
        return 0;
    }
    if (client->crc_disabled) {
        proto_forget_cached_crc(client, path);
        crc32 = 0UL;
    } else if (!proto_lookup_cached_crc(client, path, size, dos_stamp, &crc32)) {
        if (!proto_file_crc32(local_path, &crc32)) {
            proto_set_error(client, "CRC32 konnte nicht berechnet werden");
            return 0;
        }
        proto_remember_cached_crc(client, path, size, dos_stamp, crc32);
    }

    proto_copy_text(items[*count].path, path, sizeof(items[*count].path));
    proto_copy_text(items[*count].local_path, local_path, sizeof(items[*count].local_path));
    items[*count].size = size;
    items[*count].crc32 = crc32;
    items[*count].dos_stamp = dos_stamp;
    *count += 1;
    return 1;
}

static int proto_compare_items(const void *left, const void *right)
{
    return strcmp(((const ProtoFileItem *)left)->path, ((const ProtoFileItem *)right)->path);
}

static int proto_scan_recursive(
    ProtoClient *client,
    const char *root_dir,
    const char *current_dir,
    const char *prefix,
    ProtoFileItem *items,
    int *count
)
{
    struct find_t find_data;
    char pattern[PROTO_LOCAL_PATH_CAPACITY];
    unsigned result;
    int opened;

    (void)root_dir;

    if (!proto_local_join(pattern, sizeof(pattern), current_dir, "*.*")) {
        proto_set_error(client, "Pfadmuster zu lang");
        return 0;
    }

    result = _dos_findfirst(pattern, _A_NORMAL | _A_RDONLY | _A_ARCH | _A_SUBDIR, &find_data);
    opened = result == 0U;
    while (result == 0U) {
        char name_upper[16];
        char protocol_path[PROTO_PATH_CAPACITY];
        char local_path[PROTO_LOCAL_PATH_CAPACITY];
        unsigned int index;
        int is_dir;

        if (strcmp(find_data.name, ".") != 0 && strcmp(find_data.name, "..") != 0) {
            proto_copy_text(name_upper, find_data.name, sizeof(name_upper));
            for (index = 0; name_upper[index] != '\0'; ++index) {
                if (name_upper[index] >= 'a' && name_upper[index] <= 'z') {
                    name_upper[index] = (char)(name_upper[index] - 'a' + 'A');
                }
            }

            is_dir = (find_data.attrib & _A_SUBDIR) != 0;
            if (proto_is_valid_component(name_upper, !is_dir)) {
                if (prefix != NULL && prefix[0] != '\0') {
                    sprintf(protocol_path, "%s/%s", prefix, name_upper);
                } else {
                    proto_copy_text(protocol_path, name_upper, sizeof(protocol_path));
                }
                if (!proto_local_join(local_path, sizeof(local_path), current_dir, find_data.name)) {
                    proto_set_error(client, "Lokaler Pfad zu lang");
                    _dos_findclose(&find_data);
                    return 0;
                }

                if (is_dir) {
                    if (!proto_scan_recursive(client, root_dir, local_path, protocol_path, items, count)) {
                        _dos_findclose(&find_data);
                        return 0;
                    }
                } else if (find_data.size <= PROTO_MAX_FILE_SIZE) {
                    if (strcmp(name_upper, PROTO_TEMP_NAME) != 0 &&
                        strcmp(name_upper, "W16SYNC.LOG") != 0 &&
                        strcmp(name_upper, "W16SYNC.INI") != 0) {
                        if (!proto_add_file_item(
                                client,
                                items,
                                count,
                                protocol_path,
                                local_path,
                                find_data.size,
                                (((unsigned long)find_data.wr_date) << 16) | find_data.wr_time)) {
                            _dos_findclose(&find_data);
                            return 0;
                        }
                    }
                } else {
                    proto_log(client, "IGNORED oversized file: %s", local_path);
                }
            } else {
                proto_log(client, "IGNORED non-8.3 name: %s", find_data.name);
            }
        }

        result = _dos_findnext(&find_data);
    }

    if (opened) {
        _dos_findclose(&find_data);
    }

    return 1;
}

static int proto_scan_directory(ProtoClient *client, const char *root_dir, ProtoFileItem *items, int *count)
{
    *count = 0;
    if (!proto_scan_recursive(client, root_dir, root_dir, "", items, count)) {
        return 0;
    }
    qsort(items, *count, sizeof(ProtoFileItem), proto_compare_items);
    return 1;
}

static int proto_find_item(ProtoFileItem *items, int count, const char *path)
{
    int index;

    for (index = 0; index < count; ++index) {
        if (strcmp(items[index].path, path) == 0) {
            return index;
        }
    }
    return -1;
}

static int proto_write_file_timestamp(const char *path, unsigned long dos_stamp)
{
    int handle;
    unsigned date_part;
    unsigned time_part;

    handle = _open(path, _O_RDWR | _O_BINARY);
    if (handle < 0) {
        return 0;
    }

    date_part = (unsigned)(dos_stamp >> 16);
    time_part = (unsigned)(dos_stamp & 0xFFFFUL);
    _dos_setftime(handle, date_part, time_part);
    _close(handle);
    return 1;
}

static int proto_delete_local_file(const char *path)
{
    unsigned attr;

    if (_dos_getfileattr(path, &attr) != 0U) {
        return 1;
    }
    _dos_setfileattr(path, _A_NORMAL);
    if (remove(path) != 0) {
        return 0;
    }
    return 1;
}

static int proto_copy_file(const char *source_path, const char *target_path)
{
    FILE *source_file;
    FILE *target_file;
    char *buffer;
    size_t read_now;
    int success;

    source_file = fopen(source_path, "rb");
    if (source_file == NULL) {
        return 0;
    }
    target_file = fopen(target_path, "wb");
    if (target_file == NULL) {
        fclose(source_file);
        return 0;
    }

    buffer = (char *)malloc(4096U);
    if (buffer == NULL) {
        fclose(target_file);
        fclose(source_file);
        return 0;
    }

    success = 1;
    while ((read_now = fread(buffer, 1, 4096U, source_file)) > 0U) {
        if (fwrite(buffer, 1, read_now, target_file) != read_now) {
            success = 0;
            break;
        }
    }
    if (ferror(source_file)) {
        success = 0;
    }

    free(buffer);
    fclose(target_file);
    fclose(source_file);

    if (!success) {
        remove(target_path);
        return 0;
    }

    return 1;
}

static int proto_receive_download(
    ProtoClient *client,
    const char *root_dir,
    const char *path,
    unsigned long expected_size,
    unsigned long expected_crc,
    unsigned long dos_stamp
)
{
    char line[PROTO_LINE_CAPACITY];
    char local_path[PROTO_LOCAL_PATH_CAPACITY];
    char temp_path[PROTO_LOCAL_PATH_CAPACITY];
    char message[PROTO_ERROR_CAPACITY];
    char *buffer;
    FILE *file;
    unsigned long actual_crc;
    unsigned long bytes_done;
    unsigned long last_tick;
    unsigned long started_tick;
    char kind[16];

    if (!proto_protocol_to_local(local_path, sizeof(local_path), root_dir, path)) {
        proto_set_error(client, "Download-Pfad zu lang");
        return 0;
    }
    if (!proto_build_temp_path(temp_path, sizeof(temp_path), local_path)) {
        proto_set_error(client, "Temp-Pfad zu lang");
        return 0;
    }
    proto_ensure_parent_dirs(temp_path);
    proto_ensure_parent_dirs(local_path);
    proto_delete_local_file(temp_path);

    if (!proto_send_line(client, "READY")) {
        return 0;
    }
    if (!proto_read_line(client, line, sizeof(line))) {
        return 0;
    }
    proto_line_kind(line, kind, sizeof(kind));
    if (strcmp(kind, "FILE") != 0) {
        proto_set_error(client, "FILE erwartet");
        return 0;
    }

    file = fopen(temp_path, "wb");
    if (file == NULL) {
        wsprintf(message, "Temp-Datei konnte nicht erstellt werden (%d)", errno);
        proto_log(client, "TEMP OPEN FAILED path=%s errno=%d", temp_path, errno);
        proto_set_error(client, message);
        return 0;
    }

    buffer = (char *)malloc(4096U);
    if (buffer == NULL) {
        fclose(file);
        proto_set_error(client, "Nicht genug Speicher fuer Download");
        return 0;
    }

    {
        unsigned long remaining;

        remaining = expected_size;
        bytes_done = 0UL;
        started_tick = GetTickCount();
        last_tick = 0UL;
        proto_report_transfer(client, "Download", path, 0UL, expected_size, started_tick, &last_tick);
        while (remaining > 0UL) {
            unsigned long chunk;

            chunk = remaining > 4096UL ? 4096UL : remaining;
            if (!proto_read_exact(client, buffer, chunk)) {
                free(buffer);
                fclose(file);
                remove(temp_path);
                return 0;
            }
            if (fwrite(buffer, 1, (unsigned)chunk, file) != (unsigned)chunk) {
                free(buffer);
                fclose(file);
                remove(temp_path);
                proto_set_error(client, "Download-Datei konnte nicht geschrieben werden");
                return 0;
            }
            remaining -= chunk;
            bytes_done = expected_size - remaining;
            proto_report_transfer(client, "Download", path, bytes_done, expected_size, started_tick, &last_tick);
        }
    }

    free(buffer);
    fclose(file);

    if (!client->crc_disabled) {
        if (!proto_file_crc32(temp_path, &actual_crc) || actual_crc != expected_crc) {
            remove(temp_path);
            proto_set_error(client, "Download-CRC stimmt nicht");
            return 0;
        }
    }
    if (!proto_delete_local_file(local_path)) {
        remove(temp_path);
        wsprintf(message, "Zieldatei konnte nicht geloescht werden (%d)", errno);
        proto_log(client, "TARGET DELETE FAILED path=%s errno=%d", local_path, errno);
        proto_set_error(client, message);
        return 0;
    }
    if (rename(temp_path, local_path) != 0) {
        int rename_error;

        rename_error = errno;
        proto_log(client, "RENAME FAILED temp=%s target=%s errno=%d", temp_path, local_path, rename_error);
        if (!proto_copy_file(temp_path, local_path)) {
            int copy_error;

            copy_error = errno;
            remove(temp_path);
            wsprintf(message, "Download konnte nicht gespeichert werden (%d/%d)", rename_error, copy_error);
            proto_set_error(client, message);
            return 0;
        }
        remove(temp_path);
    }
    proto_write_file_timestamp(local_path, dos_stamp);
    if (client->crc_disabled) {
        proto_forget_cached_crc(client, path);
    } else {
        proto_remember_cached_crc(client, path, expected_size, dos_stamp, expected_crc);
    }
    proto_log(client, "DOWNLOAD %s size=%lu crc=%08lX", path, expected_size, expected_crc);
    return proto_send_line(client, "OK");
}

static int proto_send_upload(ProtoClient *client, ProtoFileItem *item)
{
    char line[PROTO_LINE_CAPACITY];
    char path_hex[PROTO_PATH_CAPACITY * 2];
    unsigned long bytes_done;
    unsigned long last_tick;
    unsigned long started_tick;
    FILE *file;
    char *buffer;
    size_t read_now;

    if (!proto_encode_hex(item->path, path_hex, sizeof(path_hex))) {
        proto_set_error(client, "Upload-Pfad konnte nicht codiert werden");
        return 0;
    }
    sprintf(
        line,
        "PUT path=%s size=%lu crc=%08lX dos=%08lX crc_known=%d",
        path_hex,
        item->size,
        item->crc32,
        item->dos_stamp,
        client->crc_disabled ? 0 : 1
    );
    if (!proto_send_line(client, line)) {
        return 0;
    }

    file = fopen(item->local_path, "rb");
    if (file == NULL) {
        proto_set_error(client, "Upload-Datei konnte nicht geoeffnet werden");
        return 0;
    }

    buffer = (char *)malloc(4096U);
    if (buffer == NULL) {
        fclose(file);
        proto_set_error(client, "Nicht genug Speicher fuer Upload");
        return 0;
    }

    bytes_done = 0UL;
    started_tick = GetTickCount();
    last_tick = 0UL;
    proto_report_transfer(client, "Upload", item->path, 0UL, item->size, started_tick, &last_tick);
    while ((read_now = fread(buffer, 1, 4096U, file)) > 0U) {
        if (!proto_send_all(client, buffer, (int)read_now)) {
            free(buffer);
            fclose(file);
            return 0;
        }
        bytes_done += (unsigned long)read_now;
        proto_report_transfer(client, "Upload", item->path, bytes_done, item->size, started_tick, &last_tick);
    }

    free(buffer);
    fclose(file);
    proto_log(client, "UPLOAD %s size=%lu crc=%08lX", item->path, item->size, item->crc32);
    return 1;
}

static void proto_summary_text(char *summary, unsigned int capacity, int actions, int conflicts, int files)
{
    if (summary == NULL || capacity == 0) {
        return;
    }
    wsprintf(summary, "Sync ok: %d Aktionen, %d Konflikte, %d Dateien", actions, conflicts, files);
}

int proto_sync_directory(
    ProtoClient *client,
    const char *root_dir,
    char *summary,
    unsigned int summary_capacity,
    int *out_actions,
    int *out_conflicts
)
{
    ProtoFileItem *items;
    int item_count;
    int actions;
    int conflicts;
    char line[PROTO_LINE_CAPACITY];
    char kind[16];
    char path_hex[PROTO_PATH_CAPACITY * 2];
    char path_text[PROTO_PATH_CAPACITY];
    int index;

    if (summary != NULL && summary_capacity > 0U) {
        summary[0] = '\0';
    }
    if (out_actions != NULL) {
        *out_actions = 0;
    }
    if (out_conflicts != NULL) {
        *out_conflicts = 0;
    }
    if (root_dir == NULL || root_dir[0] == '\0') {
        proto_set_error(client, "Kein Zielverzeichnis konfiguriert");
        return 0;
    }

    proto_log(client, "SYNC START host=%s port=%u root=%s", client->host, (unsigned int)client->port, root_dir);
    items = (ProtoFileItem *)malloc(sizeof(ProtoFileItem) * PROTO_MAX_FILES);
    if (items == NULL) {
        proto_set_error(client, "Nicht genug Speicher fuer Dateiliste");
        return 0;
    }
    if (!proto_scan_directory(client, root_dir, items, &item_count)) {
        free(items);
        return 0;
    }
    if (!proto_connect(client)) {
        free(items);
        return 0;
    }

    if (!proto_read_line(client, line, sizeof(line))) {
        proto_client_disconnect(client);
        free(items);
        return 0;
    }
    proto_line_kind(line, kind, sizeof(kind));
    if (strcmp(kind, "HELLO") != 0) {
        proto_set_error(client, "Server-HELLO fehlt");
        proto_client_disconnect(client);
        free(items);
        return 0;
    }
    if (!proto_send_line(client, "HELLO proto=1 role=win16")) {
        proto_client_disconnect(client);
        free(items);
        return 0;
    }

    if (!proto_send_line(client, "MANIFEST")) {
        proto_client_disconnect(client);
        free(items);
        return 0;
    }
    for (index = 0; index < item_count; ++index) {
        if (!proto_encode_hex(items[index].path, path_hex, sizeof(path_hex))) {
            proto_set_error(client, "Pfad konnte nicht codiert werden");
            proto_client_disconnect(client);
            free(items);
            return 0;
        }
        wsprintf(
            line,
            "ITEM path=%s size=%lu crc=%08lX dos=%08lX crc_known=%d",
            path_hex,
            items[index].size,
            items[index].crc32,
            items[index].dos_stamp,
            client->crc_disabled ? 0 : 1
        );
        if (!proto_send_line(client, line)) {
            proto_client_disconnect(client);
            free(items);
            return 0;
        }
    }
    if (!proto_send_line(client, "END")) {
        proto_client_disconnect(client);
        free(items);
        return 0;
    }

    actions = 0;
    conflicts = 0;
    while (1) {
        if (!proto_read_line(client, line, sizeof(line))) {
            proto_client_disconnect(client);
            free(items);
            return 0;
        }
        proto_line_kind(line, kind, sizeof(kind));
        if (strcmp(kind, "DONE") == 0) {
            actions = (int)proto_extract_ulong_dec(line, "actions", (unsigned long)actions);
            conflicts = (int)proto_extract_ulong_dec(line, "conflicts", (unsigned long)conflicts);
            break;
        }
        if (strcmp(kind, "ACTION") != 0) {
            proto_set_error(client, "Unerwartete Server-Antwort");
            proto_client_disconnect(client);
            free(items);
            return 0;
        }
        if (!proto_extract_param(line, "path", path_hex, sizeof(path_hex)) ||
            !proto_decode_hex(path_hex, path_text, sizeof(path_text))) {
            proto_set_error(client, "Pfad in Aktion fehlt");
            proto_client_disconnect(client);
            free(items);
            return 0;
        }
        if (!proto_extract_param(line, "kind", kind, sizeof(kind))) {
            proto_set_error(client, "Aktionstyp fehlt");
            proto_client_disconnect(client);
            free(items);
            return 0;
        }

        if (strcmp(kind, "download") == 0) {
            unsigned long size;
            unsigned long crc32;
            unsigned long dos_stamp;

            size = proto_extract_ulong_dec(line, "size", 0UL);
            crc32 = proto_extract_ulong_hex(line, "crc", 0UL);
            dos_stamp = proto_extract_ulong_hex(line, "dos", 0UL);
            if (!proto_receive_download(client, root_dir, path_text, size, crc32, dos_stamp)) {
                proto_client_disconnect(client);
                free(items);
                return 0;
            }
            actions += 1;
        } else if (strcmp(kind, "upload") == 0) {
            int item_index;

            item_index = proto_find_item(items, item_count, path_text);
            if (item_index < 0) {
                proto_set_error(client, "Server verlangt unbekannte Upload-Datei");
                proto_client_disconnect(client);
                free(items);
                return 0;
            }
            if (!proto_send_upload(client, &items[item_index])) {
                proto_client_disconnect(client);
                free(items);
                return 0;
            }
            if (!proto_read_line(client, line, sizeof(line)) || strcmp(line, "OK") != 0) {
                proto_set_error(client, "Upload-OK fehlt");
                proto_client_disconnect(client);
                free(items);
                return 0;
            }
            actions += 1;
        } else if (strcmp(kind, "delete_local") == 0) {
            char local_path[PROTO_LOCAL_PATH_CAPACITY];

            if (!proto_protocol_to_local(local_path, sizeof(local_path), root_dir, path_text)) {
                proto_set_error(client, "Delete-Pfad zu lang");
                proto_client_disconnect(client);
                free(items);
                return 0;
            }
            if (!proto_delete_local_file(local_path)) {
                proto_set_error(client, "Lokale Datei konnte nicht geloescht werden");
                proto_client_disconnect(client);
                free(items);
                return 0;
            }
            proto_forget_cached_crc(client, path_text);
            proto_log(client, "DELETE LOCAL %s", path_text);
            if (!proto_send_line(client, "OK")) {
                proto_client_disconnect(client);
                free(items);
                return 0;
            }
            actions += 1;
        } else if (strcmp(kind, "delete_remote") == 0) {
            proto_log(client, "DELETE REMOTE %s", path_text);
            if (!proto_send_line(client, "OK")) {
                proto_client_disconnect(client);
                free(items);
                return 0;
            }
            actions += 1;
        } else if (strcmp(kind, "conflict") == 0) {
            proto_log(client, "CONFLICT %s", path_text);
            if (!proto_send_line(client, "OK")) {
                proto_client_disconnect(client);
                free(items);
                return 0;
            }
            conflicts += 1;
        } else {
            proto_set_error(client, "Unbekannter Aktionstyp");
            proto_client_disconnect(client);
            free(items);
            return 0;
        }
    }

    proto_client_disconnect(client);
    proto_summary_text(summary, summary_capacity, actions, conflicts, item_count);
    free(items);
    if (out_actions != NULL) {
        *out_actions = actions;
    }
    if (out_conflicts != NULL) {
        *out_conflicts = conflicts;
    }
    proto_log(client, "SYNC DONE actions=%d conflicts=%d files=%d", actions, conflicts, item_count);
    return 1;
}

const char *proto_client_last_error(const ProtoClient *client)
{
    return client != NULL ? client->last_error : "";
}
