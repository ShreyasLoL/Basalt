#pragma once

#include "basalt/common/types.hpp"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace basalt::persistence {

struct LogFrame {
    common::EventSequence sequence{};
    std::string payload{};
    friend bool operator==(const LogFrame&, const LogFrame&) = default;
};

struct RecoveryReport {
    std::uint64_t valid_frames{};
    common::EventSequence last_sequence{};
    std::uint64_t valid_bytes{};
    std::uint64_t discarded_bytes{};
    std::filesystem::path quarantined_tail{};
};

class EventLog {
public:
    explicit EventLog(std::filesystem::path path, std::uint32_t max_payload_bytes = 16U * 1024U * 1024U);

    // Recovery accepts the valid prefix, quarantines a damaged suffix, then truncates to
    // the last complete frame. Calling recover again is safe and idempotent.
    [[nodiscard]] common::Result<RecoveryReport> recover();
    [[nodiscard]] common::Result<void> append(common::EventSequence sequence,
                                               const std::string& payload);
    [[nodiscard]] common::Result<common::EventSequence> append_next(const std::string& payload);
    [[nodiscard]] common::Result<std::vector<LogFrame>> read_all() const;
    [[nodiscard]] common::EventSequence last_sequence() const noexcept { return last_sequence_.load(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    [[nodiscard]] static std::uint32_t crc32(const std::byte* data, std::size_t size) noexcept;

private:
    std::filesystem::path path_;
    std::uint32_t max_payload_bytes_;
    std::atomic<common::EventSequence> last_sequence_{};
    bool recovered_{};
    mutable std::mutex mutex_{};
    [[nodiscard]] common::Result<void> append_locked(common::EventSequence sequence,
                                                      const std::string& payload);
};

} // namespace basalt::persistence
