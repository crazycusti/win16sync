#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dos.h>
#include <windows.h>

#include "proto.h"
#include "resource.h"

#ifdef WIN32
#define COMMAND_ID(wparam, lparam) LOWORD(wparam)
#else
#define COMMAND_ID(wparam, lparam) ((UINT)(wparam))
#endif

#define APP_PATH_CAPACITY 160
#define APP_STATUS_CAPACITY 160
#define APP_TIMER_ID 1
#define APP_DEFAULT_INTERVAL_SECONDS 15
#define APP_CHECKED 1
#define APP_UNCHECKED 0
#define APP_MIN_INTERVAL_SECONDS 1
#define APP_MAX_INTERVAL_SECONDS 3600
#define APP_COMMAND_CAPACITY 320
#define APP_UPDATE_EXE_NAME "W16NEW.EXE"
#define APP_UPDATE_INI_NAME "W16UPD.INI"
#define APP_UPDATE_WAIT_MS 30000UL
#define APP_UPDATE_RETRY_MS 200UL

typedef struct AppStateTag {
    HINSTANCE instance;
    HWND window;
    ProtoClient client;
    int sync_running;
    int autostart;
    int start_minimized;
    int launch_minimized;
    unsigned int sync_interval_seconds;
    char module_path[APP_PATH_CAPACITY];
    char config_path[APP_PATH_CAPACITY];
    char log_path[APP_PATH_CAPACITY];
    char root_dir[APP_PATH_CAPACITY];
    char last_sync[APP_STATUS_CAPACITY];
    char last_status[APP_STATUS_CAPACITY];
    char last_transfer[APP_STATUS_CAPACITY];
} AppState;

static AppState g_app;

static void app_copy_text(char *target, const char *source, unsigned int capacity);
static int app_get_module_path(char *path, unsigned int capacity);
static void app_get_directory_path(char *path);
static const char *app_path_name(const char *path);
static int app_join_path(char *target, unsigned int capacity, const char *directory, const char *name);
static void app_init_paths(void);
static void app_load_config(void);
static int app_save_config(void);
static void app_write_controls(HWND window);
static void app_read_controls(HWND window);
static void app_update_status(const char *status);
static void app_update_transfer(const char *status);
static void app_update_last_sync(const char *status);
static void app_apply_autostart(void);
static void app_apply_timer(void);
static void app_remove_autostart_entry(char *line, unsigned int capacity, const char *exe_path);
static void app_append_token(char *line, unsigned int capacity, const char *token);
static int app_delete_file(const char *path);
static void app_cleanup_update_artifacts(void);
static int app_load_file(const char *path, void **out_data, unsigned long *out_size);
static int app_write_file(const char *path, const void *data, unsigned long size);
static int app_run_update_mode(void);
static void app_run_sync(int manual);
static void app_background(void);
static unsigned int app_parse_interval_text(const char *text);
static void app_show_info(void);
static void app_sync_progress(void *context, const char *status);
static int app_arg_has_switch(const char *command_line, const char *needle);

static BOOL FAR PASCAL main_dialog_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

static void app_copy_text(char *target, const char *source, unsigned int capacity)
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

static int app_get_module_path(char *path, unsigned int capacity)
{
    if (path == NULL || capacity == 0) {
        return 0;
    }

    path[0] = '\0';
    if (GetModuleFileName(g_app.instance, path, capacity) <= 0) {
        return 0;
    }
    return path[0] != '\0';
}

static void app_get_directory_path(char *path)
{
    int index;

    if (path == NULL) {
        return;
    }

    for (index = (int)strlen(path) - 1; index >= 0; --index) {
        if (path[index] == '\\' || path[index] == '/') {
            path[index] = '\0';
            return;
        }
    }
}

static const char *app_path_name(const char *path)
{
    const char *name;
    unsigned int index;

    if (path == NULL || path[0] == '\0') {
        return "";
    }

    name = path;
    for (index = 0; path[index] != '\0'; ++index) {
        if (path[index] == '\\' || path[index] == '/') {
            name = path + index + 1U;
        }
    }
    return name;
}

