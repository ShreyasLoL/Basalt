#include "basalt/persistence/catalog.hpp"

#include "storage_io.hpp"
#include "basalt/persistence/history.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace basalt::persistence {
namespace {

using Json = nlohmann::json;
constexpr int catalog_schema_version = 1;
constexpr std::string_view migration_name = "dataset-catalog-v1";
constexpr std::string_view migration_definition = "BASALT dataset catalog v1: instrument, format, source, path, SHA-256, range, import time, license and interval";

std::string migration_checksum() {
    return HistoryStore::sha256(migration_definition);
}

Json encode(const DatasetRecord& record) {
    Json json = Json::object();
    json["dataset_id"] = record.dataset_id;
    json["instrument_id"] = record.instrument_id ? Json(*record.instrument_id) : Json(nullptr);
    json["format"] = record.format;
    json["source"] = record.source;
    json["path"] = record.path;
    json["sha256"] = record.sha256;
    json["range_start_ns"] = record.range_start_ns ? Json(*record.range_start_ns) : Json(nullptr);
    json["range_end_ns"] = record.range_end_ns ? Json(*record.range_end_ns) : Json(nullptr);
    json["imported_at_ns"] = record.imported_at_ns;
    json["license_tag"] = record.license_tag;
    json["interval_ns"] = record.interval_ns;
    return json;
}

DatasetRecord decode(const Json& json) {
    DatasetRecord record;
    record.dataset_id = json.at("dataset_id").get<std::string>();
    if (json.contains("instrument_id") && !json.at("instrument_id").is_null())
        record.instrument_id = json.at("instrument_id").get<common::InstrumentId>();
    record.format = json.value("format", std::string{"csv"});
    record.source = json.value("source", std::string{});
    record.path = json.value("path", std::string{});
    record.sha256 = json.value("sha256", std::string{});
    if (json.contains("range_start_ns") && !json.at("range_start_ns").is_null())
        record.range_start_ns = json.at("range_start_ns").get<std::int64_t>();
    if (json.contains("range_end_ns") && !json.at("range_end_ns").is_null())
        record.range_end_ns = json.at("range_end_ns").get<std::int64_t>();
    record.imported_at_ns = json.value("imported_at_ns", std::int64_t{});
    record.license_tag = json.value("license_tag", std::string{"unspecified"});
    record.interval_ns = json.value("interval_ns", std::uint64_t{});
    return record;
}

struct CatalogDocument {
    int version{};
    bool has_migration{};
    std::vector<DatasetRecord> records{};
};

common::Result<CatalogDocument> read_document(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return common::Result<CatalogDocument>::failure({"CATALOG_OPEN", "Catalog file is unavailable"});
    try {
        const auto json = Json::parse(input);
        CatalogDocument document;
        document.version = json.value("schema_version", 0);
        if (document.version < 0 || document.version > catalog_schema_version)
            return common::Result<CatalogDocument>::failure({"CATALOG_SCHEMA", "Catalog schema version is unsupported"});
        const auto datasets = json.value("datasets", Json::array());
        if (!datasets.is_array())
            return common::Result<CatalogDocument>::failure({"CATALOG_SCHEMA", "Catalog datasets field must be an array"});
        for (const auto& item : datasets) document.records.push_back(decode(item));
        if (document.version == catalog_schema_version && json.contains("migrations")) {
            const auto& migrations = json.at("migrations");
            if (!migrations.is_array())
                return common::Result<CatalogDocument>::failure({"CATALOG_MIGRATION", "Catalog migration history is invalid"});
            for (const auto& migration : migrations) {
                if (migration.value("version", 0) == catalog_schema_version) {
                    if (migration.value("name", std::string{}) != migration_name ||
                        migration.value("checksum", std::string{}) != migration_checksum())
                        return common::Result<CatalogDocument>::failure({"CATALOG_MIGRATION", "Catalog migration checksum is invalid"});
                    document.has_migration = true;
                }
            }
        }
        return common::Result<CatalogDocument>::success(std::move(document));
    } catch (const std::exception&) {
        return common::Result<CatalogDocument>::failure({"CATALOG_CORRUPT", "Catalog JSON is invalid or incomplete"});
    }
}

common::Result<std::vector<DatasetRecord>> read_records(const std::filesystem::path& path) {
    const auto document = read_document(path);
    if (!document) return common::Result<std::vector<DatasetRecord>>::failure(document.error());
    if (document.value().version != catalog_schema_version || !document.value().has_migration)
        return common::Result<std::vector<DatasetRecord>>::failure({"CATALOG_MIGRATION", "Catalog must be opened to apply its migration before access"});
    return common::Result<std::vector<DatasetRecord>>::success(document.value().records);
}

} // namespace

