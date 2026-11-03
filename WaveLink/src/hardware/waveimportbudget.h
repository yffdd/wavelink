#pragma once
#include <QtGlobal>

// Bound raw import vectors before allocation. Graph copies, filtering and
// capacity growth need several times this amount of additional memory.
namespace WaveImportBudget {
inline bool fits(qint64 rows, int channels, qint64 budgetBytes) {
    return rows >= 0 && channels >= 0 && channels <= 256 && budgetBytes > 0 &&
        rows <= budgetBytes / (qint64(channels + 1) * sizeof(double));
}
inline qint64 bytesForAvailableMemory(quint64 available, quint64 configuredMaximum = 256ULL * 1024 * 1024) {
    return qMin<quint64>(configuredMaximum, available / 8);
}
}