static int app_join_path(char *target, unsigned int capacity, const char *directory, const char *name)
{
    if (target == NULL || capacity == 0 || directory == NULL || name == NULL) {
        return 0;
    }

    target[0] = '\0';
    if (!directory[0]) {
        app_copy_text(target, name, capacity);
        return target[0] != '\0';
    }
    app_copy_text(target, directory, capacity);
    if (target[strlen(target) - 1] != '\\' && target[strlen(target) - 1] != '/') {
        if (strlen(target) + 2 >= capacity) {
            return 0;
        }
        strcat(target, "\\");
    }
    if (strlen(target) + strlen(name) + 1 >= capacity) {
        return 0;
    }
    strcat(target, name);
    return 1;
}

static void app_init_paths(void)
{
    char module_path[APP_PATH_CAPACITY];

    if (!app_get_module_path(g_app.module_path, sizeof(g_app.module_path))) {
        g_app.module_path[0] = '\0';
        GetWindowsDirectory(module_path, sizeof(module_path));
    } else {
        app_copy_text(module_path, g_app.module_path, sizeof(module_path));
    }
    app_get_directory_path(module_path);
    app_join_path(g_app.config_path, sizeof(g_app.config_path), module_path, "W16SYNC.INI");
    app_join_path(g_app.log_path, sizeof(g_app.log_path), module_path, "W16SYNC.LOG");
    proto_client_set_app_info(&g_app.client, WIN16SYNC_APP_VERSION, g_app.module_path);
    proto_client_set_log_path(&g_app.client, g_app.log_path);
}

static void app_load_config(void)
{
    char interval_text[16];
    char port_text[16];

    GetPrivateProfileString("server", "host", g_app.client.host, g_app.client.host, sizeof(g_app.client.host), g_app.config_path);
    GetPrivateProfileString("server", "port", "9071", port_text, sizeof(port_text), g_app.config_path);
    GetPrivateProfileString("client", "root_dir", "C:\\SYNC", g_app.root_dir, sizeof(g_app.root_dir), g_app.config_path);
    GetPrivateProfileString("client", "interval_seconds", "15", interval_text, sizeof(interval_text), g_app.config_path);
    g_app.client.crc_disabled = GetPrivateProfileInt("client", "crc_disabled", 0, g_app.config_path) ? 1 : 0;
    g_app.client.cache_recheck_enabled = GetPrivateProfileInt("client", "crc_recheck", 0, g_app.config_path) ? 1 : 0;
    g_app.autostart = GetPrivateProfileInt("client", "autostart", 0, g_app.config_path) ? 1 : 0;
    g_app.start_minimized = GetPrivateProfileInt("client", "start_minimized", 0, g_app.config_path) ? 1 : 0;
    g_app.client.port = (unsigned short)atoi(port_text);
    g_app.sync_interval_seconds = app_parse_interval_text(interval_text);
    if (g_app.client.port == 0) {
        g_app.client.port = 9071;
    }
}

static int app_save_config(void)
{
    char interval_text[16];
    char port_text[16];
    char flag_text[8];

    wsprintf(port_text, "%u", (unsigned int)g_app.client.port);
    wsprintf(interval_text, "%u", g_app.sync_interval_seconds);
    wsprintf(flag_text, "%d", g_app.autostart ? 1 : 0);
    if (!WritePrivateProfileString("server", "host", g_app.client.host, g_app.config_path) ||
        !WritePrivateProfileString("server", "port", port_text, g_app.config_path) ||
        !WritePrivateProfileString("client", "root_dir", g_app.root_dir, g_app.config_path) ||
        !WritePrivateProfileString("client", "interval_seconds", interval_text, g_app.config_path) ||
        !WritePrivateProfileString("client", "crc_disabled", g_app.client.crc_disabled ? "1" : "0", g_app.config_path) ||
        !WritePrivateProfileString("client", "crc_recheck", g_app.client.cache_recheck_enabled ? "1" : "0", g_app.config_path) ||
        !WritePrivateProfileString("client", "autostart", flag_text, g_app.config_path)) {
        return 0;
    }
    wsprintf(flag_text, "%d", g_app.start_minimized ? 1 : 0);
    if (!WritePrivateProfileString("client", "start_minimized", flag_text, g_app.config_path)) {
        return 0;
    }
    WritePrivateProfileString(NULL, NULL, NULL, g_app.config_path);
    app_apply_autostart();
    app_apply_timer();
    return 1;
}

