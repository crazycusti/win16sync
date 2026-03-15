#include <ddeml.h>
#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "setupres.h"

#ifdef WIN32
#define COMMAND_ID(wparam, lparam) LOWORD(wparam)
#else
#define COMMAND_ID(wparam, lparam) ((UINT)(wparam))
#endif

#define SETUP_PATH_CAPACITY 160
#define SETUP_STATUS_CAPACITY 160

typedef struct SetupStateTag {
    HINSTANCE instance;
    HWND window;
    char install_dir[SETUP_PATH_CAPACITY];
    char exe_path[SETUP_PATH_CAPACITY];
} SetupState;

static SetupState g_setup;

static void setup_copy_text(char *target, const char *source, unsigned int capacity);
static int setup_append_text(char *target, const char *source, unsigned int capacity);
static int setup_join_path(char *target, unsigned int capacity, const char *directory, const char *name);
static void setup_default_install_dir(char *target, unsigned int capacity);
static void setup_write_controls(HWND window);
static void setup_read_controls(HWND window);
static void setup_update_status(const char *status);
static int setup_ensure_dir(const char *path);
static int setup_extract_client(const char *target_path);
static HDDEDATA CALLBACK setup_dde_callback(
    UINT type,
    UINT format,
    HCONV conversation,
    HSZ string1,
    HSZ string2,
    HDDEDATA data,
    DWORD data1,
    DWORD data2
);
static int setup_progman_command(const char *command);
static int setup_progman_item(const char *exe_path);
static void setup_install(void);

static BOOL FAR PASCAL setup_dialog_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

static void setup_copy_text(char *target, const char *source, unsigned int capacity)
{
    unsigned int index;

    if (target == NULL || capacity == 0U) {
        return;
    }
    if (source == NULL) {
        target[0] = '\0';
        return;
    }

    for (index = 0U; index + 1U < capacity && source[index] != '\0'; ++index) {
        target[index] = source[index];
    }
    target[index] = '\0';
}

static int setup_append_text(char *target, const char *source, unsigned int capacity)
{
    unsigned int target_length;
    unsigned int index;

    if (target == NULL || source == NULL || capacity == 0U) {
        return 0;
    }

    target_length = (unsigned int)strlen(target);
    for (index = 0U; source[index] != '\0'; ++index) {
        if (target_length + index + 1U >= capacity) {
            return 0;
        }
        target[target_length + index] = source[index];
    }
    target[target_length + index] = '\0';
    return 1;
}

static int setup_join_path(char *target, unsigned int capacity, const char *directory, const char *name)
{
    if (target == NULL || directory == NULL || name == NULL || capacity == 0U) {
        return 0;
    }

    target[0] = '\0';
    if (!setup_append_text(target, directory, capacity)) {
        return 0;
    }
    if (target[0] != '\0' && target[strlen(target) - 1] != '\\' && target[strlen(target) - 1] != '/') {
        if (!setup_append_text(target, "\\", capacity)) {
            return 0;
        }
    }
    return setup_append_text(target, name, capacity);
}

static void setup_default_install_dir(char *target, unsigned int capacity)
{
    char windows_dir[SETUP_PATH_CAPACITY];

    windows_dir[0] = '\0';
    GetWindowsDirectory(windows_dir, sizeof(windows_dir));
    if (strlen(windows_dir) >= 2U && windows_dir[1] == ':') {
        target[0] = windows_dir[0];
        target[1] = ':';
        target[2] = '\\';
        target[3] = '\0';
        setup_append_text(target, "W16SYNC", capacity);
        return;
    }

    setup_copy_text(target, "C:\\W16SYNC", capacity);
}

static void setup_write_controls(HWND window)
{
    SetDlgItemText(window, IDC_INSTALLDIR, g_setup.install_dir);
    CheckDlgButton(window, IDC_CREATEGROUP, 1);
    CheckDlgButton(window, IDC_RUNAFTER, 0);
}

static void setup_read_controls(HWND window)
{
    GetDlgItemText(window, IDC_INSTALLDIR, g_setup.install_dir, sizeof(g_setup.install_dir));
}

static void setup_update_status(const char *status)
{
    char line[SETUP_STATUS_CAPACITY];

    wsprintf(line, "Status: %s", status != NULL ? status : "");
    SetDlgItemText(g_setup.window, IDC_SETUPSTATUS, line);
    UpdateWindow(GetDlgItem(g_setup.window, IDC_SETUPSTATUS));
}

static int setup_ensure_dir(const char *path)
{
    char buffer[SETUP_PATH_CAPACITY];
    unsigned int index;

    if (path == NULL || path[0] == '\0') {
        return 0;
    }

    setup_copy_text(buffer, path, sizeof(buffer));
    for (index = 0U; buffer[index] != '\0'; ++index) {
        if ((buffer[index] == '\\' || buffer[index] == '/') && index > 2U) {
            char saved;

            saved = buffer[index];
            buffer[index] = '\0';
            _mkdir(buffer);
            buffer[index] = saved;
        }
    }
    _mkdir(buffer);
    return 1;
}

