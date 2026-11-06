#pragma once

#include "basalt/persistence/catalog.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace basalt::persistence {

struct HistoricalBar {
    std::int64_t open_time_ns{};
    std::int64_t open_micro{};
    std::int64_t high_micro{};
    std::int64_t low_micro{};
    std::int64_t close_micro{};
    std::uint64_t volume{};
    friend bool operator==(const HistoricalBar&, const HistoricalBar&) = default;
};

struct DatasetImport {
    std::string dataset_id{};
    common::InstrumentId instrument_id{};
    std::uint64_t interval_ns{};
    std::string source{};
    std::int64_t imported_at_ns{};
    std::string license_tag{"unspecified"};
};

class HistoryStore {
public:
    HistoryStore(std::filesystem::path root, Catalog& catalog);
    [[nodiscard]] common::Result<DatasetRecord> import_csv(const std::filesystem::path& input,
                                                            const DatasetImport& metadata);
    [[nodiscard]] common::Result<std::vector<HistoricalBar>> read_bars(std::string_view dataset_id) const;
    [[nodiscard]] common::Result<void> export_csv(std::string_view dataset_id,
                                                   const std::filesystem::path& output) const;
    [[nodiscard]] static std::string sha256(std::string_view bytes);

private:
    std::filesystem::path root_;
    Catalog& catalog_;
};

} // namespace basalt::persistence
