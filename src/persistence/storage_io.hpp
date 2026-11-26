#pragma once

#include "basalt/common/types.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace basalt::persistence::detail {

// Internal, one-shot failpoints keep storage failure and recovery tests
// deterministic without depending on platform-specific permission behavior.
enum class StorageFailurePoint : std::uint8_t { None, Write, Flush, Resize, Rename, PartialEventAppend };

void set_storage_failure_for_testing(StorageFailurePoint point) noexcept;
void clear_storage_failure_for_testing() noexcept;
[[nodiscard]] bool consume_storage_failure_for_testing(StorageFailurePoint point) noexcept;

class ScopedStorageFailure {
public:
    explicit ScopedStorageFailure(const StorageFailurePoint point) noexcept {
        set_storage_failure_for_testing(point);
    }
    ~ScopedStorageFailure() { clear_storage_failure_for_testing(); }
    ScopedStorageFailure(const ScopedStorageFailure&) = delete;
    ScopedStorageFailure& operator=(const ScopedStorageFailure&) = delete;
};

[[nodiscard]] common::Result<void> durable_write(const std::filesystem::path& path,
                                                  const std::string& bytes);
[[nodiscard]] common::Result<void> flush_existing(const std::filesystem::path& path);
[[nodiscard]] common::Result<void> durable_resize(const std::filesystem::path& path,
                                                   std::uint64_t size);
[[nodiscard]] common::Result<void> atomic_replace(const std::filesystem::path& path,
                                                   const std::string& bytes);

} // namespace basalt::persistence::detail
