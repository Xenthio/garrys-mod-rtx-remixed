#pragma once

// Only the two loaded-module inspection calls used by the real binding. There
// is deliberately no LoadLibrary stub: introducing a renderer load fails build.
using HMODULE = void*;
using FARPROC = void (*)();
HMODULE GetModuleHandleW(const wchar_t* name);
FARPROC GetProcAddress(HMODULE module, const char* name);
