#include "basalt/persistence/history.hpp"

#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    if (size > 256U * 1024U) return 0;
    static constexpr char empty[] = "";
    const auto* bytes = size == 0 ? empty : reinterpret_cast<const char*>(data);
    (void)basalt::persistence::HistoryStore::parse_csv(std::string_view(bytes, size));
    return 0;
}
