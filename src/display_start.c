#define main hid_session_sender_internal_main
#include "hid_session_sender.c"
#undef main

#include <stdlib.h>
#include <stdarg.h>
#include <time.h>

#define DISPLAY_MUTEX_NAME "Global\\AMG65DisplayPlayerMutex"
#define DISPLAY_STOP_EVENT_NAME "Global\\AMG65DisplayStopEvent"
#define RECONNECT_DELAY_MS 1000

static bool should_stop(HANDLE stop_event) {
    if (stop_event && WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
        return true;
    }
    return InterlockedCompareExchange(&g_stop_requested, 0, 0) != 0;
}

static void daemon_log(const char *exe_dir, const char *fmt, ...) {
    char log_path[MAX_PATH * 2] = {0};
    join_path2(log_path, sizeof(log_path), exe_dir, "markov.log");

    FILE *fp = fopen(log_path, "a");
    if (!fp) return;

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_s(&tm_now, &now);

    fprintf(
        fp,
        "[%04d-%02d-%02d %02d:%02d:%02d] ",
        tm_now.tm_year + 1900,
        tm_now.tm_mon + 1,
        tm_now.tm_mday,
        tm_now.tm_hour,
        tm_now.tm_min,
        tm_now.tm_sec
    );

    va_list args;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);

    fputc('\n', fp);
    fclose(fp);
}

static int run_display_daemon(DWORD delay_ms) {
    HANDLE mutex = CreateMutexA(NULL, FALSE, DISPLAY_MUTEX_NAME);
    if (!mutex) return 71;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    HANDLE stop_event = CreateEventA(NULL, TRUE, FALSE, DISPLAY_STOP_EVENT_NAME);
    if (!stop_event) {
        CloseHandle(mutex);
        return 72;
    }

    improve_priority();
    srand(generate_markov_seed());

    char exe_dir[MAX_PATH] = {0};
    char asset_dir[MAX_PATH * 2] = {0};
    get_exe_dir(exe_dir, sizeof(exe_dir));
    join_path2(asset_dir, sizeof(asset_dir), exe_dir, "animation_segments");

    MarkovPlayer player;
    if (!init_markov_player(&player, asset_dir)) {
        daemon_log(exe_dir, "asset init failed: %s", asset_dir);
        CloseHandle(stop_event);
        CloseHandle(mutex);
        return 74;
    }

    daemon_log(exe_dir, "daemon started delay_ms=%lu", (unsigned long)delay_ms);

    Color frame[PIXELS_PER_FRAME];
    Packet65 session[19];
    while (!should_stop(stop_event)) {
        char chosen_path[1024] = {0};
        HANDLE h = open_target_device(chosen_path, sizeof(chosen_path));
        if (h == INVALID_HANDLE_VALUE) {
            daemon_log(exe_dir, "device not found; retrying");
            Sleep(RECONNECT_DELAY_MS);
            continue;
        }

        daemon_log(exe_dir, "device connected: %s", chosen_path);
        render_markov_next_frame(&player, frame);
        while (!should_stop(stop_event)) {
            build_session_packets_from_frame(frame, session);
            if (!send_session_quiet(h, session, 19, delay_ms, true)) {
                daemon_log(exe_dir, "device write failed: %lu; reconnecting", GetLastError());
                break;
            }
            render_markov_next_frame(&player, frame);
        }
        CloseHandle(h);
        if (!should_stop(stop_event)) {
            Sleep(RECONNECT_DELAY_MS);
        }
    }

    daemon_log(exe_dir, "daemon stopped");
    free_markov_player(&player);
    CloseHandle(stop_event);
    CloseHandle(mutex);
    return 0;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR cmdline, int show) {
    (void)instance;
    (void)prev;
    (void)cmdline;
    (void)show;

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    int argc = __argc;
    char **argv = __argv;
    bool daemon = false;
    DWORD delay_ms = DEFAULT_DELAY_MS;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--daemon") == 0) {
            daemon = true;
        } else {
            delay_ms = (DWORD)strtoul(argv[i], NULL, 10);
        }
    }

    if (daemon) {
        return run_display_daemon(delay_ms);
    }

    HANDLE existing = OpenMutexA(SYNCHRONIZE, FALSE, DISPLAY_MUTEX_NAME);
    if (existing) {
        CloseHandle(existing);
        return 0;
    }

    char exe_path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);

    char cmd[2048] = {0};
    snprintf(cmd, sizeof(cmd), "\"%s\" --daemon %lu", exe_path, (unsigned long)delay_ms);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    BOOL ok = CreateProcessA(
        exe_path,
        cmd,
        NULL,
        NULL,
        FALSE,
        CREATE_NO_WINDOW | DETACHED_PROCESS,
        NULL,
        NULL,
        &si,
        &pi
    );
    if (!ok) {
        return 75;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