static void app_write_controls(HWND window)
{
    char interval_text[16];
    char port_text[16];

    SetDlgItemText(window, IDC_HOST, g_app.client.host);
    wsprintf(port_text, "%u", (unsigned int)g_app.client.port);
    wsprintf(interval_text, "%u", g_app.sync_interval_seconds);
    SetDlgItemText(window, IDC_PORT, port_text);
    SetDlgItemText(window, IDC_ROOTDIR, g_app.root_dir);
    SetDlgItemText(window, IDC_INTERVAL, interval_text);
    CheckDlgButton(window, IDC_CRCOFF, g_app.client.crc_disabled ? APP_CHECKED : APP_UNCHECKED);
    CheckDlgButton(window, IDC_CRCRECHECK, g_app.client.cache_recheck_enabled ? APP_CHECKED : APP_UNCHECKED);
    CheckDlgButton(window, IDC_AUTOSTART, g_app.autostart ? APP_CHECKED : APP_UNCHECKED);
    CheckDlgButton(window, IDC_STARTMIN, g_app.start_minimized ? APP_CHECKED : APP_UNCHECKED);
}

static void app_read_controls(HWND window)
{
    char interval_text[16];
    char port_text[16];

    GetDlgItemText(window, IDC_HOST, g_app.client.host, sizeof(g_app.client.host));
    GetDlgItemText(window, IDC_PORT, port_text, sizeof(port_text));
    GetDlgItemText(window, IDC_ROOTDIR, g_app.root_dir, sizeof(g_app.root_dir));
    GetDlgItemText(window, IDC_INTERVAL, interval_text, sizeof(interval_text));
    g_app.client.port = (unsigned short)atoi(port_text);
    g_app.sync_interval_seconds = app_parse_interval_text(interval_text);
    if (g_app.client.port == 0) {
        g_app.client.port = 9071;
    }
    g_app.client.crc_disabled = IsDlgButtonChecked(window, IDC_CRCOFF) == APP_CHECKED;
    g_app.client.cache_recheck_enabled = IsDlgButtonChecked(window, IDC_CRCRECHECK) == APP_CHECKED;
    g_app.autostart = IsDlgButtonChecked(window, IDC_AUTOSTART) == APP_CHECKED;
    g_app.start_minimized = IsDlgButtonChecked(window, IDC_STARTMIN) == APP_CHECKED;
}

static void app_update_status(const char *status)
{
    char line[APP_STATUS_CAPACITY];

    app_copy_text(g_app.last_status, status, sizeof(g_app.last_status));
    wsprintf(line, "Status: %s", g_app.last_status);
    SetDlgItemText(g_app.window, IDC_STATUS, line);
    UpdateWindow(GetDlgItem(g_app.window, IDC_STATUS));
}

static void app_update_transfer(const char *status)
{
    char line[APP_STATUS_CAPACITY];

    app_copy_text(g_app.last_transfer, status, sizeof(g_app.last_transfer));
    wsprintf(line, "Transfer: %s", g_app.last_transfer);
    SetDlgItemText(g_app.window, IDC_TRANSFER, line);
    UpdateWindow(GetDlgItem(g_app.window, IDC_TRANSFER));
}

static void app_update_last_sync(const char *status)
{
    char line[APP_STATUS_CAPACITY];

    app_copy_text(g_app.last_sync, status, sizeof(g_app.last_sync));
    wsprintf(line, "Letzter Sync: %s", g_app.last_sync);
    SetDlgItemText(g_app.window, IDC_LASTSYNC, line);
    UpdateWindow(GetDlgItem(g_app.window, IDC_LASTSYNC));
}

static void app_remove_autostart_entry(char *line, unsigned int capacity, const char *exe_path)
{
    char result[512];
    char token[APP_PATH_CAPACITY];
    unsigned int index;
    unsigned int token_index;
    int skip_next_min;

    (void)capacity;

    result[0] = '\0';
    index = 0U;
    skip_next_min = 0;
    while (line[index] != '\0') {
        while (line[index] == ' ') {
            index += 1U;
        }
        if (line[index] == '\0') {
            break;
        }
        token_index = 0U;
        while (line[index] != '\0' && line[index] != ' ' && token_index + 1U < sizeof(token)) {
            token[token_index++] = line[index++];
        }
        token[token_index] = '\0';
        if (strcmp(token, exe_path) == 0) {
            skip_next_min = 1;
            continue;
        }
        if (skip_next_min && (strcmp(token, "/MIN") == 0 || strcmp(token, "/min") == 0)) {
            skip_next_min = 0;
            continue;
        }
        skip_next_min = 0;
        app_append_token(result, sizeof(result), token);
    }
    app_copy_text(line, result, capacity);
}

