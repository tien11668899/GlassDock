// start-glass.exe [pid]: loads start-glass.dll (next to this exe) into the Start menu's host,
// StartMenuExperienceHost (or the given process), through XAML Diagnostics, then exits.
// The DLL's folder must be readable by AppContainers (ALL APPLICATION PACKAGES), which the
// Start host is; GlassDock grants that when it installs.
// Exit code: 0 on success, otherwise the HRESULT or 1 when no process was found.

#include <windows.h>
#include <tlhelp32.h>
#include <xamlOM.h>
#include <string>
#include "clsid.h"

static DWORD FindStartHost() {
    DWORD pid = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{ sizeof(entry) };
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"StartMenuExperienceHost.exe") == 0) pid = entry.th32ProcessID;
    }
    CloseHandle(snapshot);
    return pid;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR args, int) {
    DWORD pid = args && *args ? wcstoul(args, nullptr, 10) : FindStartHost();
    if (!pid) return 1;

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dll = exe;
    dll = dll.substr(0, dll.find_last_of(L'\\')) + L"\\start-glass.dll";

    HMODULE xaml = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto initialize = xaml ? reinterpret_cast<decltype(&InitializeXamlDiagnosticsEx)>(GetProcAddress(xaml, "InitializeXamlDiagnosticsEx")) : nullptr;
    if (!initialize) return 2;
    const HRESULT hr = initialize(L"VisualDiagConnection1", pid, nullptr, dll.c_str(), CLSID_StartGlassTap, nullptr);
    return SUCCEEDED(hr) ? 0 : static_cast<int>(hr);
}
