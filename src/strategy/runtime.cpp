#include "basalt/strategy/runtime.hpp"

#include <algorithm>
#include <exception>

namespace basalt::strategy {
namespace {

StrategyError illegal_transition(const LifecycleState from, const LifecycleState to) {
    return {"ILLEGAL_LIFECYCLE_TRANSITION",
            "Cannot transition strategy from " + std::string(to_string(from)) + " to " +
                std::string(to_string(to))};
}

} // namespace

StrategyRuntime::StrategyRuntime(std::unique_ptr<IStrategy> strategy, trading::RiskEngine& risk,
                                 FailureSink failure_sink)
    : strategy_(std::move(strategy)), risk_(risk), failure_sink_(std::move(failure_sink)) {}

common::Result<void, StrategyError> StrategyRuntime::transition(const LifecycleState next) {
    if (next == LifecycleState::Failed && state_ != LifecycleState::Stopped &&
        state_ != LifecycleState::Failed) {
        state_ = next;
        transitions_.push_back(next);
        return common::Result<void, StrategyError>::success();
    }
    bool allowed = false;
    switch (state_) {
    case LifecycleState::Created:
        allowed = next == LifecycleState::Configured || next == LifecycleState::Stopping;
        break;
    case LifecycleState::Configured: allowed = next == LifecycleState::Validated; break;
    case LifecycleState::Validated: allowed = next == LifecycleState::Initialising || next == LifecycleState::Stopping; break;
    case LifecycleState::Initialising:
        allowed = next == LifecycleState::WarmingUp || next == LifecycleState::Running || next == LifecycleState::Stopping;
        break;
    case LifecycleState::WarmingUp: allowed = next == LifecycleState::Running || next == LifecycleState::Stopping; break;
    case LifecycleState::Running:
        allowed = next == LifecycleState::Paused || next == LifecycleState::Stopping;
        break;
    case LifecycleState::Paused:
        allowed = next == LifecycleState::Running || next == LifecycleState::Stopping;
        break;
    case LifecycleState::Stopping: allowed = next == LifecycleState::Stopped; break;
    case LifecycleState::Stopped:
    case LifecycleState::Failed: allowed = false; break;
    }
    if (!allowed) return common::Result<void, StrategyError>::failure(illegal_transition(state_, next));
    state_ = next;
    transitions_.push_back(next);
    return common::Result<void, StrategyError>::success();
}

common::Result<void, StrategyError> StrategyRuntime::fail(StrategyError error) {
    if (!failure_) {
        failure_ = std::move(error);
        (void)transition(LifecycleState::Failed);
        if (failure_sink_) {
            try {
                failure_sink_(strategy_id_, *failure_);
            } catch (...) {
                // Failure reporting must not take down the domain dispatcher.
            }
        }
    }
    return common::Result<void, StrategyError>::failure(*failure_);
}

common::Result<void, StrategyError> StrategyRuntime::configure(const StrategyConfig& config) {
    if (state_ != LifecycleState::Created || !strategy_)
        return common::Result<void, StrategyError>::failure(
            {"STRATEGY_NOT_CONFIGURABLE", "Strategy can only be configured once from CREATED"});
    try {
        auto configured = strategy_->configure(config);
        if (!configured) return fail(configured.error());
    } catch (const std::exception& error) {
        return fail({"STRATEGY_EXCEPTION", error.what()});
    } catch (...) {
        return fail({"STRATEGY_EXCEPTION", "Unknown exception during configure"});
    }
    config_ = config;
    return transition(LifecycleState::Configured);
}

common::Result<void, StrategyError> StrategyRuntime::start(const StrategyContext& context) {
    if (state_ != LifecycleState::Configured || !strategy_ || !config_)
        return common::Result<void, StrategyError>::failure(
            {"STRATEGY_NOT_CONFIGURED", "Configure the strategy before starting it"});
    const auto descriptor = strategy_->descriptor();
    if (context.account_id == 0 || context.strategy_id == 0 || context.instrument.id == 0 ||
        !context.instrument.active || context.instrument.tick_size_micro <= 0 ||
        context.instrument.lot_size == 0 || config_->order_quantity == 0 ||
        (context.instrument.active_from_ns && context.now_ns < *context.instrument.active_from_ns) ||
        (context.instrument.active_to_ns && context.now_ns >= *context.instrument.active_to_ns))
        return fail({"INVALID_STRATEGY_CONTEXT", "Instrument, account, strategy, quantity or effective date is invalid"});
    strategy_id_ = context.strategy_id;
    if (descriptor.strategy_id.empty() || descriptor.version.empty() || descriptor.stable_hash.empty())
        return fail({"INVALID_STRATEGY_DESCRIPTOR", "Strategy descriptor requires stable ID, version and hash"});
    if (!descriptor.supported_instruments.empty() &&
        std::find(descriptor.supported_instruments.begin(), descriptor.supported_instruments.end(),
                  context.instrument.id) == descriptor.supported_instruments.end())
        return fail({"UNSUPPORTED_INSTRUMENT", "Strategy does not support this instrument"});
    auto step = transition(LifecycleState::Validated);
    if (!step) return step;
    step = transition(LifecycleState::Initialising);
    if (!step) return step;
    try {
        auto initialized = strategy_->initialise(context);
        if (!initialized) return fail(initialized.error());
    } catch (const std::exception& error) {
        return fail({"STRATEGY_EXCEPTION", error.what()});
    } catch (...) {
        return fail({"STRATEGY_EXCEPTION", "Unknown exception during initialise"});
    }
    return transition(descriptor.required_warmup_events == 0 ? LifecycleState::Running
                                                             : LifecycleState::WarmingUp);
}

bool StrategyRuntime::event_is_fresh(const market_data::CanonicalMarketEvent& event,
                                     const StrategyContext& context) const noexcept {
    if (event.quality != common::FeedQualityState::Ok || event.sequence_gap || event.clock_skew ||
        event.event_time_ns > context.now_ns || event.receive_time_utc_ns > context.now_ns)
        return false;
    const auto event_time = event.receive_time_utc_ns > 0 ? event.receive_time_utc_ns : event.event_time_ns;
    if (event_time <= 0 || context.now_ns < event_time) return false;
    const auto age = static_cast<std::uint64_t>(context.now_ns - event_time);
    return age <= context.max_data_age_ns;
}

common::Result<StrategyRunResult, StrategyError> StrategyRuntime::on_market_event(
    const market_data::CanonicalMarketEvent& event, const StrategyContext& context) {
    if (state_ == LifecycleState::Paused)
        return common::Result<StrategyRunResult, StrategyError>::success({{}, {}, state_});
    if (state_ != LifecycleState::WarmingUp && state_ != LifecycleState::Running)
        return common::Result<StrategyRunResult, StrategyError>::failure(
            {"STRATEGY_NOT_RUNNING", "Market events require WARMING_UP or RUNNING state"});
    if (!strategy_ || event.instrument_id != context.instrument.id || !event_is_fresh(event, context))
        return common::Result<StrategyRunResult, StrategyError>::success({{}, {}, state_});
    const auto descriptor = strategy_->descriptor();
    if (!descriptor.required_channels.empty() &&
        std::find(descriptor.required_channels.begin(), descriptor.required_channels.end(), event.kind) ==
            descriptor.required_channels.end())
        return common::Result<StrategyRunResult, StrategyError>::success({{}, {}, state_});

    auto effective_context = context;
    effective_context.order_quantity = config_->order_quantity;
    effective_context.sizing_budget_micro = config_->sizing_budget_micro;
    effective_context.fee_bps = config_->fee_bps;
    effective_context.slippage_bps = config_->slippage_bps;
    effective_context.risk_snapshot.position_quantity = context.position_quantity;
    effective_context.risk_snapshot.now_ns = context.now_ns;
    try {
        auto generated = strategy_->on_event(event, effective_context);
        if (!generated) {
            auto failed = fail(generated.error());
            return common::Result<StrategyRunResult, StrategyError>::failure(failed.error());
        }
        if (state_ == LifecycleState::WarmingUp) {
            ++warmup_events_seen_;
            if (warmup_events_seen_ >= descriptor.required_warmup_events) {
                auto moved = transition(LifecycleState::Running);
                if (!moved) return common::Result<StrategyRunResult, StrategyError>::failure(moved.error());
            }
            return common::Result<StrategyRunResult, StrategyError>::success({{}, {}, state_});
        }
        StrategyRunResult result{};
        for (auto& signal : generated.value()) {
            if (signal.intent.account_id != context.account_id ||
                signal.intent.strategy_id != context.strategy_id ||
                signal.intent.instrument_id != context.instrument.id || signal.signal_id.empty() ||
                signal.intent.client_order_id != signal.signal_id)
            {
                auto failed = fail({"INVALID_STRATEGY_SIGNAL", "Signal identity does not match its runtime context"});
                return common::Result<StrategyRunResult, StrategyError>::failure(failed.error());
            }
            const auto decision = risk_.evaluate(signal.intent, effective_context.risk_snapshot,
                                                  effective_context.risk_limits);
            if (decision.accepted) result.approved_signals.push_back(std::move(signal));
            else result.rejected_signals.push_back({std::move(signal), decision});
        }
        result.state = state_;
        return common::Result<StrategyRunResult, StrategyError>::success(std::move(result));
    } catch (const std::exception& error) {
        auto failed = fail({"STRATEGY_EXCEPTION", error.what()});
        return common::Result<StrategyRunResult, StrategyError>::failure(failed.error());
    } catch (...) {
        auto failed = fail({"STRATEGY_EXCEPTION", "Unknown exception during event handling"});
        return common::Result<StrategyRunResult, StrategyError>::failure(failed.error());
    }
}

common::Result<void, StrategyError> StrategyRuntime::on_execution(
    const trading::ExecutionReport& report, const StrategyContext& context) {
    if (state_ != LifecycleState::Running && state_ != LifecycleState::Paused)
        return common::Result<void, StrategyError>::failure(
            {"STRATEGY_NOT_ACTIVE", "Execution callbacks require RUNNING or PAUSED state"});
    try {
        strategy_->on_execution(report, context);
    } catch (const std::exception& error) {
        return fail({"STRATEGY_EXCEPTION", error.what()});
    } catch (...) {
        return fail({"STRATEGY_EXCEPTION", "Unknown exception during execution callback"});
    }
    return common::Result<void, StrategyError>::success();
}

common::Result<void, StrategyError> StrategyRuntime::pause() {
    return transition(LifecycleState::Paused);
}

common::Result<void, StrategyError> StrategyRuntime::resume() {
    return transition(LifecycleState::Running);
}

common::Result<void, StrategyError> StrategyRuntime::stop() {
    if (state_ == LifecycleState::Stopped || state_ == LifecycleState::Failed)
        return common::Result<void, StrategyError>::success();
    auto moved = transition(LifecycleState::Stopping);
    if (!moved) return moved;
    if (strategy_) strategy_->on_stop();
    return transition(LifecycleState::Stopped);
}

std::string_view to_string(const LifecycleState state) noexcept {
    switch (state) {
    case LifecycleState::Created: return "CREATED";
    case LifecycleState::Configured: return "CONFIGURED";
    case LifecycleState::Validated: return "VALIDATED";
    case LifecycleState::Initialising: return "INITIALISING";
    case LifecycleState::WarmingUp: return "WARMING_UP";
    case LifecycleState::Running: return "RUNNING";
    case LifecycleState::Paused: return "PAUSED";
    case LifecycleState::Stopping: return "STOPPING";
    case LifecycleState::Stopped: return "STOPPED";
    case LifecycleState::Failed: return "FAILED";
    }
    return "UNKNOWN";
}

} // namespace basalt::strategy
