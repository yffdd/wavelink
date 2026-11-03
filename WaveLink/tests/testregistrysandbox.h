#pragma once
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <QUuid>
#include <QString>

// On Windows, QSettings(organization, application) explicitly uses the native
// registry format: setDefaultFormat/setPath do NOT isolate these constructors.
// Redirect HKCU for THIS PROCESS ONLY before constructing any application
// settings. Native QSettings code stays identical to production code.
class TestRegistrySandbox
{
public:
    TestRegistrySandbox()
    {
        m_path = QStringLiteral("Software\\WaveLinkBleUiTests\\") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (RegCreateKeyExW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(m_path.utf16()), 0, nullptr, 0,
                            KEY_ALL_ACCESS, nullptr, &m_key, nullptr) != ERROR_SUCCESS) return;
        m_ready = RegOverridePredefKey(HKEY_CURRENT_USER, m_key) == ERROR_SUCCESS;
    }
    ~TestRegistrySandbox()
    {
        if (m_ready) RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        if (m_key) {
            RegCloseKey(m_key);
            // Exact generated UUID subtree only, never the user's WaveLink key.
            using DeleteTree = LSTATUS (WINAPI *)(HKEY, LPCWSTR);
            const auto deleteTree = reinterpret_cast<DeleteTree>(GetProcAddress(GetModuleHandleW(L"advapi32.dll"), "RegDeleteTreeW"));
            if (deleteTree) deleteTree(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(m_path.utf16()));
        }
    }
    bool ready() const { return m_ready; }
    bool containsApplicationSettings() const
    {
        HKEY appKey = nullptr;
        const bool ok = RegOpenKeyExW(m_key, L"Software\\WaveLink\\WaveLink", 0, KEY_READ, &appKey) == ERROR_SUCCESS;
        if (appKey) RegCloseKey(appKey);
        return ok;
    }
private:
    QString m_path;
    HKEY m_key = nullptr;
    bool m_ready = false;
};
