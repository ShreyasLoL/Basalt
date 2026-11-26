#include "storage_io.hpp"

#include <atomic>
#include <fstream>
#include <system_error>
#include <cstdint>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace basalt::persistence::detail {
namespace {

std::atomic<StorageFailurePoint> pending_failure{StorageFailurePoint::None};

common::Result<void> flush_path(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return common::Result<void>::failure({"STORAGE_FLUSH", "Could not open file for durable flush"});
    const auto result = FlushFileBuffers(handle);
    CloseHandle(handle);
#else
    const auto descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) return common::Result<void>::failure({"STORAGE_FLUSH", "Could not open file for durable flush"});
    const auto result = ::fsync(descriptor);
    ::close(descriptor);
#endif
    if (!result) return common::Result<void>::failure({"STORAGE_FLUSH", "Durable file flush failed"});
    return common::Result<void>::success();
}

} // namespace

void set_storage_failure_for_testing(const StorageFailurePoint point) noexcept {
    pending_failure.store(point, std::memory_order_release);
}

void clear_storage_failure_for_testing() noexcept {
    pending_failure.store(StorageFailurePoint::None, std::memory_order_release);
}

bool consume_storage_failure_for_testing(const StorageFailurePoint point) noexcept {
    auto expected = point;
    return pending_failure.compare_exchange_strong(expected, StorageFailurePoint::None,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed);
}

common::Result<void> durable_write(const std::filesystem::path& path, const std::string& bytes) {
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    if (error) return common::Result<void>::failure({"STORAGE_DIRECTORY", error.message()});
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return common::Result<void>::failure({"STORAGE_OPEN", "Could not open file for writing"});
    const auto inject_write_failure = consume_storage_failure_for_testing(StorageFailurePoint::Write);
    const auto bytes_to_write = inject_write_failure ? bytes.size() / 2U : bytes.size();
    output.write(bytes.data(), static_cast<std::streamsize>(bytes_to_write));
    output.flush();
    if (!output || inject_write_failure) {
        output.close();
        return common::Result<void>::failure({"STORAGE_WRITE", "File write failed"});
    }
    output.close();
    return flush_path(path);
}

common::Result<void> flush_existing(const std::filesystem::path& path) {
    if (consume_storage_failure_for_testing(StorageFailurePoint::Flush))
        return common::Result<void>::failure({"STORAGE_FLUSH", "Durable file flush failed"});
    return flush_path(path);
}

common::Result<void> durable_resize(const std::filesystem::path& path, const std::uint64_t size) {
    if (consume_storage_failure_for_testing(StorageFailurePoint::Resize))
        return common::Result<void>::failure({"STORAGE_RESIZE", "File resize failed"});
    std::error_code error;
    std::filesystem::resize_file(path, size, error);
    if (error) return common::Result<void>::failure({"STORAGE_RESIZE", error.message()});
    return flush_path(path);
}

common::Result<void> atomic_replace(const std::filesystem::path& path, const std::string& bytes) {
    auto temporary = path;
    temporary += ".tmp";
    const auto written = durable_write(temporary, bytes);
    if (!written) return written;
    if (consume_storage_failure_for_testing(StorageFailurePoint::Rename))
        return common::Result<void>::failure({"STORAGE_RENAME", "Atomic replacement failed"});
#if defined(_WIN32)
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto code = static_cast<int>(GetLastError());
        return common::Result<void>::failure({"STORAGE_RENAME", std::system_category().message(code)});
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) return common::Result<void>::failure({"STORAGE_RENAME", error.message()});
    if (!path.parent_path().empty()) {
        const auto descriptor = ::open(path.parent_path().c_str(), O_RDONLY);
        if (descriptor >= 0) { (void)::fsync(descriptor); ::close(descriptor); }
    }
#endif
    return common::Result<void>::success();
}

} // namespace basalt::persistence::detail
