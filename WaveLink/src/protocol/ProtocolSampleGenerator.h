#pragma once
#include "ProtocolProfile.h"

namespace DataProtocols {
// Produces a locally validated sample (one frame; header-only framing also
// appends the following header as lookahead). Never writes or sends data.
class ProtocolSampleGenerator {
public:
    static bool generate(const ProtocolProfile &profile, QByteArray &bytes, QString &error, int arrayChannels = 0);
};
}
