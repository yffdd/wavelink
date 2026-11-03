// Intentionally has no Qt/DLL dependency: it runs from a temporary directory
// while the installer replaces the entire application (including Qt DLLs).
#include "updatecrypto.h"
#include <shellapi.h>
#include <cwctype>

namespace {
int report(const std::wstring &message) {
#ifdef WAVELINK_UPDATE_LAUNCHER_TEST
    (void)message; return 1;
#else
    MessageBoxW(nullptr, message.c_str(), L"WaveLink 更新", MB_OK | MB_ICONWARNING); return 1;
#endif
}
std::wstring parent(const std::wstring &path) { return path.substr(0, path.find_last_of(L"\\/")); }
bool same(std::wstring a, std::wstring b) {
    for (auto &c : a) if (c == L'/') c = L'\\';
    for (auto &c : b) if (c == L'/') c = L'\\';
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}
std::wstring quote(const std::wstring &s) { return L"\"" + s + L"\""; }
}
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int count = 0; auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments || count != 7) return 2;
    const DWORD pid = wcstoul(arguments[1], nullptr, 10);
    const std::wstring eventName = arguments[2], installer = arguments[3], expectedHash = arguments[4], restart = arguments[5], backup = arguments[6];
    LocalFree(arguments);
    if (!pid || expectedHash.size() != 64 || installer.find(L'"') != std::wstring::npos || restart.find(L'"') != std::wstring::npos)
        return 2;
    wchar_t ownPath[32768]; if (!GetModuleFileNameW(nullptr, ownPath, 32768) || !same(parent(ownPath), parent(installer))) return 2;
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return 2;
    wchar_t processPath[32768]; DWORD pathLength = 32768;
    if (!QueryFullProcessImageNameW(process, 0, processPath, &pathLength) || !same(processPath, restart)) { CloseHandle(process); return 2; }
    // Open without write/delete sharing, and retain the handle through launch:
    // the verified installer cannot be replaced between hashing and execution.
    HANDLE package = CreateFileW(installer.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (package == INVALID_HANDLE_VALUE) { CloseHandle(process); return 2; }
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str());
    if (!ready) { CloseHandle(package); CloseHandle(process); return 2; }
    HANDLE commit = OpenEventW(SYNCHRONIZE, FALSE, (eventName + L"-commit").c_str());
    if (!commit) { CloseHandle(ready); CloseHandle(package); CloseHandle(process); return 2; }
    SetEvent(ready); CloseHandle(ready);
    const DWORD committed = WaitForSingleObject(commit,
#ifdef WAVELINK_UPDATE_LAUNCHER_TEST
        1000
#else
        10000
#endif
    ); CloseHandle(commit);
    // A late/failed handshake must never turn a later ordinary app exit into
    // an unrequested update. Only the normal accepted close commits it.
    if (committed != WAIT_OBJECT_0) { CloseHandle(package); CloseHandle(process); return 2; }
    const DWORD stopped = WaitForSingleObject(process,
#ifdef WAVELINK_UPDATE_LAUNCHER_TEST
        3000
#else
        120000
#endif
    ); CloseHandle(process);
    if (stopped != WAIT_OBJECT_0) { CloseHandle(package); return report(L"主程序尚未安全退出，已取消安装。请结束录制并退出后重试。" ); }
    std::wstring actualHash;
    if (!UpdateCrypto::hashFile(package, actualHash) || !same(actualHash, expectedHash)) {
        CloseHandle(package); return report(L"安装包再次校验失败，已阻止安装。\n配置备份：" + backup);
    }
#ifdef WAVELINK_UPDATE_LAUNCHER_TEST
    // Test-only binary cannot launch an installer or show an elevation prompt.
    HANDLE marker = CreateFileW((parent(installer) + L"\\test-install-verified.marker").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    CloseHandle(package); if (marker == INVALID_HANDLE_VALUE) return 1; CloseHandle(marker); return 0;
#else
    const auto log = parent(installer) + L"\\install.log";
    const auto parameters = L"/SILENT /SP- /NORESTART /NOCLOSEAPPLICATIONS /NOFORCECLOSEAPPLICATIONS /NORESTARTAPPLICATIONS /DIR=" + quote(parent(restart)) + L" /LOG=" + quote(log);
    SHELLEXECUTEINFOW launch{}; launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    launch.lpVerb = L"open"; launch.lpFile = installer.c_str(); launch.lpParameters = parameters.c_str(); launch.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&launch) || !launch.hProcess) {
        CloseHandle(package); return report(L"安装未启动或权限请求已取消。原配置及录制数据未删除。\n请重新打开 WaveLink 后重试。\n配置备份：" + backup);
    }
    WaitForSingleObject(launch.hProcess, INFINITE); DWORD result = 1; GetExitCodeProcess(launch.hProcess, &result); CloseHandle(launch.hProcess); CloseHandle(package);
    if (result != 0) return report(L"安装未完成（代码 " + std::to_wstring(result) + L"），没有自动回滚程序。\n请查看安装日志或重新运行安装包。\n日志：" + log + L"\n配置备份：" + backup);
    // This helper is not elevated, so the restarted app retains the original
    // user's settings instead of accidentally switching to an admin profile.
    SHELLEXECUTEINFOW reopen{}; reopen.cbSize = sizeof(reopen); reopen.fMask = SEE_MASK_NOASYNC;
    reopen.lpFile = restart.c_str(); reopen.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&reopen)) return report(L"安装成功，请手动重新打开 WaveLink。" );
    DeleteFileW(installer.c_str()); // Keep the tiny launcher and install log for diagnosis.
    return 0;
#endif
}
