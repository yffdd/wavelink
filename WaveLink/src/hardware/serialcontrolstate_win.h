#pragma once
#include <QtGlobal>
#ifdef Q_OS_WIN
#include <qt_windows.h>

namespace SerialControlState {
// Modify only the two control-line fields of a DCB returned by GetCommState.
// One SetCommState applies the pair; it is not a guarantee of simultaneous
// physical edges in a USB bridge/driver. Hardware-flow RTS remains automatic.
inline bool update(DCB &state, bool dtr, bool rts, bool hardwareFlow) {
    const DWORD wantedDtr = dtr ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
    const DWORD wantedRts = hardwareFlow ? RTS_CONTROL_HANDSHAKE
        : rts ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE;
    const bool changed = state.fDtrControl != wantedDtr || state.fRtsControl != wantedRts;
    state.fDtrControl = wantedDtr;
    state.fRtsControl = wantedRts;
    return changed;
}
}
#endif
