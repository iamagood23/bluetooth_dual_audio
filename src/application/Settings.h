#pragma once
// User settings, stored as UTF-8 key=value text in config\settings.ini next to the app
// (or %LOCALAPPDATA%\DualAudio if that folder is read-only). Never in the registry.
#include "audio/AudioEngine.h"

#include <string>

namespace da {

enum class ThemeMode { System = 0, Light, Dark };
enum class SourceMode { AllApps = 0, SingleApp };

struct Settings {
    // General
    ThemeMode theme = ThemeMode::System;
    bool startMinimized = false;
    bool autoStart = false;       // start dual audio automatically at launch
    bool runAtStartup = false;    // mirrors the Startup-folder shortcut
    // Audio
    BufferMode buffer = BufferMode::Balanced;
    int sampleRate = 48000;       // internal timeline rate
    SyncStrength strength = SyncStrength::Normal;
    bool micCalibration = true;   // allow microphone-based auto calibration
    bool clipProtection = true;
    bool syncEnabled = true;
    SourceMode source = SourceMode::AllApps;
    std::wstring sourceApp;       // executable name for SingleApp
    // Diagnostics
    bool showDiagnostics = false;
    bool verboseLogging = false;
    // Outputs
    std::wstring outputId[2];
    std::wstring outputName[2];   // last known name, shown if the device is absent
    float volume[2] = {1.0f, 1.0f};
    bool muted[2] = {false, false};
    double offsetMs[2] = {0.0, 0.0};

    static std::wstring DefaultPath();
    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path) const;
};

// "Open at Windows startup" via a shortcut in the user's Startup folder (no registry writes).
namespace StartupShortcut {
bool IsEnabled();
bool SetEnabled(bool enabled);
} // namespace StartupShortcut

} // namespace da
