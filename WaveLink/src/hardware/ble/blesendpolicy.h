#pragma once
#include <algorithm>

// Shared by the Qt controller and the independent Windows helper. This is a
// conservative ordinary-write policy, not a claim about a module's UART buffer
// or application-level acknowledgement. No long/prepared writes are used.
namespace BleSendPolicy {
constexpr int DefaultPacket = 20, DefaultInterval = 10, BackendLimit = 512;
inline bool valid(int packet, int interval) { return packet >= 1 && packet <= BackendLimit && interval >= 0 && interval <= 1000; }
inline bool needsConfirmation(int packet, int interval) { return packet > DefaultPacket || interval < DefaultInterval; }
inline int limit(int mtu) { return mtu >= 23 ? std::min(BackendLimit, mtu - 3) : DefaultPacket; }
inline int effective(int requested, int mtu) { return std::min(requested, limit(mtu)); }
}
