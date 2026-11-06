#include "basalt/backtest/backtest.hpp"

#include <algorithm>
#include <set>

namespace basalt::backtest {

ReplayController::ReplayController(std::vector<BacktestEvent> events) : events_(std::move(events)) {}

common::Result<void> ReplayController::validate() const {
    std::set<common::EventSequence> sequences;
    std::set<std::string, std::less<>> event_ids;
    std::optional<std::pair<std::int64_t, common::EventSequence>> previous;
    common::EventSequence previous_sequence = 0;
    for (const auto& record : events_) {
        if (record.sequence == 0 || record.event.event_id.empty() || record.event.event_time_ns <= 0)
            return common::Result<void>::failure({"REPLAY_EVENT_INVALID", "Replay event needs a stable ID, sequence and time"});
        if (!sequences.insert(record.sequence).second || !event_ids.insert(record.event.event_id).second)
            return common::Result<void>::failure({"REPLAY_DUPLICATE", "Replay sequence and event IDs must be unique"});
        if (record.sequence <= previous_sequence)
            return common::Result<void>::failure({"REPLAY_SEQUENCE", "Replay event sequences must increase strictly"});
        const auto order = std::pair{record.event.event_time_ns, record.sequence};
        if (previous && order < *previous)
            return common::Result<void>::failure({"REPLAY_ORDER", "Replay events must be chronological and sequence-stable"});
        previous = order;
        previous_sequence = record.sequence;
    }
    return common::Result<void>::success();
}

std::optional<BacktestEvent> ReplayController::next() {
    if (!playing_ || cursor_ >= events_.size()) return std::nullopt;
    auto event = events_[cursor_++];
    current_sequence_ = event.sequence;
    virtual_time_ns_ = event.event.event_time_ns;
    return event;
}

common::Result<BacktestEvent> ReplayController::step_one() {
    if (playing_) return common::Result<BacktestEvent>::failure(
        {"REPLAY_MUST_BE_PAUSED", "StepOne is available only while replay is paused"});
    if (cursor_ >= events_.size()) return common::Result<BacktestEvent>::failure(
        {"REPLAY_AT_END", "There is no next replay event"});
    auto event = events_[cursor_++];
    current_sequence_ = event.sequence;
    virtual_time_ns_ = event.event.event_time_ns;
    return common::Result<BacktestEvent>::success(std::move(event));
}

common::Result<void> ReplayController::seek(const common::EventSequence target,
                                            const std::vector<ReplayCheckpoint>& checkpoints,
                                            const Restore& restore, const Apply& apply) {
    if (!restore || !apply || checkpoints.empty()) return common::Result<void>::failure(
        {"REPLAY_SEEK_CONFIG", "Seek needs a checkpoint and restore/apply handlers"});
    const ReplayCheckpoint* selected = nullptr;
    for (const auto& checkpoint : checkpoints) {
        if (checkpoint.sequence <= target && checkpoint.sequence != 0 && checkpoint.time_ns > 0 &&
            !checkpoint.state_hash.empty() && !checkpoint.state_blob.empty() &&
            (selected == nullptr || checkpoint.sequence > selected->sequence)) selected = &checkpoint;
    }
    if (selected == nullptr) return common::Result<void>::failure(
        {"REPLAY_CHECKPOINT_NOT_FOUND", "No verified checkpoint exists at or before the target sequence"});
    auto restored = restore(*selected);
    if (!restored) return restored;
    std::size_t next = 0;
    std::int64_t virtual_time = selected->time_ns;
    common::EventSequence last = selected->sequence;
    for (std::size_t i = 0; i < events_.size(); ++i) {
        const auto& event = events_[i];
        if (event.sequence <= selected->sequence) continue;
        if (event.sequence > target) {
            next = i;
            break;
        }
        auto applied = apply(event);
        if (!applied) return applied;
        last = event.sequence;
        virtual_time = event.event.event_time_ns;
        next = i + 1;
    }
    if (next == 0) {
        const auto found = std::upper_bound(events_.begin(), events_.end(), target,
            [](const common::EventSequence sequence, const BacktestEvent& event) { return sequence < event.sequence; });
        next = static_cast<std::size_t>(std::distance(events_.begin(), found));
    }
    cursor_ = next;
    current_sequence_ = last;
    virtual_time_ns_ = virtual_time;
    playing_ = false;
    return common::Result<void>::success();
}

std::string_view to_string(const DataSplit split) noexcept {
    switch (split) {
    case DataSplit::Train: return "TRAIN";
    case DataSplit::Validation: return "VALIDATION";
    case DataSplit::Test: return "TEST";
    }
    return "UNKNOWN";
}

std::string_view to_string(const ReplaySpeed speed) noexcept {
    switch (speed) {
    case ReplaySpeed::OneX: return "1x";
    case ReplaySpeed::FiveX: return "5x";
    case ReplaySpeed::TenX: return "10x";
    case ReplaySpeed::Max: return "MAX";
    }
    return "UNKNOWN";
}

common::Result<void> validate_splits(const DatasetSplits& splits) {
    const auto valid_range = [](const TimeRange& range) {
        return range.start_ns > 0 && range.end_ns > range.start_ns;
    };
    if (!valid_range(splits.train) || !valid_range(splits.validation) || !valid_range(splits.test))
        return common::Result<void>::failure({"SPLIT_RANGE_INVALID", "All split ranges must be nonempty half-open UTC intervals"});
    if (splits.train.end_ns > splits.validation.start_ns ||
        splits.validation.end_ns > splits.test.start_ns)
        return common::Result<void>::failure({"SPLIT_OVERLAP", "Train, validation and test ranges must be chronological and disjoint"});
    const auto train_gap = static_cast<std::uint64_t>(splits.validation.start_ns - splits.train.end_ns);
    const auto validation_gap = static_cast<std::uint64_t>(splits.test.start_ns - splits.validation.end_ns);
    if (train_gap < splits.embargo_ns || validation_gap < splits.embargo_ns)
        return common::Result<void>::failure({"SPLIT_EMBARGO", "Split boundary gaps are smaller than the configured embargo"});
    return common::Result<void>::success();
}

} // namespace basalt::backtest
