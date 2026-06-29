#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>

#define DISPLAY_STOP_EVENT_NAME "Global\\AMG65DisplayStopEvent"

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR cmdline, int show) {
    (void)instance;
    (void)prev;
    (void)cmdline;
    (void)show;

    HANDLE ev = OpenEventA(EVENT_MODIFY_STATE, FALSE, DISPLAY_STOP_EVENT_NAME);
    if (!ev) {
        return 0;
    }
    SetEvent(ev);
    CloseHandle(ev);
    return 0;
}
