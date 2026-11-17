#include <nlohmann/json.hpp>

#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    if (size > 256U * 1024U) return 0;
    static constexpr char empty[] = "";
    const auto* bytes = size == 0 ? empty : reinterpret_cast<const char*>(data);
    const std::string_view input(bytes, size);
    (void)nlohmann::json::parse(input.begin(), input.end(), nullptr, false, true);
    return 0;
}
