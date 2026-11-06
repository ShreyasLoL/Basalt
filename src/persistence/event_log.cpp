#include "basalt/persistence/event_log.hpp"

#include "storage_io.hpp"

#include <array>
#include <fstream>
#include <limits>
#include <system_error>
#include <type_traits>

namespace basalt::persistence {
namespace {

constexpr std::array<char, 4> magic{'B', 'L', 'G', '1'};
constexpr std::uint16_t version = 1;
constexpr std::size_t header_size = 22;

template <typename T>
void write_le(std::string& bytes, T value) {
    using U = std::make_unsigned_t<T>;
    auto bits = static_cast<U>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        bytes.push_back(static_cast<char>(bits & static_cast<U>(0xff)));
        bits >>= 8;
    }
}

template <typename T>
T read_le(const std::string& bytes, std::size_t offset) {
    using U = std::make_unsigned_t<T>;
    U bits{};
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        bits |= static_cast<U>(static_cast<unsigned char>(bytes[offset + i])) << (8U * i);
    }
    return static_cast<T>(bits);
}

bool read_exact(std::ifstream& input, char* data, std::size_t size) {
    input.read(data, static_cast<std::streamsize>(size));
    return static_cast<std::size_t>(input.gcount()) == size;
}

std::filesystem::path quarantine_name(const std::filesystem::path& path) {
    auto candidate = path;
    candidate += ".corrupt";
    for (std::uint32_t suffix = 1; std::filesystem::exists(candidate); ++suffix) {
        candidate = path;
        candidate += ".corrupt." + std::to_string(suffix);
    }
    return candidate;
}

common::Result<void> preserve_tail(const std::filesystem::path& source, std::uint64_t offset,
                                    const std::filesystem::path& destination) {
    std::ifstream input(source, std::ios::binary);
    if (!input) return common::Result<void>::failure({"LOG_READ", "Could not open event log tail"});
    input.seekg(static_cast<std::streamoff>(offset));
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output) return common::Result<void>::failure({"LOG_QUARANTINE", "Could not create corrupt-tail quarantine"});
    std::array<char, 64U * 1024U> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) output.write(buffer.data(), count);
    }
    output.flush();
    if (!output) return common::Result<void>::failure({"LOG_QUARANTINE", "Could not preserve corrupt event-log tail"});
    output.close();
    const auto flushed = detail::flush_existing(destination);
    if (!flushed) return flushed;
    return common::Result<void>::success();
}

} // namespace

EventLog::EventLog(std::filesystem::path path, const std::uint32_t max_payload_bytes)
    : path_(std::move(path)), max_payload_bytes_(max_payload_bytes) {}

std::uint32_t EventLog::crc32(const std::byte* data, const std::size_t size) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint8_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

common::Result<RecoveryReport> EventLog::recover() {
    RecoveryReport report;
    std::error_code error;
    if (!std::filesystem::exists(path_, error)) {
        if (error) return common::Result<RecoveryReport>::failure({"LOG_STAT", error.message()});
        const auto created = detail::durable_write(path_, {});
        if (!created) return common::Result<RecoveryReport>::failure(created.error());
        recovered_ = true;
        last_sequence_ = 0;
        return common::Result<RecoveryReport>::success(report);
    }
    const auto file_size = std::filesystem::file_size(path_, error);
    if (error) return common::Result<RecoveryReport>::failure({"LOG_STAT", error.message()});
    std::ifstream input(path_, std::ios::binary);
    if (!input) return common::Result<RecoveryReport>::failure({"LOG_OPEN", "Could not open event log"});
    std::uint64_t offset = 0;
    common::EventSequence previous = 0;
    std::string header(header_size, '\0');
    while (offset < file_size) {
        if (file_size - offset < header_size || !read_exact(input, header.data(), header_size)) break;
        bool valid_magic = true;
        for (std::size_t i = 0; i < magic.size(); ++i) valid_magic = valid_magic && header[i] == magic[i];
        const auto frame_version = read_le<std::uint16_t>(header, 4);
        const auto length = read_le<std::uint32_t>(header, 6);
        const auto sequence = read_le<std::uint64_t>(header, 10);
        const auto expected_crc = read_le<std::uint32_t>(header, 18);
        if (!valid_magic || frame_version != version || length > max_payload_bytes_ || sequence <= previous ||
            file_size - offset - header_size < length) break;
        std::string payload(length, '\0');
        if (length > 0 && !read_exact(input, payload.data(), length)) break;
        std::string crc_input = header.substr(4, 14);
        crc_input += payload;
        const auto actual_crc = crc32(reinterpret_cast<const std::byte*>(crc_input.data()), crc_input.size());
        if (actual_crc != expected_crc) break;
        ++report.valid_frames;
        previous = sequence;
        offset += header_size + length;
    }
    report.valid_bytes = offset;
    report.last_sequence = previous;
    report.discarded_bytes = file_size - offset;
    input.close();
    if (report.discarded_bytes > 0) {
        report.quarantined_tail = quarantine_name(path_);
        auto saved = preserve_tail(path_, offset, report.quarantined_tail);
        if (!saved) return common::Result<RecoveryReport>::failure(saved.error());
        const auto truncated = detail::durable_resize(path_, offset);
        if (!truncated) return common::Result<RecoveryReport>::failure(truncated.error());
    }
    last_sequence_ = previous;
    recovered_ = true;
    return common::Result<RecoveryReport>::success(report);
}

