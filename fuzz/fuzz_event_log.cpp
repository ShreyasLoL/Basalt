#include "basalt/persistence/event_log.hpp"

#include <filesystem>
#include <fstream>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    if (size > 64U * 1024U) return 0;
    std::error_code error;
    const auto root = std::filesystem::temp_directory_path() / "basalt-phase4-fuzz-event-log";
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(root, error);
    if (error) return 0;

    const auto path = root / "input.blog";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) return 0;
        if (size > 0) output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!output) return 0;
    }
    {
        basalt::persistence::EventLog log(path, 64U * 1024U);
        const auto recovery = log.recover();
        if (recovery) (void)log.read_all();
        (void)log.close();
    }
    std::filesystem::remove_all(root, error);
    return 0;
}
