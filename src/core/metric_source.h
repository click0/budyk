// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"

namespace budyk {

class MetricSource {
public:
    virtual ~MetricSource() = default;
    virtual int collect(Sample* out, Level level) = 0;
};

} // namespace budyk