static void app_append_token(char *line, unsigned int capacity, const char *token)
{
    if (line[0] != '\0') {
        if (strlen(line) + 2 >= capacity) {
            return;
        }
        strcat(line, " ");
    }
    if (strlen(line) + strlen(token) + 1 >= capacity) {
        return;
    }
    strcat(line, token);
}

static void app_apply_autostart(void)
{
    char module_path[APP_PATH_CAPACITY];
    char run_line[512];
    char load_line[512];

    if (!app_get_module_path(module_path, sizeof(module_path))) {
        return;
    }

    GetProfileString("windows", "run", "", run_line, sizeof(run_line));
    GetProfileString("windows", "load", "", load_line, sizeof(load_line));
    app_remove_autostart_entry(run_line, sizeof(run_line), module_path);
    app_remove_autostart_entry(load_line, sizeof(load_line), module_path);

    if (g_app.autostart) {
        if (g_app.start_minimized) {
            app_append_token(load_line, sizeof(load_line), module_path);
        } else {
            app_append_token(run_line, sizeof(run_line), module_path);
        }
    }

    WriteProfileString("windows", "run", run_line);
    WriteProfileString("windows", "load", load_line);
}

static int app_delete_file(const char *path)
{
    unsigned attr;

    if (path == NULL || path[0] == '\0') {
        return 1;
    }
    if (_dos_getfileattr(path, &attr) != 0U) {
        return 1;
    }
    _dos_setfileattr(path, _A_NORMAL);
    if (remove(path) != 0) {
        return 0;
    }
    return 1;
}

static void app_cleanup_update_artifacts(void)
{
    char module_path[APP_PATH_CAPACITY];
    char module_dir[APP_PATH_CAPACITY];
    char update_path[APP_PATH_CAPACITY];
    char update_ini[APP_PATH_CAPACITY];

    if (!app_get_module_path(module_path, sizeof(module_path))) {
        return;
    }
    if (strcmp(app_path_name(module_path), APP_UPDATE_EXE_NAME) == 0) {
        return;
    }

    app_copy_text(module_dir, module_path, sizeof(module_dir));
    app_get_directory_path(module_dir);
    if (!app_join_path(update_path, sizeof(update_path), module_dir, APP_UPDATE_EXE_NAME) ||
        !app_join_path(update_ini, sizeof(update_ini), module_dir, APP_UPDATE_INI_NAME)) {
        return;
    }

    app_delete_file(update_ini);
    app_delete_file(update_path);
}

static int app_load_file(const char *path, void **out_data, unsigned long *out_size)
{
    FILE *file;
    long file_size;
    void *data;

    if (out_data != NULL) {
        *out_data = NULL;
    }
    if (out_size != NULL) {
        *out_size = 0UL;
    }
    if (path == NULL || path[0] == '\0') {
        return 0;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    file_size = ftell(file);
    if (file_size <= 0L || fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }

    data = malloc((unsigned)file_size);
    if (data == NULL) {
        fclose(file);
        return 0;
    }
    if (fread(data, 1, (unsigned)file_size, file) != (unsigned)file_size) {
        free(data);
        fclose(file);
        return 0;
    }

    fclose(file);
    if (out_data != NULL) {
        *out_data = data;
    } else {
        free(data);
    }
    if (out_size != NULL) {
        *out_size = (unsigned long)file_size;
    }
    return 1;
}

static int app_write_file(const char *path, const void *data, unsigned long size)
{
    FILE *file;

    file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }
    if (size > 0UL && fwrite(data, 1, (unsigned)size, file) != (unsigned)size) {
        fclose(file);
        app_delete_file(path);
        return 0;
    }
    fclose(file);
    return 1;
}

