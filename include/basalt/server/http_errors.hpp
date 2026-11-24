#pragma once

#include <string>

namespace basalt::server::detail {

// Stable client payload for unexpected benchmark failures. Internal exception
// details belong in the local diagnostic log, never in this response body.
[[nodiscard]] std::string benchmark_failure_json();

} // namespace basalt::server::detail