static int setup_extract_client(const char *target_path)
{
    HRSRC resource;
    HGLOBAL handle;
    DWORD size;
    const void FAR *data;
    FILE *file;

    resource = FindResource(g_setup.instance, MAKEINTRESOURCE(IDR_CLIENTBIN), RT_RCDATA);
    if (resource == NULL) {
        return 0;
    }

    handle = LoadResource(g_setup.instance, resource);
    if (handle == NULL) {
        return 0;
    }

    size = SizeofResource(g_setup.instance, resource);
    data = LockResource(handle);
    if (data == NULL || size == 0UL) {
        return 0;
    }

    file = fopen(target_path, "wb");
    if (file == NULL) {
        return 0;
    }
    if (fwrite(data, 1, (unsigned int)size, file) != (unsigned int)size) {
        fclose(file);
        remove(target_path);
        return 0;
    }
    fclose(file);
    return 1;
}

static HDDEDATA CALLBACK setup_dde_callback(
    UINT type,
    UINT format,
    HCONV conversation,
    HSZ string1,
    HSZ string2,
    HDDEDATA data,
    DWORD data1,
    DWORD data2
)
{
    (void)type;
    (void)format;
    (void)conversation;
    (void)string1;
    (void)string2;
    (void)data;
    (void)data1;
    (void)data2;
    return (HDDEDATA)NULL;
}

static int setup_progman_command(const char *command)
{
    DWORD instance_id;
    HSZ service;
    HSZ topic;
    HCONV conversation;
    HDDEDATA result;
    int success;

    instance_id = 0UL;
    if (DdeInitialize(&instance_id, (PFNCALLBACK)setup_dde_callback, APPCMD_CLIENTONLY, 0UL) != DMLERR_NO_ERROR) {
        return 0;
    }

    service = DdeCreateStringHandle((DWORD)instance_id, "PROGMAN", CP_WINANSI);
    topic = DdeCreateStringHandle((DWORD)instance_id, "PROGMAN", CP_WINANSI);
    conversation = DdeConnect((DWORD)instance_id, service, topic, NULL);
    success = 0;

    if (conversation != NULL) {
        result = DdeClientTransaction(
            (LPBYTE)command,
            (DWORD)(strlen(command) + 1U),
            conversation,
            0,
            0U,
            XTYP_EXECUTE,
            5000U,
            NULL
        );
        success = result != NULL;
        DdeDisconnect(conversation);
    }

    if (service != 0) {
        DdeFreeStringHandle((DWORD)instance_id, service);
    }
    if (topic != 0) {
        DdeFreeStringHandle((DWORD)instance_id, topic);
    }
    DdeUninitialize((DWORD)instance_id);
    return success;
}

static int setup_progman_item(const char *exe_path)
{
    char command[SETUP_STATUS_CAPACITY];

    if (!setup_progman_command("[CreateGroup(\"Win16Sync\")]") ||
        !setup_progman_command("[ShowGroup(\"Win16Sync\",1)]") ||
        !setup_progman_command("[ReplaceItem(\"Win16Sync\")]")) {
        return 0;
    }

    wsprintf(command, "[AddItem(\"%s\",\"Win16Sync\")]", exe_path);
    return setup_progman_command(command);
}

static void setup_install(void)
{
    setup_read_controls(g_setup.window);
    if (g_setup.install_dir[0] == '\0') {
        setup_update_status("Bitte Zielordner angeben");
        return;
    }
    if (!setup_join_path(g_setup.exe_path, sizeof(g_setup.exe_path), g_setup.install_dir, "W16SYNC.EXE")) {
        setup_update_status("Zielpfad zu lang");
        return;
    }

    setup_update_status("Erstelle Zielordner...");
    if (!setup_ensure_dir(g_setup.install_dir)) {
        setup_update_status("Zielordner konnte nicht angelegt werden");
        return;
    }

    setup_update_status("Schreibe Win16Sync...");
    if (!setup_extract_client(g_setup.exe_path)) {
        setup_update_status("Client konnte nicht installiert werden");
        return;
    }

    if (IsDlgButtonChecked(g_setup.window, IDC_CREATEGROUP) == 1) {
        setup_update_status("Erzeuge Progman-Eintrag...");
        if (!setup_progman_item(g_setup.exe_path)) {
            MessageBox(g_setup.window, "Die Datei wurde installiert, aber Progman konnte nicht aktualisiert werden.", "Win16Sync Setup", MB_OK | MB_ICONEXCLAMATION);
        }
    }

    setup_update_status("Fertig");
    if (IsDlgButtonChecked(g_setup.window, IDC_RUNAFTER) == 1) {
        WinExec(g_setup.exe_path, SW_SHOWNORMAL);
    }
    MessageBox(g_setup.window, "Win16Sync wurde installiert.", "Win16Sync Setup", MB_OK | MB_ICONINFORMATION);
    EndDialog(g_setup.window, IDOK);
}

static BOOL FAR PASCAL setup_dialog_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    (void)lparam;

    switch (message) {
    case WM_INITDIALOG:
        g_setup.window = window;
        setup_default_install_dir(g_setup.install_dir, sizeof(g_setup.install_dir));
        setup_write_controls(window);
        setup_update_status("Bereit");
        return TRUE;

    case WM_COMMAND:
        switch (COMMAND_ID(wparam, lparam)) {
        case IDC_INSTALLNOW:
            setup_install();
            return TRUE;

        case IDCANCEL:
            EndDialog(window, IDCANCEL);
            return TRUE;
        }
        break;
    }

    return FALSE;
}

int PASCAL WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show_state)
{
    (void)previous;
    (void)command_line;
    (void)show_state;

    memset(&g_setup, 0, sizeof(g_setup));
    g_setup.instance = instance;
    return DialogBox(instance, MAKEINTRESOURCE(IDD_SETUP), 0, (DLGPROC)setup_dialog_proc);
}
