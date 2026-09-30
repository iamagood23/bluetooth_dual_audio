// DualAudio - play the same system audio on two outputs (e.g. two pairs of Bluetooth earbuds)
// using only documented, user-mode Windows audio APIs. No drivers, no hooks, no admin rights.
#include "application/ApplicationController.h"
#include "common/ComUtil.h"
#include "common/Paths.h"
#include "diagnostics/Logger.h"
#include "ui/MainWindow.h"

#include <windows.h>

using namespace da;

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

    // One instance per user session: a second launch brings the existing window forward.
    UniqueHandle mutex(CreateMutexW(nullptr, TRUE, L"Local\\DualAudio.SingleInstance"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(ui::MainWindow::kClassName, nullptr)) {
            ShowWindow(existing, IsIconic(existing) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    ComInit com(COINIT_APARTMENTTHREADED); // UI thread: STA for shell APIs
    Logger::Get().Start(LogsDirectory(), false);
    LOG_APP(L"DualAudio 1.0.0 starting");

    const bool atStartup = cmdLine && wcsstr(cmdLine, L"--startup") != nullptr;
    int exitCode = 0;
    {
        ApplicationController controller;
        controller.LoadSettings();
        const Settings s = controller.CurrentSettings();

        ui::MainWindow window;
        if (!window.Create(instance, &controller, s.startMinimized)) {
            LOG_ERROR(LogChannel::App, L"Could not create the main window (error %lu)", GetLastError());
            MessageBoxW(nullptr, L"DualAudio could not create its window (Direct2D unavailable?).", L"DualAudio",
                        MB_ICONERROR);
            exitCode = 1;
        } else {
            controller.Initialize(window.Hwnd(), ui::MainWindow::kStateMessage, atStartup);
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        // Stops playback and releases every stream, thread, COM object and handle we own.
        controller.Shutdown();
    }
    LOG_APP(L"DualAudio exited cleanly");
    Logger::Get().Stop();
    return exitCode;
}