static int app_run_update_mode(void)
{
    char source_path[APP_PATH_CAPACITY];
    char source_dir[APP_PATH_CAPACITY];
    char update_ini[APP_PATH_CAPACITY];
    char target_path[APP_PATH_CAPACITY];
    void *data;
    unsigned long size;
    unsigned long started_tick;
    unsigned long last_try_tick;

    if (!app_get_module_path(source_path, sizeof(source_path))) {
        MessageBox(0, "Updatequelle konnte nicht gefunden werden.", "Win16Sync Update", MB_OK | MB_ICONEXCLAMATION);
        return 1;
    }

    app_copy_text(source_dir, source_path, sizeof(source_dir));
    app_get_directory_path(source_dir);
    if (!app_join_path(update_ini, sizeof(update_ini), source_dir, APP_UPDATE_INI_NAME)) {
        MessageBox(0, "Update-Steuerdatei fehlt.", "Win16Sync Update", MB_OK | MB_ICONEXCLAMATION);
        return 1;
    }

    GetPrivateProfileString("update", "target_path", "", target_path, sizeof(target_path), update_ini);
    if (target_path[0] == '\0') {
        MessageBox(0, "Update-Ziel fehlt.", "Win16Sync Update", MB_OK | MB_ICONEXCLAMATION);
        return 1;
    }
    if (!app_load_file(source_path, &data, &size)) {
        MessageBox(0, "Update-Datei konnte nicht geladen werden.", "Win16Sync Update", MB_OK | MB_ICONEXCLAMATION);
        return 1;
    }

    started_tick = GetTickCount();
    last_try_tick = 0UL;
    while (1) {
        unsigned long now;

        now = GetTickCount();
        if (last_try_tick == 0UL || now - last_try_tick >= APP_UPDATE_RETRY_MS) {
            last_try_tick = now;
            app_delete_file(target_path);
            if (app_write_file(target_path, data, size)) {
                break;
            }
        }
        if (now - started_tick >= APP_UPDATE_WAIT_MS) {
            free(data);
            MessageBox(0, "Update konnte nicht installiert werden.", "Win16Sync Update", MB_OK | MB_ICONEXCLAMATION);
            return 1;
        }
        Yield();
    }

    free(data);
    app_delete_file(update_ini);
    if (WinExec(target_path, SW_SHOWNORMAL) <= 31) {
        MessageBox(0, "Aktualisierte Win16Sync.exe konnte nicht gestartet werden.", "Win16Sync Update", MB_OK | MB_ICONEXCLAMATION);
        return 1;
    }
    return 0;
}

static void app_apply_timer(void)
{
    if (g_app.window == 0) {
        return;
    }

    KillTimer(g_app.window, APP_TIMER_ID);
    SetTimer(g_app.window, APP_TIMER_ID, (UINT)(g_app.sync_interval_seconds * 1000U), NULL);
}

static unsigned int app_parse_interval_text(const char *text)
{
    unsigned int value;

    value = (unsigned int)atoi(text != NULL ? text : "");
    if (value < APP_MIN_INTERVAL_SECONDS || value > APP_MAX_INTERVAL_SECONDS) {
        value = APP_DEFAULT_INTERVAL_SECONDS;
    }
    return value;
}

static void app_show_info(void)
{
    char text[APP_STATUS_CAPACITY];

    wsprintf(
        text,
        "Win16Sync %s\r\nWynton Grund 2026\r\nMade with Codex <3",
        WIN16SYNC_APP_VERSION
    );
    MessageBox(
        g_app.window,
        text,
        "Info",
        MB_OK
    );
}

static void app_sync_progress(void *context, const char *status)
{
    (void)context;
    app_update_transfer(status != NULL && status[0] != '\0' ? status : "-");
    UpdateWindow(g_app.window);
}

