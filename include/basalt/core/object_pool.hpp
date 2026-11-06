#pragma once

#include "basalt/common/types.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace basalt {

struct PoolHandle {
    std::size_t index{};
    std::uint64_t generation{};
    friend bool operator==(const PoolHandle&, const PoolHandle&) = default;
};

template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(const std::size_t capacity) : slots_(capacity) {
        for (std::size_t i = 0; i < slots_.size(); ++i)
            slots_[i].next_free = i + 1 < slots_.size() ? i + 1 : invalid_index;
        if (slots_.empty()) free_head_ = invalid_index;
    }

    [[nodiscard]] common::Result<PoolHandle> acquire(T value = {}) {
        if (free_head_ == invalid_index)
            return common::Result<PoolHandle>::failure({"POOL_EXHAUSTED", "Fixed-capacity object pool is exhausted"});
        const auto index = free_head_;
        auto& slot = slots_[index];
        free_head_ = slot.next_free;
        slot.value = std::move(value);
        slot.active = true;
        ++active_;
        ++acquisitions_;
        high_water_ = std::max(high_water_, active_);
        return common::Result<PoolHandle>::success({index, slot.generation});
    }

    [[nodiscard]] T* get(const PoolHandle handle) noexcept {
        if (handle.index >= slots_.size()) return nullptr;
        auto& slot = slots_[handle.index];
        return slot.active && slot.generation == handle.generation ? &slot.value : nullptr;
    }

    [[nodiscard]] const T* get(const PoolHandle handle) const noexcept {
        if (handle.index >= slots_.size()) return nullptr;
        const auto& slot = slots_[handle.index];
        return slot.active && slot.generation == handle.generation ? &slot.value : nullptr;
    }

    [[nodiscard]] bool release(const PoolHandle handle) noexcept {
        if (get(handle) == nullptr) return false;
        auto& slot = slots_[handle.index];
        slot.value = T{};
        slot.active = false;
        slot.generation = slot.generation == std::numeric_limits<std::uint64_t>::max()
            ? 1 : slot.generation + 1;
        slot.next_free = free_head_;
        free_head_ = handle.index;
        --active_;
        return true;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] std::size_t active_count() const noexcept { return active_; }
    [[nodiscard]] std::size_t available() const noexcept { return capacity() - active_; }
    [[nodiscard]] std::uint64_t acquisitions() const noexcept { return acquisitions_; }
    [[nodiscard]] std::size_t high_water() const noexcept { return high_water_; }

private:
    static constexpr std::size_t invalid_index = std::numeric_limits<std::size_t>::max();
    struct Slot { T value{}; std::size_t next_free{invalid_index}; std::uint64_t generation{1}; bool active{}; };
    std::vector<Slot> slots_{};
    std::size_t free_head_{};
    std::size_t active_{};
    std::uint64_t acquisitions_{};
    std::size_t high_water_{};
};

} // namespace basalt
