#include "../src/server/http_validation.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    constexpr std::size_t maximum_body_size = 64U * 1024U;
    if (size > maximum_body_size + 1U) return 0;
    const auto selector = size == 0 ? 0U : static_cast<unsigned int>(data[0] % 4U);
    static constexpr char empty[] = "";
    const auto* bytes = size <= 1U ? empty : reinterpret_cast<const char*>(data + 1);
    const auto body_size = size <= 1U ? 0U : size - 1U;
    const std::string body(bytes, body_size);
    const auto schema = static_cast<basalt::server::detail::MutationBodySchema>(selector);
    const auto result = basalt::server::detail::validate_mutation_body(body, schema);
    if (result && !result.body.is_object()) std::abort();
    if (!result && (result.code == nullptr || result.message == nullptr)) std::abort();
    return 0;
}
