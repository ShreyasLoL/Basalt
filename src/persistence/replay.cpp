#include "basalt/persistence/replay.hpp"

#include "basalt/persistence/history.hpp"

#include <type_traits>

namespace basalt::persistence {
namespace {

template <typename T>
void append_le(std::string& bytes, T value) {
    using U = std::make_unsigned_t<T>;
    auto bits = static_cast<U>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        bytes.push_back(static_cast<char>(bits & static_cast<U>(0xff)));
        bits >>= 8;
    }
}

} // namespace

common::Result<ReplayResult> replay_from_checkpoint(const Checkpoint& checkpoint,
                                                    const std::vector<LogFrame>& frames,
                                                    const ReplayApply& apply,
                                                    const StateHasher& hash_state) {
    if (!apply || !hash_state)
        return common::Result<ReplayResult>::failure({"REPLAY_CALLBACK", "Replay apply and state-hash callbacks are required"});
    if (checkpoint.state_hash.empty() || hash_state(checkpoint.state_blob) != checkpoint.state_hash)
        return common::Result<ReplayResult>::failure({"REPLAY_CHECKPOINT_HASH", "Checkpoint state hash does not match its blob"});

    ReplayResult result;
    result.state_blob = checkpoint.state_blob;
    result.manifest.start_sequence = checkpoint.event_sequence;
    result.manifest.end_sequence = checkpoint.event_sequence;
    std::string event_bytes;
    common::EventSequence previous = 0;
    for (const auto& frame : frames) {
        if (frame.sequence <= previous)
            return common::Result<ReplayResult>::failure({"REPLAY_SEQUENCE", "Event log sequences must increase strictly"});
        previous = frame.sequence;
        if (frame.sequence <= checkpoint.event_sequence) continue;
        append_le(event_bytes, frame.sequence);
        append_le(event_bytes, static_cast<std::uint64_t>(frame.payload.size()));
        event_bytes += frame.payload;
        const auto applied = apply(result.state_blob, frame);
        if (!applied) return common::Result<ReplayResult>::failure(applied.error());
        ++result.manifest.replayed_events;
        result.manifest.end_sequence = frame.sequence;
    }
    result.manifest.event_sha256 = HistoryStore::sha256(event_bytes);
    result.manifest.state_sha256 = hash_state(result.state_blob);
    return common::Result<ReplayResult>::success(std::move(result));
}

} // namespace basalt::persistence
