#include "basalt/persistence/history.hpp"

#include "storage_io.hpp"

#include <array>
#include <bit>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>

namespace basalt::persistence {
namespace {

constexpr std::size_t max_dataset_bytes = 256U * 1024U * 1024U;
constexpr std::string_view csv_header = "open_time_ns,open_micro,high_micro,low_micro,close_micro,volume\n";

template <typename T>
bool parse_integer(std::string_view text, T& value) {
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

common::Result<std::vector<HistoricalBar>> parse_csv(const std::string& bytes) {
    std::istringstream input(bytes);
    std::string line;
    if (!std::getline(input, line)) {
        return common::Result<std::vector<HistoricalBar>>::failure({"DATASET_HEADER", "CSV header must use the BASALT integer OHLCV schema"});
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != csv_header.substr(0, csv_header.size() - 1)) {
        return common::Result<std::vector<HistoricalBar>>::failure({"DATASET_HEADER", "CSV header must use the BASALT integer OHLCV schema"});
    }
    std::vector<HistoricalBar> bars;
    std::int64_t previous_time = std::numeric_limits<std::int64_t>::min();
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) return common::Result<std::vector<HistoricalBar>>::failure({"DATASET_ROW", "Blank OHLCV rows are not allowed"});
        std::array<std::string_view, 6> columns{};
        std::size_t begin = 0;
        for (std::size_t column = 0; column < columns.size(); ++column) {
            const auto comma = line.find(',', begin);
            if (column + 1U == columns.size()) {
                if (comma != std::string::npos) return common::Result<std::vector<HistoricalBar>>::failure({"DATASET_ROW", "OHLCV row has extra columns"});
                columns[column] = std::string_view(line).substr(begin);
            } else {
                if (comma == std::string::npos) return common::Result<std::vector<HistoricalBar>>::failure({"DATASET_ROW", "OHLCV row is missing columns"});
                columns[column] = std::string_view(line).substr(begin, comma - begin);
                begin = comma + 1U;
            }
        }
        HistoricalBar bar;
        if (!parse_integer(columns[0], bar.open_time_ns) || !parse_integer(columns[1], bar.open_micro) ||
            !parse_integer(columns[2], bar.high_micro) || !parse_integer(columns[3], bar.low_micro) ||
            !parse_integer(columns[4], bar.close_micro) || !parse_integer(columns[5], bar.volume)) {
            return common::Result<std::vector<HistoricalBar>>::failure({"DATASET_NUMBER", "OHLCV values must be decimal integers"});
        }
        if (bar.open_time_ns <= previous_time || bar.low_micro <= 0 || bar.open_micro <= 0 ||
            bar.high_micro < bar.open_micro || bar.high_micro < bar.close_micro ||
            bar.low_micro > bar.open_micro || bar.low_micro > bar.close_micro || bar.low_micro > bar.high_micro) {
            return common::Result<std::vector<HistoricalBar>>::failure({"DATASET_OHLC", "OHLC values must be positive, valid and chronologically ordered"});
        }
        previous_time = bar.open_time_ns;
        bars.push_back(bar);
    }
    if (bars.empty()) return common::Result<std::vector<HistoricalBar>>::failure({"DATASET_EMPTY", "Dataset must contain at least one bar"});
    return common::Result<std::vector<HistoricalBar>>::success(std::move(bars));
}

std::string encode_csv(const std::vector<HistoricalBar>& bars) {
    std::string output(csv_header);
    for (const auto& bar : bars) {
        output += std::to_string(bar.open_time_ns) + "," + std::to_string(bar.open_micro) + "," +
                  std::to_string(bar.high_micro) + "," + std::to_string(bar.low_micro) + "," +
                  std::to_string(bar.close_micro) + "," + std::to_string(bar.volume) + "\n";
    }
    return output;
}

bool safe_id(std::string_view id) {
    if (id.empty() || id.size() > 96U) return false;
    for (const char ch : id) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.')) return false;
    }
    return id != "." && id != "..";
}

} // namespace

HistoryStore::HistoryStore(std::filesystem::path root, Catalog& catalog)
    : root_(std::move(root)), catalog_(catalog) {}

