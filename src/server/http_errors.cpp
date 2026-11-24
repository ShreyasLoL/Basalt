#include "basalt/server/http_errors.hpp"

#include <nlohmann/json.hpp>

namespace basalt::server::detail {

std::string benchmark_failure_json() {
    return nlohmann::json{
        {"error", "internal_error"},
        {"code", "INTERNAL_ERROR"},
        {"message", "Benchmark execution failed. See the local server log for details."}
    }.dump();
}

} // namespace basalt::server::detail
