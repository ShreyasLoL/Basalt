#pragma once

#include "basalt/common/types.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace basalt::persistence {

struct Checkpoint {
    common::EventSequence event_sequence{};
    std::string state_hash{};
    std::string state_blob{};
    friend bool operator==(const Checkpoint&, const Checkpoint&) = default;
};

class CheckpointStore {
public:
    explicit CheckpointStore(std::filesystem::path path) : path_(std::move(path)) {}
    [[nodiscard]] common::Result<void> save(const Checkpoint& checkpoint) const;
    [[nodiscard]] common::Result<Checkpoint> load() const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace basalt::persistence
