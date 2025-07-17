#include <sentry.h>

#include <malloc.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    IDT_TIMER_CRASH = 1,
    IDT_TIMER_STACK_OVERFLOW,
    IDT_TIMER_FASTFAIL,
};

static bool
has_arg(int argc, LPWSTR *argv, LPCWSTR arg)
{
    for (int i = 0; i < argc; i++) {
        if (wcscmp(argv[i], arg) == 0) {
            return true;
        }
    }
    return false;
}

static void *invalid_mem = (void *)1;

static void
trigger_crash()
{
    memset((char *)invalid_mem, 1, 100);
}

static void
trigger_stack_overflow()
{
    alloca(1024);
    trigger_stack_overflow();
}

static void
trigger_fastfail_crash()
{
    // this bypasses WINDOWS SEH and will only be caught with the crashpad WER
    // module enabled
    __fastfail(77);
}

static LRESULT CALLBACK
wnd_proc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_TIMER:
        switch (wParam) {
        case IDT_TIMER_CRASH:
            trigger_crash();
            break;
        case IDT_TIMER_STACK_OVERFLOW:
            trigger_stack_overflow();
            break;
        case IDT_TIMER_FASTFAIL:
            trigger_fastfail_crash();
            break;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI
wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine,
    int nCmdShow)
{
    sentry_options_t *options = sentry_options_new();
    sentry_options_set_release(options, "sentry-screenshot");
    sentry_options_set_attach_screenshot(options, true);
    sentry_options_set_debug(options, true);
    sentry_init(options);

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(lpCmdLine, &argc);

#if (WINVER >= 0x0605)
    if (has_arg(argc, argv, L"dpi-unaware")) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
    } else if (has_arg(argc, argv, L"dpi-system-aware")) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    } else if (has_arg(argc, argv, L"dpi-per-monitor-aware")) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    } else if (has_arg(argc, argv, L"dpi-per-monitor-aware-v2")) {
        SetProcessDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    } else if (has_arg(argc, argv, L"dpi-unaware-gdiscaled")) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED);
    }
#endif

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInstance;
    wc.hbrBackground = CreateSolidBrush(RGB(37, 31, 61));
    wc.lpszClassName = L"sentry-screenshot";
    RegisterClass(&wc);

    HWND hwnd = CreateWindowExW(0, L"sentry-screenshot", L"Hello, Sentry!",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 300, 200, NULL, NULL,
        hInstance, NULL);
    ShowWindow(hwnd, nCmdShow);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    UINT delay = 100;
    if (has_arg(argc, argv, L"crash")) {
        SetTimer(hwnd, IDT_TIMER_CRASH, delay, NULL);
    }
    if (has_arg(argc, argv, L"stack-overflow")) {
        SetTimer(hwnd, IDT_TIMER_STACK_OVERFLOW, delay, NULL);
    }
    if (has_arg(argc, argv, L"fastfail")) {
        SetTimer(hwnd, IDT_TIMER_FASTFAIL, delay, NULL);
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    sentry_close();
}