common::Result<void> Catalog::open() {
    std::error_code error;
    const bool exists = std::filesystem::exists(path_, error);
    if (error) return common::Result<void>::failure({"CATALOG_STAT", error.message()});
    if (!exists) {
        const auto created = write_records({});
        if (!created) return created;
    } else {
        const auto document = read_document(path_);
        if (!document) return common::Result<void>::failure(document.error());
        if (document.value().version != catalog_schema_version || !document.value().has_migration) {
            const auto migrated = write_records(document.value().records);
            if (!migrated) return migrated;
        }
    }
    opened_ = true;
    return common::Result<void>::success();
}

common::Result<void> Catalog::write_records(const std::vector<DatasetRecord>& input) const {
    auto records = input;
    std::sort(records.begin(), records.end(), [](const DatasetRecord& left, const DatasetRecord& right) {
        return left.dataset_id < right.dataset_id;
    });
    Json datasets = Json::array();
    for (const auto& record : records) datasets.push_back(encode(record));
    Json catalog = Json::object();
    catalog["schema_version"] = catalog_schema_version;
    catalog["migrations"] = Json::array({Json{{"version", catalog_schema_version},
                                               {"name", migration_name},
                                               {"checksum", migration_checksum()}}});
    catalog["datasets"] = std::move(datasets);
    return detail::atomic_replace(path_, catalog.dump(2) + "\n");
}

common::Result<void> Catalog::upsert_dataset(const DatasetRecord& record) {
    if (!opened_) return common::Result<void>::failure({"CATALOG_NOT_OPEN", "Open the catalog before writing"});
    if (record.dataset_id.empty() || record.path.empty() || record.license_tag.empty())
        return common::Result<void>::failure({"CATALOG_RECORD", "Dataset ID, path and license tag are required"});
    const auto old = read_records(path_);
    if (!old) return common::Result<void>::failure(old.error());
    auto records = old.value();
    const auto found = std::find_if(records.begin(), records.end(), [&](const auto& value) {
        return value.dataset_id == record.dataset_id;
    });
    if (found == records.end()) records.push_back(record);
    else *found = record;
    return write_records(records);
}

common::Result<std::optional<DatasetRecord>> Catalog::dataset(const std::string_view id) const {
    if (!opened_) return common::Result<std::optional<DatasetRecord>>::failure({"CATALOG_NOT_OPEN", "Open the catalog before reading"});
    const auto records = read_records(path_);
    if (!records) return common::Result<std::optional<DatasetRecord>>::failure(records.error());
    const auto found = std::find_if(records.value().begin(), records.value().end(), [&](const auto& value) {
        return value.dataset_id == id;
    });
    if (found == records.value().end()) return common::Result<std::optional<DatasetRecord>>::success(std::nullopt);
    return common::Result<std::optional<DatasetRecord>>::success(*found);
}

common::Result<std::vector<DatasetRecord>> Catalog::datasets() const {
    if (!opened_) return common::Result<std::vector<DatasetRecord>>::failure({"CATALOG_NOT_OPEN", "Open the catalog before reading"});
    return read_records(path_);
}

} // namespace basalt::persistence
