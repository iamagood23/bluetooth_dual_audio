#pragma once
#include <string>

namespace da {

// Folder containing DualAudio.exe.
std::wstring ExeDirectory();
// Where config/ and logs/ live: next to the exe when writable (portable), otherwise
// %LOCALAPPDATA%\DualAudio. Created on first call.
std::wstring DataDirectory();
std::wstring ConfigDirectory();
std::wstring LogsDirectory();
bool EnsureDirectory(const std::wstring& path);

} // namespace da
