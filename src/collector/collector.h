// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/metric_source.h"

namespace budyk {
// Factory — returns platform-specific MetricSource
MetricSource* create_collector();
} // namespace budyk