common::Result<void> EventLog::append(const common::EventSequence sequence, const std::string& payload) {
    if (!recovered_) return common::Result<void>::failure({"LOG_NOT_RECOVERED", "Recover the event log before appending"});
    if (payload.size() > max_payload_bytes_ || payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return common::Result<void>::failure({"LOG_FRAME_TOO_LARGE", "Event payload exceeds configured frame limit"});
    }
    if (sequence <= last_sequence_) return common::Result<void>::failure({"LOG_SEQUENCE", "Event sequence must increase strictly"});
    std::string frame;
    frame.reserve(header_size + payload.size());
    frame.append(magic.data(), magic.size());
    write_le(frame, version);
    write_le(frame, static_cast<std::uint32_t>(payload.size()));
    write_le(frame, sequence);
    std::string crc_input = frame.substr(4);
    crc_input += payload;
    const auto crc = crc32(reinterpret_cast<const std::byte*>(crc_input.data()), crc_input.size());
    write_le(frame, crc);
    frame += payload;
    std::error_code error;
    if (!path_.parent_path().empty()) std::filesystem::create_directories(path_.parent_path(), error);
    if (error) return common::Result<void>::failure({"LOG_DIRECTORY", error.message()});
    std::ofstream output(path_, std::ios::binary | std::ios::app);
    if (!output) return common::Result<void>::failure({"LOG_OPEN", "Could not open event log for append"});
    output.write(frame.data(), static_cast<std::streamsize>(frame.size()));
    output.flush();
    if (!output) return common::Result<void>::failure({"LOG_WRITE", "Event-log append failed"});
    output.close();
    const auto flushed = detail::flush_existing(path_);
    if (!flushed) return flushed;
    last_sequence_ = sequence;
    return common::Result<void>::success();
}

common::Result<std::vector<LogFrame>> EventLog::read_all() const {
    if (!recovered_) return common::Result<std::vector<LogFrame>>::failure({"LOG_NOT_RECOVERED", "Recover the event log before reading"});
    std::error_code error;
    const auto file_size = std::filesystem::file_size(path_, error);
    if (error) return common::Result<std::vector<LogFrame>>::failure({"LOG_STAT", error.message()});
    std::ifstream input(path_, std::ios::binary);
    if (!input) return common::Result<std::vector<LogFrame>>::failure({"LOG_OPEN", "Could not open event log"});
    std::vector<LogFrame> frames;
    std::string header(header_size, '\0');
    std::uint64_t offset = 0;
    common::EventSequence previous = 0;
    while (offset < file_size) {
        if (file_size - offset < header_size || !read_exact(input, header.data(), header_size))
            return common::Result<std::vector<LogFrame>>::failure({"LOG_CORRUPT", "Event-log frame header is truncated"});
        bool valid_magic = true;
        for (std::size_t i = 0; i < magic.size(); ++i) valid_magic = valid_magic && header[i] == magic[i];
        const auto frame_version = read_le<std::uint16_t>(header, 4);
        const auto length = read_le<std::uint32_t>(header, 6);
        const auto sequence = read_le<std::uint64_t>(header, 10);
        const auto expected_crc = read_le<std::uint32_t>(header, 18);
        if (!valid_magic || frame_version != version || length > max_payload_bytes_ || sequence <= previous ||
            file_size - offset - header_size < length)
            return common::Result<std::vector<LogFrame>>::failure({"LOG_CORRUPT", "Event-log frame metadata is invalid"});
        std::string payload(length, '\0');
        if (length > 0 && !read_exact(input, payload.data(), length))
            return common::Result<std::vector<LogFrame>>::failure({"LOG_CORRUPT", "Unexpected end of recovered event log"});
        std::string crc_input = header.substr(4, 14);
        crc_input += payload;
        const auto actual_crc = crc32(reinterpret_cast<const std::byte*>(crc_input.data()), crc_input.size());
        if (actual_crc != expected_crc)
            return common::Result<std::vector<LogFrame>>::failure({"LOG_CORRUPT", "Event-log frame checksum does not match"});
        frames.push_back({sequence, std::move(payload)});
        previous = sequence;
        offset += header_size + length;
    }
    if (input.bad()) return common::Result<std::vector<LogFrame>>::failure({"LOG_READ", "Event-log read failed"});
    return common::Result<std::vector<LogFrame>>::success(std::move(frames));
}

} // namespace basalt::persistence
