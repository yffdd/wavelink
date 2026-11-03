#include "mainwindow.h"
#include "chinesetextmenus.h"
#include <QApplication>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QMutex>
#include <QVector>
#include <QMetaType>
#include <QDir>
#include <windows.h>

#ifndef WAVELINK_VERSION
#define WAVELINK_VERSION "0.14"
#endif
#include <dbghelp.h>
#include <psapi.h>
#include "debuglogger.h"
#include "qtfontwarningfilter.h"

LONG WINAPI TopLevelExceptionHandler(PEXCEPTION_POINTERS pExceptionInfo)
{
    PEXCEPTION_RECORD er = pExceptionInfo->ExceptionRecord;
    PCONTEXT ctx = pExceptionInfo->ContextRecord;

    // --- Exception basic info ---
    QString crashInfo;
    crashInfo += "========== WaveLink Crash Report ==========\n";
    crashInfo += QString("Exception Code : 0x%1").arg(er->ExceptionCode, 8, 16, QChar('0')) + "\n";
    crashInfo += QString("Exception Addr : 0x%1").arg((quintptr)er->ExceptionAddress, 0, 16) + "\n";

    // Translate exception code to human-readable string
    QString codeStr;
    switch (er->ExceptionCode) {
    case 0xC0000005: codeStr = "ACCESS_VIOLATION (内存访问越界)"; break;
    case 0xC0000094: codeStr = "INTEGER_DIVIDE_BY_ZERO (整数除零)"; break;
    case 0xC00000FD: codeStr = "STACK_OVERFLOW (栈溢出)"; break;
    case 0xC0000409: codeStr = "STACK_BUFFER_OVERRUN (栈缓冲区溢出)"; break;
    case 0xC000001D: codeStr = "ILLEGAL_INSTRUCTION (非法指令)"; break;
    case 0xC0000008: codeStr = "INVALID_HANDLE (无效句柄)"; break;
    default: codeStr = "UNKNOWN"; break;
    }
    crashInfo += "Exception Type : " + codeStr + "\n";

    // For ACCESS_VIOLATION, show read/write target address
    if (er->ExceptionCode == 0xC0000005 && er->NumberParameters >= 2) {
        QString rwType = (er->ExceptionInformation[0] == 0) ? "读取(Read)" : "写入(Write)";
        crashInfo += QString("违规操作: 尝试%1地址 0x%2")
            .arg(rwType)
            .arg(er->ExceptionInformation[1], 0, 16) + "\n";
    }

    // --- CPU Register dump ---
    crashInfo += "\n--- CPU 寄存器状态 ---\n";
#ifdef _WIN64
    crashInfo += QString("RAX=0x%1  RBX=0x%2  RCX=0x%3  RDX=0x%4\n")
        .arg(ctx->Rax, 16, 16, QChar('0')).arg(ctx->Rbx, 16, 16, QChar('0'))
        .arg(ctx->Rcx, 16, 16, QChar('0')).arg(ctx->Rdx, 16, 16, QChar('0'));
    crashInfo += QString("RSI=0x%1  RDI=0x%2  RBP=0x%3  RSP=0x%4\n")
        .arg(ctx->Rsi, 16, 16, QChar('0')).arg(ctx->Rdi, 16, 16, QChar('0'))
        .arg(ctx->Rbp, 16, 16, QChar('0')).arg(ctx->Rsp, 16, 16, QChar('0'));
    crashInfo += QString("RIP=0x%1  EFLAGS=0x%2\n")
        .arg(ctx->Rip, 16, 16, QChar('0')).arg(ctx->EFlags, 8, 16, QChar('0'));
    crashInfo += QString("R8 =0x%1  R9 =0x%2  R10=0x%3  R11=0x%4\n")
        .arg(ctx->R8, 16, 16, QChar('0')).arg(ctx->R9, 16, 16, QChar('0'))
        .arg(ctx->R10, 16, 16, QChar('0')).arg(ctx->R11, 16, 16, QChar('0'));
    crashInfo += QString("R12=0x%1  R13=0x%2  R14=0x%3  R15=0x%4\n")
        .arg(ctx->R12, 16, 16, QChar('0')).arg(ctx->R13, 16, 16, QChar('0'))
        .arg(ctx->R14, 16, 16, QChar('0')).arg(ctx->R15, 16, 16, QChar('0'));
#else
    crashInfo += QString("EAX=0x%1  EBX=0x%2  ECX=0x%3  EDX=0x%4\n")
        .arg(ctx->Eax, 8, 16, QChar('0')).arg(ctx->Ebx, 8, 16, QChar('0'))
        .arg(ctx->Ecx, 8, 16, QChar('0')).arg(ctx->Edx, 8, 16, QChar('0'));
    crashInfo += QString("ESI=0x%1  EDI=0x%2  EBP=0x%3  ESP=0x%4\n")
        .arg(ctx->Esi, 8, 16, QChar('0')).arg(ctx->Edi, 8, 16, QChar('0'))
        .arg(ctx->Ebp, 8, 16, QChar('0')).arg(ctx->Esp, 8, 16, QChar('0'));
    crashInfo += QString("EIP=0x%1  EFLAGS=0x%2\n")
        .arg(ctx->Eip, 8, 16, QChar('0')).arg(ctx->EFlags, 8, 16, QChar('0'));
#endif

    // --- Stack trace using CaptureStackBackTrace ---
    crashInfo += "\n--- 调用栈回溯 (Stack Trace) ---\n";
    void* stackFrames[64];
    USHORT frameCount = CaptureStackBackTrace(0, 64, stackFrames, NULL);
    for (USHORT i = 0; i < frameCount; ++i) {
        HMODULE hModule = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCSTR)stackFrames[i], &hModule);
        char moduleName[MAX_PATH] = {0};
        if (hModule) {
            GetModuleFileNameA(hModule, moduleName, MAX_PATH);
            // Extract just the filename
            char *lastSlash = strrchr(moduleName, '\\');
            if (lastSlash) memmove(moduleName, lastSlash + 1, strlen(lastSlash));
        }
        crashInfo += QString("  [%1] 0x%2  (%3+0x%4)\n")
            .arg(i, 2, 10, QChar('0'))
            .arg((quintptr)stackFrames[i], 0, 16)
            .arg(moduleName[0] ? moduleName : "???")
            .arg(hModule ? (quintptr)stackFrames[i] - (quintptr)hModule : 0, 0, 16);
    }

    // --- System Environment ---
    crashInfo += "\n--- 系统环境 ---\n";
    OSVERSIONINFOW osvi;
    ZeroMemory(&osvi, sizeof(osvi));
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    crashInfo += QString("CPU 架构: %1\n").arg(si.wProcessorArchitecture == 9 ? "x64" : "x86");
    crashInfo += QString("CPU 核心数: %1\n").arg(si.dwNumberOfProcessors);
    
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(memInfo);
    GlobalMemoryStatusEx(&memInfo);
    crashInfo += QString("物理内存: %1 MB (可用: %2 MB)\n")
        .arg(memInfo.ullTotalPhys / 1048576)
        .arg(memInfo.ullAvailPhys / 1048576);

    // --- Loaded modules around crash address ---
    crashInfo += "\n--- 已加载模块 (Modules) ---\n";
    HMODULE hMods[256];
    DWORD cbNeeded;
    HANDLE hProcess = GetCurrentProcess();
    if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
        int count = qMin((int)(cbNeeded / sizeof(HMODULE)), 256);
        for (int i = 0; i < count && i < 20; ++i) { // Show first 20 modules
            char modName[MAX_PATH];
            if (GetModuleFileNameA(hMods[i], modName, MAX_PATH)) {
                char *lastSlash = strrchr(modName, '\\');
                crashInfo += QString("  0x%1  %2\n")
                    .arg((quintptr)hMods[i], 0, 16)
                    .arg(lastSlash ? lastSlash + 1 : modName);
            }
        }
    }

    crashInfo += "============================================\n";

    // Do not call the normal Qt logger here: the faulting thread may already
    // own its mutex or the Qt heap may be damaged. Use a best-effort Win32
    // write that cannot deadlock on application locks.
    HANDLE crashFile = CreateFileW(L"WaveLink_crash_report.txt", GENERIC_WRITE,
                                   FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (crashFile != INVALID_HANDLE_VALUE) {
        const WORD bom = 0xFEFF;
        DWORD written = 0;
        WriteFile(crashFile, &bom, sizeof(bom), &written, nullptr);
        const DWORD byteCount = static_cast<DWORD>(crashInfo.size() * sizeof(QChar));
        WriteFile(crashFile, crashInfo.utf16(), byteCount, &written, nullptr);
        FlushFileBuffers(crashFile);
        CloseHandle(crashFile);
    }
    
    return EXCEPTION_CONTINUE_SEARCH;
}

void myMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context);
    static QtFontWarningFilter fontWarnings;
    // Ignore normal qDebug and QtInfoMsg
    if (type == QtDebugMsg || type == QtInfoMsg) {
        return;
    }
    
    switch (type) {
    case QtWarningMsg:
        // Filter out harmless geometry restoration warnings from Qt internals
        if (msg.startsWith("setGeometry: Unable to set geometry")) {
            return;
        }
        {
            const QString message = fontWarnings.messageForLog(type, msg);
            if (!message.isEmpty()) DebugLogger::instance().log(message, "WARN");
        }
        break;
    case QtCriticalMsg:
        DebugLogger::instance().log(msg, "CRIT");
        break;
    case QtFatalMsg:
        DebugLogger::instance().log(msg, "FATAL");
        break;
    default:
        break;
    }
}

int main(int argc, char *argv[])
{
    SetUnhandledExceptionFilter(TopLevelExceptionHandler);
    qInstallMessageHandler(myMessageOutput);

    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication a(argc, argv);
    installChineseTextMenus(a);
    QCoreApplication::setApplicationName(QStringLiteral("WaveLink"));
    QCoreApplication::setApplicationVersion(QStringLiteral(WAVELINK_VERSION));
    
    // Register QVector<double> for cross-thread signal/slot communication
    qRegisterMetaType<QVector<double>>("QVector<double>");
    
    MainWindow w;
    w.show();
    return a.exec();
}