common::Result<DatasetRecord> HistoryStore::import_csv(const std::filesystem::path& input_path,
                                                        const DatasetImport& metadata) {
    if (!safe_id(metadata.dataset_id) || metadata.instrument_id == 0 || metadata.interval_ns == 0 ||
        metadata.source.empty() || metadata.license_tag.empty()) {
        return common::Result<DatasetRecord>::failure({"DATASET_METADATA", "Dataset ID, instrument, interval, source and license are required"});
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(input_path, error);
    if (error || size > max_dataset_bytes) return common::Result<DatasetRecord>::failure({"DATASET_SIZE", "Input dataset is unavailable or exceeds the 256 MiB import limit"});
    std::ifstream input(input_path, std::ios::binary);
    if (!input) return common::Result<DatasetRecord>::failure({"DATASET_OPEN", "Could not open input CSV"});
    std::string raw(static_cast<std::size_t>(size), '\0');
    input.read(raw.data(), static_cast<std::streamsize>(raw.size()));
    if (!input && !input.eof()) return common::Result<DatasetRecord>::failure({"DATASET_READ", "Could not read input CSV"});
    const auto parsed = parse_csv(raw);
    if (!parsed) return common::Result<DatasetRecord>::failure(parsed.error());
    const auto normalized = encode_csv(parsed.value());
    const auto filename = metadata.dataset_id + ".csv";
    const auto written = detail::atomic_replace(root_ / filename, normalized);
    if (!written) return common::Result<DatasetRecord>::failure(written.error());
    DatasetRecord record;
    record.dataset_id = metadata.dataset_id;
    record.instrument_id = metadata.instrument_id;
    record.format = "csv-int64-ohlcv-v1";
    record.source = metadata.source;
    record.path = filename;
    record.sha256 = sha256(normalized);
    record.range_start_ns = parsed.value().front().open_time_ns;
    record.range_end_ns = parsed.value().back().open_time_ns;
    record.imported_at_ns = metadata.imported_at_ns;
    record.license_tag = metadata.license_tag;
    record.interval_ns = metadata.interval_ns;
    const auto registered = catalog_.upsert_dataset(record);
    if (!registered) return common::Result<DatasetRecord>::failure(registered.error());
    return common::Result<DatasetRecord>::success(std::move(record));
}

common::Result<std::vector<HistoricalBar>> HistoryStore::read_bars(const std::string_view dataset_id) const {
    const auto lookup = catalog_.dataset(dataset_id);
    if (!lookup) return common::Result<std::vector<HistoricalBar>>::failure(lookup.error());
    if (!lookup.value()) return common::Result<std::vector<HistoricalBar>>::failure({"DATASET_NOT_FOUND", "Dataset ID is not registered"});
    const auto& record = *lookup.value();
    const std::filesystem::path relative(record.path);
    if (relative.has_parent_path() || relative.filename() != relative)
        return common::Result<std::vector<HistoricalBar>>::failure({"DATASET_PATH", "Catalog dataset path must be a local filename"});
    std::error_code error;
    const auto size = std::filesystem::file_size(root_ / relative, error);
    if (error || size > max_dataset_bytes) return common::Result<std::vector<HistoricalBar>>::failure({"DATASET_SIZE", "Stored dataset is unavailable or oversized"});
    std::ifstream input(root_ / relative, std::ios::binary);
    if (!input) return common::Result<std::vector<HistoricalBar>>::failure({"DATASET_OPEN", "Could not open stored dataset"});
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input && !input.eof()) return common::Result<std::vector<HistoricalBar>>::failure({"DATASET_READ", "Could not read stored dataset"});
    if (sha256(bytes) != record.sha256) return common::Result<std::vector<HistoricalBar>>::failure({"DATASET_HASH", "Stored dataset SHA-256 does not match catalog"});
    return parse_csv(bytes);
}

common::Result<void> HistoryStore::export_csv(const std::string_view dataset_id,
                                               const std::filesystem::path& output) const {
    const auto bars = read_bars(dataset_id);
    if (!bars) return common::Result<void>::failure(bars.error());
    return detail::atomic_replace(output, encode_csv(bars.value()));
}

std::string HistoryStore::sha256(const std::string_view bytes) {
    constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
        0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
        0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
        0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
        0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
        0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
        0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
        0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};
    std::array<std::uint32_t, 8> hash{0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,
                                      0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U};
    std::vector<std::uint8_t> padded;
    padded.reserve(((bytes.size() + 9U + 63U) / 64U) * 64U);
    for (const auto ch : bytes) padded.push_back(static_cast<std::uint8_t>(ch));
    const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8U;
    padded.push_back(0x80U);
    while ((padded.size() % 64U) != 56U) padded.push_back(0U);
    for (int shift = 56; shift >= 0; shift -= 8) padded.push_back(static_cast<std::uint8_t>(bit_length >> shift));
    for (std::size_t offset = 0; offset < padded.size(); offset += 64U) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16U; ++i) {
            const auto at = offset + i * 4U;
            words[i] = (static_cast<std::uint32_t>(padded[at]) << 24U) |
                       (static_cast<std::uint32_t>(padded[at + 1U]) << 16U) |
                       (static_cast<std::uint32_t>(padded[at + 2U]) << 8U) | padded[at + 3U];
        }
        for (std::size_t i = 16U; i < 64U; ++i) {
            const auto s0 = std::rotr(words[i - 15U], 7) ^ std::rotr(words[i - 15U], 18) ^ (words[i - 15U] >> 3U);
            const auto s1 = std::rotr(words[i - 2U], 17) ^ std::rotr(words[i - 2U], 19) ^ (words[i - 2U] >> 10U);
            words[i] = words[i - 16U] + s0 + words[i - 7U] + s1;
        }
        auto a=hash[0], b=hash[1], c=hash[2], d=hash[3], e=hash[4], f=hash[5], g=hash[6], h=hash[7];
        for (std::size_t i = 0; i < 64U; ++i) {
            const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto temp1 = h + sum1 + choose + constants[i] + words[i];
            const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = sum0 + majority;
            h=g; g=f; f=e; e=d+temp1; d=c; c=b; b=a; a=temp1+temp2;
        }
        hash[0]+=a; hash[1]+=b; hash[2]+=c; hash[3]+=d;
        hash[4]+=e; hash[5]+=f; hash[6]+=g; hash[7]+=h;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto word : hash) output << std::setw(8) << word;
    return output.str();
}

} // namespace basalt::persistence
