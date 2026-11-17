#include "basalt/common/spsc_ring_buffer.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
struct Message { std::uint64_t sequence{}; std::int64_t enqueued_ns{}; };
constexpr std::uint64_t message_count = 250'000;
constexpr std::size_t queue_capacity = 1'024;

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

void report(const char* name, const double seconds, std::vector<std::int64_t>& latency_ns) {
    std::sort(latency_ns.begin(), latency_ns.end());
    const auto percentile_us = [&](const double percentile) {
        const auto index = static_cast<std::size_t>(percentile * static_cast<double>(latency_ns.size() - 1U));
        return static_cast<double>(latency_ns[index]) / 1'000.0;
    };
    std::cout << name << ": " << std::fixed << std::setprecision(0)
              << static_cast<double>(message_count) / seconds << " msg/s; "
              << std::setprecision(2) << "latency us p50=" << percentile_us(0.50)
              << " p95=" << percentile_us(0.95) << " p99=" << percentile_us(0.99)
              << " p99.9=" << percentile_us(0.999) << '\n';
}

void benchmark_spsc() {
    basalt::common::SpscRingBuffer<Message> queue(queue_capacity);
    std::vector<std::int64_t> latency;
    latency.reserve(message_count);
    const auto started = Clock::now();
    std::thread consumer([&] {
        for (std::uint64_t received = 0; received < message_count;) {
            auto message = queue.try_pop();
            if (!message) { std::this_thread::yield(); continue; }
            latency.push_back(now_ns() - message->enqueued_ns);
            ++received;
        }
    });
    for (std::uint64_t sequence = 0; sequence < message_count; ++sequence) {
        Message message{sequence, now_ns()};
        while (!queue.try_push(std::move(message))) std::this_thread::yield();
    }
    consumer.join();
    const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
    report("SPSC ring", seconds, latency);
}

void benchmark_mutex_queue() {
    std::mutex mutex;
    std::condition_variable not_empty;
    std::condition_variable not_full;
    std::deque<Message> queue;
    bool producer_done = false;
    std::vector<std::int64_t> latency;
    latency.reserve(message_count);
    const auto started = Clock::now();
    std::thread consumer([&] {
        while (true) {
            Message message{};
            {
                std::unique_lock lock(mutex);
                not_empty.wait(lock, [&] { return producer_done || !queue.empty(); });
                if (queue.empty()) return;
                message = queue.front();
                queue.pop_front();
            }
            not_full.notify_one();
            latency.push_back(now_ns() - message.enqueued_ns);
        }
    });
    for (std::uint64_t sequence = 0; sequence < message_count; ++sequence) {
        Message message{sequence, now_ns()};
        {
            std::unique_lock lock(mutex);
            not_full.wait(lock, [&] { return queue.size() < queue_capacity; });
            queue.push_back(message);
        }
        not_empty.notify_one();
    }
    {
        std::lock_guard lock(mutex);
        producer_done = true;
    }
    not_empty.notify_all();
    consumer.join();
    const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
    report("Mutex deque", seconds, latency);
}

} // namespace

int main() {
    std::cout << "bounded single producer / single consumer; messages=" << message_count
              << ", capacity=" << queue_capacity << '\n';
    benchmark_spsc();
    benchmark_mutex_queue();
    return 0;
}
