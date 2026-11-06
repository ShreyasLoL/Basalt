#pragma once

#include "basalt/common/types.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace basalt::persistence {

struct DatasetRecord {
    std::string dataset_id{};
    std::optional<common::InstrumentId> instrument_id{};
    std::string format{"csv"};
    std::string source{};
    std::string path{};
    std::string sha256{};
    std::optional<std::int64_t> range_start_ns{};
    std::optional<std::int64_t> range_end_ns{};
    std::int64_t imported_at_ns{};
    std::string license_tag{"unspecified"};
    std::uint64_t interval_ns{};
    friend bool operator==(const DatasetRecord&, const DatasetRecord&) = default;
};

class Catalog {
public:
    explicit Catalog(std::filesystem::path path) : path_(std::move(path)) {}
    [[nodiscard]] common::Result<void> open();
    [[nodiscard]] common::Result<void> upsert_dataset(const DatasetRecord& record);
    [[nodiscard]] common::Result<std::optional<DatasetRecord>> dataset(std::string_view id) const;
    [[nodiscard]] common::Result<std::vector<DatasetRecord>> datasets() const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    [[nodiscard]] common::Result<void> write_records(const std::vector<DatasetRecord>& records) const;
    std::filesystem::path path_;
    bool opened_{};
};

} // namespace basalt::persistence
