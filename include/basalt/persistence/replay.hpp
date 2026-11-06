#pragma once

#include "basalt/persistence/checkpoint.hpp"
#include "basalt/persistence/event_log.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::persistence {

struct ReplayManifest {
    common::EventSequence start_sequence{};
    common::EventSequence end_sequence{};
    std::uint64_t replayed_events{};
    std::string event_sha256{};
    std::string state_sha256{};
    friend bool operator==(const ReplayManifest&, const ReplayManifest&) = default;
};

struct ReplayResult {
    std::string state_blob{};
    ReplayManifest manifest{};
    friend bool operator==(const ReplayResult&, const ReplayResult&) = default;
};

using ReplayApply = std::function<common::Result<void>(std::string&, const LogFrame&)>;
using StateHasher = std::function<std::string(std::string_view)>;

// Replays only frames after the checkpoint. Both the snapshot hash and each
// resulting state hash are supplied by the caller's canonical state encoder.
[[nodiscard]] common::Result<ReplayResult> replay_from_checkpoint(
    const Checkpoint& checkpoint, const std::vector<LogFrame>& frames,
    const ReplayApply& apply, const StateHasher& hash_state);

} // namespace basalt::persistence
