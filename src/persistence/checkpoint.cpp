#include "basalt/persistence/checkpoint.hpp"
#include "basalt/persistence/event_log.hpp"

#include "storage_io.hpp"

#include <fstream>
#include <limits>
#include <type_traits>

namespace basalt::persistence {
namespace {

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
    for (std::size_t i = 0; i < sizeof(T); ++i)
        bits |= static_cast<U>(static_cast<unsigned char>(bytes[offset + i])) << (8U * i);
    return static_cast<T>(bits);
}

} // namespace

common::Result<void> CheckpointStore::save(const Checkpoint& checkpoint) const {
    if (checkpoint.state_hash.empty() || checkpoint.state_hash.size() > 256U ||
        checkpoint.state_blob.size() > 128U * 1024U * 1024U) {
        return common::Result<void>::failure({"CHECKPOINT_SIZE", "Checkpoint hash or state blob is outside the supported bounds"});
    }
    std::string bytes{"BCP1", 4};
    write_le(bytes, checkpoint.event_sequence);
    write_le(bytes, static_cast<std::uint16_t>(checkpoint.state_hash.size()));
    write_le(bytes, static_cast<std::uint64_t>(checkpoint.state_blob.size()));
    std::string checksum_input = bytes.substr(4);
    checksum_input += checkpoint.state_hash;
    checksum_input += checkpoint.state_blob;
    write_le(bytes, EventLog::crc32(reinterpret_cast<const std::byte*>(checksum_input.data()), checksum_input.size()));
    bytes += checkpoint.state_hash;
    bytes += checkpoint.state_blob;
    return detail::atomic_replace(path_, bytes);
}

common::Result<Checkpoint> CheckpointStore::load() const {
    std::ifstream input(path_, std::ios::binary);
    if (!input) return common::Result<Checkpoint>::failure({"CHECKPOINT_OPEN", "Checkpoint file is unavailable"});
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 26 || size > static_cast<std::streamoff>(128U * 1024U * 1024U + 282U))
        return common::Result<Checkpoint>::failure({"CHECKPOINT_SIZE", "Checkpoint file has an invalid size"});
    input.seekg(0);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), size);
    if (!input || bytes.compare(0, 4, "BCP1") != 0)
        return common::Result<Checkpoint>::failure({"CHECKPOINT_FORMAT", "Checkpoint header is invalid"});
    const auto sequence = read_le<std::uint64_t>(bytes, 4);
    const auto hash_size = read_le<std::uint16_t>(bytes, 12);
    const auto blob_size = read_le<std::uint64_t>(bytes, 14);
    const auto expected_crc = read_le<std::uint32_t>(bytes, 22);
    if (hash_size == 0 || hash_size > 256U || blob_size > 128U * 1024U * 1024U ||
        26U + static_cast<std::uint64_t>(hash_size) + blob_size != bytes.size())
        return common::Result<Checkpoint>::failure({"CHECKPOINT_SIZE", "Checkpoint lengths are invalid"});
    Checkpoint checkpoint;
    checkpoint.event_sequence = sequence;
    checkpoint.state_hash = bytes.substr(26, hash_size);
    checkpoint.state_blob = bytes.substr(26U + hash_size, static_cast<std::size_t>(blob_size));
    std::string checksum_input = bytes.substr(4, 18);
    checksum_input += checkpoint.state_hash;
    checksum_input += checkpoint.state_blob;
    const auto actual_crc = EventLog::crc32(reinterpret_cast<const std::byte*>(checksum_input.data()), checksum_input.size());
    if (actual_crc != expected_crc)
        return common::Result<Checkpoint>::failure({"CHECKPOINT_CRC", "Checkpoint checksum does not match"});
    return common::Result<Checkpoint>::success(std::move(checkpoint));
}

} // namespace basalt::persistence