static void app_run_sync(int manual)
{
    char summary[PROTO_SUMMARY_CAPACITY];
    char last_sync[APP_STATUS_CAPACITY];
    int sync_result;
    const char *notice;

    if (g_app.sync_running) {
        return;
    }

    g_app.sync_running = 1;
    app_read_controls(g_app.window);
    if (manual && !app_save_config()) {
        app_update_status("Konfiguration konnte nicht gespeichert werden");
        g_app.sync_running = 0;
        return;
    }

    app_update_status("Synchronisiere...");
    app_update_transfer("Scanne...");
    sync_result = proto_sync_directory(&g_app.client, g_app.root_dir, summary, sizeof(summary), NULL, NULL);
    notice = proto_client_last_notice(&g_app.client);
    if (sync_result == PROTO_SYNC_RESULT_OK) {
        app_update_status("Verbunden");
        app_update_transfer("-");
        if (manual && notice != NULL && notice[0] != '\0' &&
            strlen(summary) + strlen(notice) + 4U < sizeof(last_sync)) {
            wsprintf(last_sync, "%s | %s", summary, notice);
            app_update_last_sync(last_sync);
        } else {
            app_update_last_sync(summary);
        }
    } else if (sync_result == PROTO_SYNC_RESULT_UPDATE) {
        app_update_status(notice != NULL && notice[0] != '\0' ? notice : "Update wird installiert");
        app_update_transfer("-");
        app_update_last_sync(summary[0] != '\0' ? summary : "Update geladen, Neustart");
        g_app.sync_running = 0;
        KillTimer(g_app.window, APP_TIMER_ID);
        EndDialog(g_app.window, IDOK);
        return;
    } else {
        app_update_status(proto_client_last_error(&g_app.client));
        app_update_transfer("-");
        app_update_last_sync("Fehlgeschlagen");
    }
    g_app.sync_running = 0;
}

static void app_background(void)
{
    ShowWindow(g_app.window, SW_MINIMIZE);
}

static int app_arg_has_switch(const char *command_line, const char *needle)
{
    char upper_line[128];
    char upper_need[32];
    unsigned int index;

    app_copy_text(upper_line, command_line != NULL ? command_line : "", sizeof(upper_line));
    app_copy_text(upper_need, needle, sizeof(upper_need));
    for (index = 0; upper_line[index] != '\0'; ++index) {
        if (upper_line[index] >= 'a' && upper_line[index] <= 'z') {
            upper_line[index] = (char)(upper_line[index] - 'a' + 'A');
        }
    }
    for (index = 0; upper_need[index] != '\0'; ++index) {
        if (upper_need[index] >= 'a' && upper_need[index] <= 'z') {
            upper_need[index] = (char)(upper_need[index] - 'a' + 'A');
        }
    }
    return strstr(upper_line, upper_need) != NULL;
}

static BOOL FAR PASCAL main_dialog_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    (void)lparam;

    switch (message) {
    case WM_INITDIALOG:
        g_app.window = window;
        app_write_controls(window);
        app_apply_timer();
        SetDlgItemText(window, IDC_LOGINFO, "Log: W16SYNC.LOG");
        app_update_status("Bereit");
        app_update_transfer("-");
        app_update_last_sync("-");
        if (g_app.launch_minimized) {
            ShowWindow(window, SW_MINIMIZE);
        }
        return TRUE;

    case WM_TIMER:
        if (wparam == APP_TIMER_ID) {
            app_run_sync(0);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        app_background();
        return TRUE;

    case WM_COMMAND:
        switch (COMMAND_ID(wparam, lparam)) {
        case IDC_SAVE:
            app_read_controls(window);
            if (app_save_config()) {
                app_update_status("Konfiguration gespeichert");
            } else {
                app_update_status("Konfiguration konnte nicht gespeichert werden");
            }
            return TRUE;

        case IDC_SYNCNOW:
            app_run_sync(1);
            return TRUE;

        case IDC_INFOBTN:
            app_show_info();
            return TRUE;

        case IDC_BACKGROUND:
            app_background();
            return TRUE;

        case IDC_EXIT:
            app_read_controls(window);
            app_save_config();
            KillTimer(window, APP_TIMER_ID);
            EndDialog(window, IDOK);
            return TRUE;
        }
        break;
    }

    return FALSE;
}

int PASCAL WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    int result;

    (void)hPrevInstance;
    (void)nCmdShow;

    memset(&g_app, 0, sizeof(g_app));
    g_app.instance = hInstance;
    if (app_arg_has_switch(lpCmdLine, "/SELFUPDATE") || app_arg_has_switch(lpCmdLine, "-SELFUPDATE")) {
        return app_run_update_mode();
    }
    proto_client_init(&g_app.client);
    app_init_paths();
    app_cleanup_update_artifacts();
    proto_client_set_progress_callback(&g_app.client, app_sync_progress, &g_app);
    app_load_config();

    g_app.launch_minimized = app_arg_has_switch(lpCmdLine, "/MIN") || app_arg_has_switch(lpCmdLine, "-MIN") || g_app.start_minimized;
    result = DialogBox(hInstance, MAKEINTRESOURCE(IDD_MAIN), 0, (DLGPROC)main_dialog_proc);

    return result;
}
