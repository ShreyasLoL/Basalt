# PROJECT ATLAS: Miniature Exchange Matching Engine
**Document Type:** Master System Architecture, Technical Specification & Autonomous Codex Execution Blueprint  
**Target Platform:** Windows (x64), VS Code, CMake, C++20  
**System Classification:** Deterministic Continuous Limit Order Book (CLOB) & Matching Engine  

---

## 1. Executive Summary

**Project Atlas** is a deterministic, single-instrument Continuous Limit Order Book (CLOB) and financial exchange matching engine written in modern **C++20**. It implements the core price-time priority (FIFO) mechanics used by institutional electronic exchanges (such as NASDAQ, CME, and NYSE) to ingest buy and sell orders, organize resting liquidity across discrete price levels, match compatible aggressive orders against passive resting orders, and emit immutable trade execution records.

Rather than optimizing prematurely for sub-microsecond kernel-bypass networking or lock-free distributed topologies, Project Atlas optimizes for **algorithmic correctness, architectural clarity, empirical observability, and verifiable systems engineering**.

### Overall Architecture Summary
The system is structured into four cleanly decoupled layers:
1. **Core Domain & Order Book Layer (`atlas::core`):** Pure, zero-dependency C++20 library implementing fixed-point tick pricing, the `OrderBook` data structure (price-ordered maps backed by intrusive/doubly-linked FIFO queues and an $\mathcal{O}(1)$ iterator lookup table), and the `MatchingEngine` state machine.
2. **Verification & Invariant Layer (`atlas::test`):** A comprehensive GoogleTest suite paired with a deterministic `OrderBook::verify_invariants()` auditor that mathematically validates book integrity after state mutations.
3. **Synthetic Workload & Benchmarking Layer (`atlas::bench`):** A deterministic workload generator (`std::mt19937_64` with fixed seeds) that pre-allocates 7 distinct market workload profiles in memory prior to timing, measuring nanosecond-level latency distributions ($\text{Mean}$, $p_{50}$, $p_{95}$, $p_{99}$, $p_{99.9}$) and comparing our primary $\mathcal{O}(1)$-cancellation hybrid book against a baseline contiguous-vector book.
4. **Local Gateway & Observability UI Layer (`atlas::server` & `web/`):** A lightweight C++ HTTP server (`cpp-httplib`) that wraps the engine and serves a zero-build, single-page HTML5/Tailwind/Canvas visual interface. The frontend never simulates market mechanics; it renders live snapshots, trade feeds, depth charts, and benchmark reports directly from the C++ engine.

---

## 2. Final Technology Stack

Every component in the stack has been selected to guarantee frictionless compilation on Windows inside VS Code without requiring external package managers (such as Conan or vcpkg) or heavy frontend toolchains (such as Node.js/npm/Webpack).

| Layer | Selected Technology | Version / Standard | Rationale |
| :--- | :--- | :--- | :--- |
| **Core Language** | **C++** | **C++20** (`-std=c++20` / `/std:c++20`) | Provides `std::span`, `[[nodiscard]]`, designated initializers, `<chrono>` calendar/nanosecond utilities, and strong `enum class` typing while remaining 100% supported by MSVC, Clang, and MinGW-w64. |
| **Build System** | **CMake** | **3.20+** | Industry-standard C++ build generator with native VS Code integration (`CMake Tools` extension) and built-in `FetchContent` for zero-setup dependency management. |
| **Compiler Support** | **MSVC (Visual Studio 2022) or MinGW-w64 (GCC 12+) / Clang 15+** | x64 Release & Debug | Standardizes compiler flags across Windows environments (`/W4 /O2` on MSVC, `-Wall -Wextra -O3` on GCC/Clang). |
| **Unit Testing** | **GoogleTest (`gtest`)** | `v1.14.0` (via CMake `FetchContent`) | De facto standard for C++ testing; supports parameterized tests, fixture setup, and seamless CTest integration in VS Code. |
| **HTTP Gateway** | **`cpp-httplib`** | `v0.15.3` (Header-only via `FetchContent`) | Single-header C++ HTTP library using native Windows Winsock2 (`ws2_32`). Eliminates Boost/Asio compilation overhead. |
| **JSON Serialization** | **`nlohmann/json`** | `v3.11.3` (Header-only via `FetchContent`) | Ergonomic, reliable JSON serialization restricted strictly to the HTTP gateway boundary (never used in the engine hot path). |
| **Visual Interface** | **Vanilla HTML5 + ES6 JS + Tailwind CSS (CDN)** | Static SPA (`web/index.html`) | Served directly by the C++ executable on `http://localhost:8080`. Requires zero Node.js installation, zero `node_modules`, and starts instantaneously in one click. |

---

## 3. Architecture

### 3.1 Architectural Principles & Component Separation
1. **The Core Engine Knows Nothing About JSON, HTTP, or I/O:** Everything inside `include/atlas/core/` and `src/core/` uses purely native C++ structs and standard containers. It never performs console I/O (`std::cout`), network calls, or JSON parsing.
2. **Deterministic State Machine:** The `MatchingEngine` is a pure state machine:
   $$\text{State}_{t+1}, \text{OrderResult}_t = f(\text{State}_t, \text{OrderCommand}_t)$$
   Given an identical initial book state and an identical sequence of commands, the engine is guaranteed to emit the exact same sequence of trades and sequence numbers.
3. **Unidirectional Dependency Graph:**
   * `atlas_core` (Static Library) $\leftarrow$ depends only on the C++20 Standard Library.
   * `atlas_tests` (Executable) $\leftarrow$ depends on `atlas_core` + `GTest`.
   * `atlas_bench` (Executable / Library module) $\leftarrow$ depends on `atlas_core`.
   * `atlas_server` (Main Executable) $\leftarrow$ depends on `atlas_core`, `atlas_bench`, `cpp-httplib`, and `nlohmann/json`.

### 3.2 System Architecture Diagram

~~~mermaid
flowchart TB
    subgraph Frontend["Visual Interface (Browser @ localhost:8080)"]
        UI_Book["Order Book Ladder & Depth View"]
        UI_Entry["Order Entry & Sim Controls"]
        UI_Trades["Live Trade Feed & Activity Log"]
        UI_Bench["Benchmark Runner & Latency Charts"]
    end

    subgraph Gateway["C++ Gateway Layer (atlas_server)"]
        HTTP["cpp-httplib REST Server\n(Mutex-Guarded Engine Access)"]
        JSON["nlohmann::json Serializer"]
        SimGen["Interactive Synthetic Order Generator"]
    end

    subgraph Core["C++20 Core Engine Layer (atlas_core)"]
        ME["MatchingEngine\n(Validation, Sequence Gen, Matching Loop)"]
        OB["OrderBook (MapListOrderBook)\nBids: std::map<Price, PriceLevel, std::greater>\nAsks: std::map<Price, PriceLevel, std::less>\nIndex: std::unordered_map<OrderId, OrderLocation>"]
        TL["TradeLog & Activity RingBuffer\n(std::deque / std::vector)"]
    end

    subgraph Verification["Testing & Benchmarking Suite"]
        GT["GoogleTest Suite (atlas_tests)\n18 Scenarios + Invariant Auditor"]
        BM["Benchmark Suite (atlas_bench)\n7 Pre-allocated Workloads + Latency Profiler"]
        VOB["VectorOrderBook (Baseline Comparison)"]
    end

    UI_Entry -->|"POST /api/orders\nDELETE /api/orders/{id}\nPOST /api/sim/step"| HTTP
    HTTP -->|"GET /api/state"| UI_Book
    HTTP -->|"GET /api/state"| UI_Trades
    UI_Bench -->|"POST /api/benchmark"| HTTP

    HTTP <--> JSON
    HTTP --> SimGen
    SimGen --> ME
    HTTP --> ME
    HTTP --> BM

    ME --> OB
    ME --> TL
    GT --> ME
    GT --> OB
    BM --> ME
    BM --> VOB
~~~

### 3.3 Order Lifecycle Sequence

~~~mermaid
sequenceDiagram
    participant Client as Caller (HTTP / Benchmark / Test)
    participant ME as MatchingEngine
    participant OB as OrderBook
    participant TL as TradeLog

    Client->>ME: submit_order(OrderRequest)
    ME->>ME: validate(request) & assign SequenceNumber
    alt Validation Fails
        ME-->>Client: OrderResult(REJECTED, reason)
    else Validation Succeeds
        ME->>OB: match_incoming_order(Order)
        loop While incoming.remaining_qty > 0 AND opposing best price crosses
            OB->>OB: front_order = best_price_level.orders.front()
            OB->>OB: fill_qty = min(incoming.remaining_qty, front_order.remaining_qty)
            OB->>TL: record Trade(exec_price = front_order.price, fill_qty)
            OB->>OB: decrement quantities; remove front_order if remaining_qty == 0
            OB->>OB: remove best_price_level if empty
        end
        alt incoming.remaining_qty > 0 AND incoming.type == LIMIT
            OB->>OB: insert_resting_order(incoming) into PriceLevel & LookupMap
        else incoming.remaining_qty > 0 AND incoming.type == MARKET
            OB->>OB: cancel unfilled market remainder (IOC semantics)
        end
        OB-->>ME: MatchingSummary(trades, final_status)
        ME-->>Client: OrderResult(status, trades, remaining_qty)
    end
~~~

---

## 4. Critical Engineering Decisions

### Decision 1: Price and Quantity Representation
* **Selected Approach:** **64-bit Fixed-Point Integer Price Ticks (`int64_t`) and 64-bit Unsigned Quantities (`uint64_t`).**
  * Every price is stored as an integer number of discrete minimum price increments ("ticks"), where $1\text{ tick} = \$0.01$ (or $100\text{ ticks} = \$1.00$). For example, $\$105.25$ is stored in C++ as `Price = 10525`.
  * Quantities are stored as `uint64_t` whole shares/contracts (`Quantity > 0`).
* **Alternatives Considered:**
  1. *IEEE 754 Floating-Point (`double` / `float`):* Standard hardware floats.
  2. *Arbitrary-Precision Decimal Class (`boost::multiprecision::cpp_dec_float` or custom `Decimal` struct):* Stores mantissa and exponent separately.
* **Reasoning:** IEEE 754 binary floating-point cannot represent base-10 fractions like $0.10$ or $0.01$ exactly ($0.1 + 0.2 = 0.30000000000000004$). Using `double` as a key in an ordered map (`std::map<double, PriceLevel>`) causes catastrophic bucket fragmentation, failed price-level lookups, and non-deterministic rounding drift. Fixed-point `int64_t` ticks provide exact hardware integer ALU comparisons (`==`, `<`, `>`), zero rounding errors, and single-cycle hashing/comparison.
* **Trade-offs:** The UI or API client must format ticks into human-readable decimal strings (e.g., dividing by $100$ for display) or submit integer ticks directly. To make this effortless, the API accepts integer ticks (and clearly documents $100\text{ ticks} = \$1.00$), while the UI displays both formatted dollars (`$100.50`) and raw ticks (`10050`).
* **Limitations:** Fixed tick sizes cannot change dynamically mid-session without re-indexing the book.

### Decision 2: Primary Order Book Data Structure
* **Selected Approach:** **Hybrid Three-Structure Design (`MapListOrderBook`):**
  1. **Price Levels:** `std::map<Price, PriceLevel, std::greater<Price>>` for Bids (highest price at `.begin()`) and `std::map<Price, PriceLevel, std::less<Price>>` for Asks (lowest price at `.begin()`).
  2. **FIFO Queue Within Each Price Level:** `std::list<Order>` inside each `PriceLevel`, accompanied by cached `uint64_t total_volume` and `size_t order_count` fields on the `PriceLevel` struct.
  3. **Fast Order Lookup Index:** `std::unordered_map<OrderId, OrderLocation>` where `OrderLocation` stores `{ Side side; Price price; std::list<Order>::iterator iter; }`.
* **Alternatives Considered:**
  1. *Sorted Contiguous Vector (`std::vector<PriceLevel>` + `std::vector<Order>`):* High spatial cache locality for small books.
  2. *Array-Indexed Direct Lookup Table (`std::vector<PriceLevel>` indexed directly by `Price` tick offset) + Custom Arena Object Pool with Intrusive Doubly-Linked Lists:* The institutional HFT approach.
  3. *Binary Heap / Priority Queue (`std::priority_queue<Order>`):* Simple insertion and top lookup.
* **Reasoning:**
  * A `std::priority_queue` is disqualifying because cancelling an arbitrary resting order by `OrderId` requires an $\mathcal{O}(N)$ linear scan and rebuild.
  * While an HFT Arena + Direct Price Array achieves the highest theoretical throughput, a naive array indexed by price fails if a user submits an outlier limit order at price $\$1,000,000.00$, and custom pointer manipulation obscures core exchange logic behind manual allocator bookkeeping.
  * The **Hybrid `std::map` + `std::list` + `std::unordered_map`** design hits the sweet spot:
    * **Best-Price Lookup (`best_bid` / `best_ask`):** $\mathcal{O}(1)$ via `.begin()` on `std::map`.
    * **Matching Against Front of Queue:** $\mathcal{O}(1)$ via `level.orders.front()`.
    * **Order Insertion at Existing Price Level:** $\mathcal{O}(\log P)$ to find the `PriceLevel` (where $P$ is the number of active price levels, typically $50\text{--}500$, far smaller than total orders $N$) + $\mathcal{O}(1)$ `push_back` into `std::list` + $\mathcal{O}(1)$ average insertion into `std::unordered_map`.
    * **Order Cancellation by ID:** $\mathcal{O}(1)$ average lookup in `std::unordered_map` yields the exact `std::list<Order>::iterator`. Because `std::list` iterators are never invalidated by insertions or deletions of other elements, erasing the order from the queue takes strict $\mathcal{O}(1)$ time! If the `PriceLevel` becomes empty, erasing the level from `std::map` takes $\mathcal{O}(\log P)$.
  * **Empirical Engineering Lesson (Dual-Book Benchmark):** To turn data structure trade-offs into a concrete demonstration for interviews, Project Atlas also implements a lightweight baseline **`VectorOrderBook`** (using contiguous `std::vector` structures) alongside the primary **`MapListOrderBook`** inside the benchmark suite. Running the benchmark demonstrates firsthand when contiguous memory wins (tiny books with few price levels) and when $\mathcal{O}(1)$ iterator erasure crushes $\mathcal{O}(N)$ vector shifting (deep books with heavy cancellations).
* **Trade-offs:** `std::list` and `std::map` allocate heap nodes individually, which incurs pointer-chasing and L1/L2 cache misses compared to an intrusive flat pool.
* **Limitations:** Heap allocation jitter on the `new` path can elevate $p_{99.9}$ tail latency under multi-million-order bursts.

### Decision 3: Memory Ownership and C++ Design
* **Selected Approach:** **Strict Value Semantics & Container Ownership (Zero `std::shared_ptr`, Zero Raw `new`/`delete`).**
  * Every `Order` lives by value directly inside its `PriceLevel::orders` (`std::list<Order>`).
  * The `std::unordered_map<OrderId, OrderLocation>` stores a lightweight value struct containing a `std::list<Order>::iterator` (which remains valid for the entire resting lifetime of that `Order` inside `std::list`).
  * Completed `Trade` structs are value-types returned in `std::vector<Trade>` and appended to a bounded history buffer (`std::deque<Trade>`).
* **Alternatives Considered:**
  1. *`std::shared_ptr<Order>` shared between the PriceLevel queue, the ID lookup table, and trade records.*
  2. *Raw `Order*` pointers managed manually via `new` and `delete`.*
* **Reasoning:** Using `std::shared_ptr<Order>` introduces atomic reference-count increments/decrements on every copy and lookup, destroying single-threaded performance and obscuring ownership. Storing `Order` by value inside `std::list<Order>` establishes a single, unambiguous owner (`PriceLevel`), eliminates memory leaks by construction (RAII), and allows $\mathcal{O}(1)$ iterator splicing/erasure.
* **Trade-offs:** Requires careful ordering during cancellation: erase the `std::list<Order>::iterator` from the `PriceLevel` first, update the level volume, erase the `PriceLevel` if empty, and erase the `OrderLocation` entry from `std::unordered_map`.
* **Limitations:** Iterators must never be dereferenced after the order is filled or cancelled.

### Decision 4: Execution Model (Single-Threaded vs. Concurrent)
* **Selected Approach:** **Single-Threaded Deterministic Matching Core with a Coarse Mutex at the HTTP Adapter Boundary.**
* **Alternatives Considered:**
  1. *Multi-threaded Fine-Grained Locking inside the OrderBook (e.g., locking individual price levels).*
  2. *Lock-Free SPSC (Single-Producer Single-Consumer) Ring Buffer feeding a pinned Matching Thread.*
* **Reasoning:** Even in real-world institutional exchanges (like NASDAQ OMX and LMAX Disruptor), the matching engine for a single instrument is **strictly single-threaded**. Why? Because price-time priority requires a strict total ordering of events. Introducing multiple threads mutating the same order book introduces lock contention, cache-line bouncing (MESI protocol invalidations), and non-deterministic race conditions without parallelizing the inherently serial matching loop.
  * In **Benchmark Mode**, the engine runs on a single thread with **zero locks**, measuring raw algorithmic throughput and latency.
  * In **Interactive Server Mode**, `cpp-httplib` handles incoming HTTP requests using a single `std::mutex` guarding the `EngineController` facade so concurrent browser fetches (`GET /api/state` and `POST /api/orders`) never race.
* **Trade-offs:** Cannot parallelize matching within a single stock symbol.
* **Limitations:**Scaling to multiple symbols in the future would use *sharding by symbol* (one independent single-threaded `MatchingEngine` instance per ticker symbol).

### Decision 5: Execution Price Convention & Market Order Handling
* **Selected Approach:** **Maker/Resting Price Execution Convention + Immediate-Or-Cancel (IOC) Remainder for Market Orders.**
  * When an aggressive incoming order crosses the spread against a passive resting order, the trade always executes at the **resting order's price** (`execution_price = resting_order.price`).
  * For example, if a passive Sell Limit order rests at $\$100.00$ (`10000` ticks) and an aggressive Buy Limit order arrives at $\$105.00$ (`10500` ticks), the trade executes at $\$100.00$ (`10000` ticks), granting the price improvement to the incoming aggressor while giving the passive maker their exact limit price.
  * When an incoming **Market Order** arrives, it matches immediately against available opposing liquidity at each successive price level until filled or until the opposing side of the book is exhausted. Any **unfilled remainder of a Market Order is immediately cancelled (`EXPIRED` / `PARTIALLY_FILLED_AND_CANCELLED`)** and **never** rests in the book (since a Market Order has no limit price).
  * If a Market Order arrives when the opposing side of the book is completely empty, zero trades occur and the order is immediately cancelled.
* **Alternatives Considered:**
  1. *Executing at the incoming order's price.*
  2. *Converting unfilled market order remainders into limit orders at the last traded price (Market-to-Limit).*
* **Reasoning:** Executing at the resting order's price is the universal rule of continuous double auctions (CLOB). Cancelling unfilled market order remainders prevents synthetic price invention when liquidity dries up.

### Decision 6: Frontend Communication & Visual Stack
* **Selected Approach:** **Embedded C++ REST Server (`cpp-httplib`) serving a Self-Contained Vanilla HTML5/JS/Tailwind Single-Page Application (`web/index.html`).**
* **Alternatives Considered:**
  1. *React / Vite / TypeScript frontend + WebSockets.*
  2. *Dear ImGui / GLFW native C++ desktop GUI.*
  3. *CLI-only terminal UI (ncurses).*
* **Reasoning:**
  * A React/Vite build adds `package.json`, `node_modules`, npm version mismatches, and CORS headaches that distract from a C++ systems project.
  * Dear ImGui requires OpenGL/DirectX windowing dependencies that complicate headless testing and automated builds on Windows.
  * Serving a clean, static `web/index.html` directly from the compiled C++ `atlas_server.exe` binary over `http://localhost:8080` gives the best of both worlds: you compile **only C++**, run `atlas_server.exe`, open `http://localhost:8080`, and get a responsive, modern institutional dark-mode interface powered by real REST calls to the C++ engine.

---

## 5. Data Structures and Models

All core types live under `namespace atlas`.

### 5.1 Fundamental Type Aliases & Enumerations (`include/atlas/core/types.hpp`)

~~~cpp
#pragma once
#include <cstdint>
#include <string>

namespace atlas {

using OrderId        = uint64_t;
using TradeId        = uint64_t;
using Price          = int64_t;   // Fixed-point integer ticks (e.g., 10000 = $100.00)
using Quantity       = uint64_t;  // Whole units/shares (must be > 0)
using SequenceNumber = uint64_t;  // Strictly monotonic engine event counter
using TimestampNs    = uint64_t;  // Monotonic nanoseconds since engine start

enum class Side : uint8_t {
    Buy  = 0,
    Sell = 1
};

enum class OrderType : uint8_t {
    Limit  = 0,
    Market = 1
};

enum class OrderStatus : uint8_t {
    New             = 0, // Accepted and resting in the book (0 filled so far)
    PartiallyFilled = 1, // Partially executed; remainder resting (Limit) or cancelled (Market)
    Filled          = 2, // 100% of initial_quantity executed
    Cancelled       = 3, // Explicitly cancelled by user or unfilled Market order remainder
    Rejected        = 4  // Failed validation (e.g., qty == 0, limit price <= 0, duplicate ID)
};

enum class ActionType : uint8_t {
    Submit = 0,
    Cancel = 1,
    Trade  = 2,
    Reject = 3
};

} // namespace atlas
~~~

### 5.2 Core Domain Structures (`include/atlas/core/order.hpp`, `trade.hpp`)

~~~cpp
#pragma once
#include "atlas/core/types.hpp"
#include <vector>
#include <list>

namespace atlas {

struct OrderRequest {
    OrderId   id{0};
    Side      side{Side::Buy};
    OrderType type{OrderType::Limit};
    Price     price{0};          // Ignored / set to 0 for Market orders
    Quantity  quantity{0};
};

struct Order {
    OrderId        id{0};
    Side           side{Side::Buy};
    OrderType      type{OrderType::Limit};
    Price          price{0};
    Quantity       initial_quantity{0};
    Quantity       remaining_quantity{0};
    SequenceNumber sequence{0};
    TimestampNs    timestamp_ns{0};

    [[nodiscard]] Quantity filled_quantity() const noexcept {
        return initial_quantity - remaining_quantity;
    }
    [[nodiscard]] bool is_filled() const noexcept {
        return remaining_quantity == 0;
    }
};

struct PriceLevel {
    Price            price{0};
    Quantity         total_volume{0}; // Cached sum of remaining_quantity across all orders in list
    std::list<Order> orders{};        // FIFO queue: front() is oldest (highest time priority)

    [[nodiscard]] bool empty() const noexcept {
        return orders.empty();
    }
    [[nodiscard]] std::size_t order_count() const noexcept {
        return orders.size();
    }
};

struct OrderLocation {
    Side                        side{Side::Buy};
    Price                       price{0};
    std::list<Order>::iterator  iterator{};
};

struct Trade {
    TradeId        trade_id{0};
    OrderId        buy_order_id{0};
    OrderId        sell_order_id{0};
    OrderId        aggressor_order_id{0};
    OrderId        resting_order_id{0};
    Side           aggressor_side{Side::Buy};
    Price          price{0};          // Always the resting order's limit price
    Quantity       quantity{0};
    SequenceNumber sequence{0};
    TimestampNs    timestamp_ns{0};
};

struct ActivityRecord {
    SequenceNumber sequence{0};
    ActionType     action{ActionType::Submit};
    OrderId        order_id{0};
    Side           side{Side::Buy};
    OrderType      order_type{OrderType::Limit};
    Price          price{0};
    Quantity       quantity{0};
    OrderStatus    status{OrderStatus::New};
    std::string    message{};
};

struct OrderResult {
    OrderId            order_id{0};
    OrderStatus        status{OrderStatus::Rejected};
    Quantity           initial_quantity{0};
    Quantity           filled_quantity{0};
    Quantity           remaining_quantity{0};
    SequenceNumber     sequence{0};
    std::vector<Trade> trades{};
    std::string        message{};
};

struct CancelResult {
    OrderId        order_id{0};
    bool           success{false};
    OrderStatus    status{OrderStatus::Rejected};
    Price          cancelled_price{0};
    Quantity       cancelled_quantity{0};
    SequenceNumber sequence{0};
    std::string    message{};
};

struct PriceLevelSummary {
    Price       price{0};
    Quantity    total_volume{0};
    std::size_t order_count{0};
};

struct BookSnapshot {
    std::vector<PriceLevelSummary> bids{}; // Sorted highest price first
    std::vector<PriceLevelSummary> asks{}; // Sorted lowest price first
    std::vector<Order>             resting_orders{}; // Individual resting orders in priority order
    bool                           has_best_bid{false};
    Price                          best_bid{0};
    Quantity                       best_bid_volume{0};
    bool                           has_best_ask{false};
    Price                          best_ask{0};
    Quantity                       best_ask_volume{0};
    bool                           has_spread{false};
    Price                          spread{0};
};

struct EngineStats {
    uint64_t total_orders_submitted{0};
    uint64_t total_orders_rejected{0};
    uint64_t total_orders_cancelled{0};
    uint64_t total_trades_executed{0};
    Quantity total_volume_traded{0};
    bool     has_last_trade_price{false};
    Price    last_trade_price{0};
    Quantity last_trade_quantity{0};
    std::size_t active_order_count{0};
    std::size_t bid_level_count{0};
    std::size_t ask_level_count{0};
};

} // namespace atlas
~~~

---

## 6. Matching Algorithm

### 6.1 Validation Rules
Before any order enters the matching loop, `MatchingEngine::submit_order(const OrderRequest& req)` validates:
1. **`req.id > 0`:** Order ID zero is reserved/invalid.
2. **Unique Order ID:** `req.id` must not already exist as an active resting order in `order_lookup_` (`std::unordered_map<OrderId, OrderLocation>`) or in the set of previously seen IDs if strict uniqueness is enforced. Specifically, if `book_.has_order(req.id)` is true, reject with `OrderStatus::Rejected` (`"Duplicate active Order ID"`).
3. **`req.quantity > 0` and `req.quantity <= MAX_ORDER_QUANTITY` ($10^9$):** Zero-quantity orders or overflow-inducing quantities are rejected.
4. **Price Validation:**
   * If `req.type == OrderType::Limit`: `req.price > 0` and `req.price <= MAX_ORDER_PRICE` ($10^{12}$). Limit orders with price $\le 0$ are rejected (`"Limit order price must be positive"`).
   * If `req.type == OrderType::Market`: `req.price` is normalized to $0$ internally (since market orders have no price limit).

### 6.2 Price Compatibility & Crossing Rules
Let `incoming` be the validated aggressive order and `best_opposing_level` be the first entry (`.begin()`) of the opposite side of the book:
* **Incoming Buy Limit Order (`incoming.side == Side::Buy`, `incoming.type == OrderType::Limit`):**
  * Opposing book is `asks_` (sorted ascending by `std::less<Price>`, so `asks_.begin()` has the lowest ask price).
  * Compatible if and only if:
    $$\text{incoming.price} \ge \text{best\_ask.price}$$
* **Incoming Sell Limit Order (`incoming.side == Side::Sell`, `incoming.type == OrderType::Limit`):**
  * Opposing book is `bids_` (sorted descending by `std::greater<Price>`, so `bids_.begin()` has the highest bid price).
  * Compatible if and only if:
    $$\text{incoming.price} \le \text{best\_bid.price}$$
* **Incoming Market Order (`incoming.type == OrderType::Market`):**
  * Compatible with *any* price level on the opposing side as long as the opposing side is non-empty.

### 6.3 Exact Matching Procedure (Pseudocode)

~~~text
FUNCTION submit_order(req: OrderRequest) -> OrderResult:
    1. IF NOT validate(req):
           INCREMENT stats.total_orders_rejected
           RECORD ActivityRecord(Reject)
           RETURN OrderResult(status = Rejected, message = validation_error)

    2. seq = ++next_sequence_number_
       now = current_monotonic_ns()
       incoming = Order {
           id: req.id, side: req.side, type: req.type,
           price: (req.type == Market ? 0 : req.price),
           initial_quantity: req.quantity,
           remaining_quantity: req.quantity,
           sequence: seq, timestamp_ns: now
       }
       INCREMENT stats.total_orders_submitted
       trades = empty vector<Trade>

    3. // MATCHING LOOP (Aggressive Phase)
       WHILE incoming.remaining_quantity > 0:
           opposing_side = (incoming.side == Buy) ? book.asks : book.bids
           IF opposing_side.empty():
               BREAK

           best_level_iter = opposing_side.begin()
           best_price = best_level_iter->first
           level = best_level_iter->second

           // Check price compatibility for Limit orders
           IF incoming.type == Limit:
               IF incoming.side == Buy AND incoming.price < best_price:
                   BREAK
               IF incoming.side == Sell AND incoming.price > best_price:
                   BREAK

           // Match against FIFO queue inside best_level
           WHILE incoming.remaining_quantity > 0 AND NOT level.orders.empty():
               resting = REF(level.orders.front())
               fill_qty = MIN(incoming.remaining_quantity, resting.remaining_quantity)
               exec_price = resting.price  // CRITICAL: Always execute at resting order's price!

               incoming.remaining_quantity -= fill_qty
               resting.remaining_quantity  -= fill_qty
               level.total_volume          -= fill_qty

               trade_seq = ++next_sequence_number_
               trade = Trade {
                   trade_id: ++next_trade_id_,
                   buy_order_id:  (incoming.side == Buy) ? incoming.id : resting.id,
                   sell_order_id: (incoming.side == Sell) ? incoming.id : resting.id,
                   aggressor_order_id: incoming.id,
                   resting_order_id:   resting.id,
                   aggressor_side:     incoming.side,
                   price:              exec_price,
                   quantity:           fill_qty,
                   sequence:           trade_seq,
                   timestamp_ns:       now
               }
               trades.push_back(trade)
                RECORD trade in trade_history_ and stats

               IF resting.remaining_quantity == 0:
                   book.order_lookup.erase(resting.id)
                   level.orders.pop_front()

           // Clean up exhausted price level immediately
           IF level.orders.empty():
               opposing_side.erase(best_level_iter)

    4. // RESTING OR EXPIRATION PHASE (Passive Phase)
       IF incoming.remaining_quantity == 0:
           final_status = OrderStatus::Filled
       ELSE IF incoming.type == Limit:
           // Rest remaining quantity in own side of book at incoming.price
           own_side = (incoming.side == Buy) ? book.bids : book.asks
           level_ref = own_side[incoming.price]
           level_ref.price = incoming.price
           level_ref.total_volume += incoming.remaining_quantity
           level_ref.orders.push_back(incoming)
           iter = std::prev(level_ref.orders.end())
           book.order_lookup[incoming.id] = OrderLocation{incoming.side, incoming.price, iter}

           final_status = (incoming.remaining_quantity < incoming.initial_quantity)
                          ? OrderStatus::PartiallyFilled
                          : OrderStatus::New
       ELSE: // incoming.type == Market with remaining_quantity > 0
           // Unfilled Market order remainder is cancelled immediately (never rests)
           final_status = (incoming.remaining_quantity < incoming.initial_quantity)
                          ? OrderStatus::PartiallyFilled
                          : OrderStatus::Cancelled

    5. RECORD ActivityRecord(Submit, final_status)
       RETURN OrderResult {
           order_id: incoming.id,
           status: final_status,
           initial_quantity: incoming.initial_quantity,
           filled_quantity: incoming.initial_quantity - incoming.remaining_quantity,
           remaining_quantity: incoming.remaining_quantity,
           sequence: seq,
           trades: trades
       }
~~~

### 6.4 Order Cancellation Procedure (Pseudocode)

~~~text
FUNCTION cancel_order(order_id: OrderId) -> CancelResult:
    1. lookup_iter = book.order_lookup.find(order_id)
       IF lookup_iter == book.order_lookup.end():
           RETURN CancelResult(order_id, success = false, status = Rejected, message = "Order ID not found")

    2. loc = lookup_iter->second
       cancelled_qty = loc.iterator->remaining_quantity
       cancelled_price = loc.price
       side = loc.side

    3. side_map = (side == Buy) ? book.bids : book.asks
       level_iter = side_map.find(cancelled_price)
       ASSERT(level_iter != side_map.end())

       level_iter->second.total_volume -= cancelled_qty
       level_iter->second.orders.erase(loc.iterator) // O(1) list erasure

       IF level_iter->second.orders.empty():
           side_map.erase(level_iter)                // O(log P) map level cleanup

       book.order_lookup.erase(lookup_iter)          // O(1) hash map cleanup

    4. seq = ++next_sequence_number_
       INCREMENT stats.total_orders_cancelled
       RECORD ActivityRecord(Cancel, order_id, side, cancelled_price, cancelled_qty)
       RETURN CancelResult(order_id, success = true, status = Cancelled, cancelled_price, cancelled_qty, seq)
~~~

### 6.5 Edge Cases Explicitly Handled
1. **Multi-Level Sweep (Walking the Book):** An incoming order with `quantity = 500` facing three ask levels (`100 @ 10000`, `150 @ 10050`, `300 @ 10100`) and limit price `10100` will generate 3 distinct `Trade` records at prices `10000`, `10050`, and `10100` respectively, completely removing the `10000` and `10050` levels, and leaving `50 @ 10100` resting in the third level.
2. **Partial Fill Resting:** If that same incoming order had `quantity = 600` at limit price `10100` and the next ask level was `10200`, it fills `550` across the three compatible levels and rests the remaining `50` on the Bid side at `10100`, immediately becoming the new Best Bid.
3. **Market Order on Thin or Empty Book:** A Market Buy for `1000` shares against a book containing only `200` Ask shares fills `200` shares and cancels the remaining `800` shares (`status = PartiallyFilled`, `remaining_quantity = 800`, active book remains empty). A Market Buy on a completely empty Ask book fills `0` shares and returns `status = Cancelled`.
4. **Overflow Protection:** Price and quantity arithmetic uses 64-bit integers with validation caps (`MAX_ORDER_PRICE = 1'000'000'000'000`, `MAX_ORDER_QUANTITY = 1'000'000'000`) so `total_volume` sums never overflow `uint64_t`.

---

## 7. API and Communication

The C++ executable `atlas_server` starts an HTTP server on `http://127.0.0.1:8080` (configurable via CLI `--port`).
* `GET /` serves the single-page web interface (`web/index.html`, or an embedded fallback HTML string if run from a different working directory so it never fails with a 404).
* All API routes use `Content-Type: application/json` and CORS headers (`Access-Control-Allow-Origin: *`).

### 7.1 Endpoint Specification

#### 1. `GET /api/state`
Returns the current order book depth, market summary statistics, recent trades (up to 50), and recent order activity log (up to 50).
* **Query Parameters:** `depth` (optional, default `20`).
* **Response (`200 OK`):**
~~~json
{
  "market": {
    "best_bid": 10000,
    "has_best_bid": true,
    "best_bid_volume": 250,
    "best_ask": 10050,
    "has_best_ask": true,
    "best_ask_volume": 180,
    "spread": 50,
    "has_spread": true,
    "last_trade_price": 10025,
    "has_last_trade_price": true,
    "last_trade_quantity": 50,
    "total_volume_traded": 1420,
    "total_trades_executed": 18,
    "total_orders_submitted": 45,
    "total_orders_cancelled": 4,
    "active_order_count": 12,
    "bid_level_count": 5,
    "ask_level_count": 4
  },
  "book": {
    "bids": [
      { "price": 10000, "total_volume": 250, "order_count": 2 },
      { "price": 9950,  "total_volume": 400, "order_count": 3 }
    ],
    "asks": [
      { "price": 10050, "total_volume": 180, "order_count": 1 },
      { "price": 10100, "total_volume": 320, "order_count": 2 }
    ],
    "resting_orders": [
      {
        "id": 101,
        "side": "BUY",
        "type": "LIMIT",
        "price": 10000,
        "initial_quantity": 150,
        "remaining_quantity": 150,
        "sequence": 12
      }
    ]
  },
  "trades": [
    {
      "trade_id": 18,
      "buy_order_id": 104,
      "sell_order_id": 98,
      "aggressor_side": "BUY",
      "price": 10025,
      "quantity": 50,
      "sequence": 44
    }
  ],
  "activity": [
    {
      "sequence": 44,
      "action": "TRADE",
      "order_id": 104,
      "side": "BUY",
      "order_type": "LIMIT",
      "price": 10025,
      "quantity": 50,
      "status": "FILLED",
      "message": "Executed 50 @ 10025 (Buy #104 vs Sell #98)"
    }
  ]
}
~~~

#### 2. `POST /api/orders`
Submits a new manual Buy or Sell order (`LIMIT` or `MARKET`). If `id` is omitted or `0`, the server auto-assigns the next monotonic `OrderId`.
* **Request Body:**
~~~json
{
  "id": 0,
  "side": "BUY",
  "type": "LIMIT",
  "price": 10050,
  "quantity": 100
}
~~~
* **Response (`200 OK` or `400 Bad Request` on rejection):**
~~~json
{
  "order_id": 105,
  "status": "PARTIALLY_FILLED",
  "initial_quantity": 100,
  "filled_quantity": 60,
  "remaining_quantity": 40,
  "sequence": 46,
  "message": "Order accepted",
  "trades": [
    {
      "trade_id": 19,
      "buy_order_id": 105,
      "sell_order_id": 99,
      "aggressor_side": "BUY",
      "price": 10050,
      "quantity": 60,
      "sequence": 47
    }
  ]
}
~~~

#### 3. `DELETE /api/orders/:id`
Cancels an active resting order by its numeric `OrderId`.
* **Response (`200 OK` or `404 Not Found`):**
~~~json
{
  "order_id": 101,
  "success": true,
  "status": "CANCELLED",
  "cancelled_price": 10000,
  "cancelled_quantity": 150,
  "sequence": 48,
  "message": "Cancelled 150 units @ 10000"
}
~~~

#### 4. `POST /api/sim/step`
Generates and executes `count` synthetic orders against the live engine to populate the book or drive visual simulation mode.
* **Request Body:** `{ "count": 5 }` (clamped between 1 and 500).
* **Response (`200 OK`):** `{ "executed": 5, "active_orders": 28, "trades_generated": 3 }`

#### 5. `POST /api/reset`
Resets the engine state to a clean empty order book (or optionally seeds an initial balanced book if `{"seed_demo": true}` is passed).

#### 6. `POST /api/benchmark`
Triggers the in-memory C++ benchmark runner for a selected workload profile (or all 7 profiles) and returns the empirical latency and throughput measurements.
* **Request Body:**
~~~json
{
  "profile": "ALL",
  "order_count": 100000,
  "compare_baseline": true
}
~~~
* **Response (`200 OK`):**
~~~json
{
  "results": [
    {
      "profile_name": "1. Balanced Market Flow",
      "implementation": "MapListOrderBook (Primary)",
      "orders_processed": 100000,
      "trades_executed": 42150,
      "cancellations_executed": 14820,
      "total_time_ms": 48.25,
      "throughput_ops_per_sec": 2072538.8,
      "avg_latency_ns": 482.5,
      "p50_latency_ns": 390,
      "p95_latency_ns": 910,
      "p99_latency_ns": 1450,
      "p999_latency_ns": 4200
    }
  ]
}
~~~

---

## 8. Benchmarking

### 8.1 Methodology & Isolation Rules
To ensure the benchmark measures **only** the C++ order book and matching engine—and never accidentally measures random number generation, string formatting, HTTP overhead, or console printing—the benchmark suite (`atlas::bench::BenchmarkRunner`) enforces strict separation:

1. **Pre-Generation Phase (Untimed):**
   * Using a deterministic 64-bit Mersenne Twister (`std::mt19937_64 rng(42)`), the generator builds a contiguous `std::vector<WorkloadCommand>` of size $N$ (default $N = 100,000$ operations per profile) in memory **before** starting the clock.
   * Each `WorkloadCommand` is a compact, 32-byte value struct (`ActionType`, `OrderRequest`, or `cancel_order_id`).
2. **Pre-Allocated Latency Buffer (Untimed):**
   * A `std::vector<uint64_t> latencies_ns(N, 0)` is pre-allocated and zeroed prior to timing so recording per-operation latency never triggers a `std::vector` reallocation.
3. **Warm-Up Phase (Untimed):**
   * Runs $10,000$ operations on a throwaway `MatchingEngine` instance to warm up the CPU instruction cache and branch predictor.
4. **Timed Hot Loop:**
   * Records `batch_start = std::chrono::steady_clock::now()`.
   * For each index $i \in [0, N)$:
     * `t0 = std::chrono::steady_clock::now()`
     * Executes `engine.submit_order(cmd.req)` or `engine.cancel_order(cmd.cancel_id)`
     * `t1 = std::chrono::steady_clock::now()`
     * `latencies_ns[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()`
   * Records `batch_end = std::chrono::steady_clock::now()`.
5. **Post-Processing Phase (Untimed):**
   * Sorts `latencies_ns` to compute exact percentiles:
     $$\text{Mean}, \quad p_{50} \text{ (Median)}, \quad p_{95}, \quad p_{99}, \quad p_{99.9}$$
   * Computes throughput:
     $$\text{Throughput (ops/sec)} = \frac{N \times 10^9}{\text{Total Elapsed Nanoseconds}}$$

### 8.2 The Seven Required Workload Profiles
All workloads operate around a reference mid-price of $P_0 = 10,000$ ticks ($\$100.00$) with quantities sampled uniformly from $[1, 100]$:

| # | Profile ID & Name | Workload Composition & Distribution | Engineering Bottleneck Tested |
| :--- | :--- | :--- | :--- |
| **1** | `BALANCED`<br>Balanced Buying & Selling | $42.5\%$ Buy Limit, $42.5\%$ Sell Limit, $5\%$ Market, $10\%$ Cancel. Prices normally distributed around $P_0$ ($\sigma = 25$ ticks). | Baseline steady-state CLOB behavior with moderate spread crossing. |
| **2** | `BUY_HEAVY`<br>Mostly Buy-Side Activity | $75\%$ Buy Limit, $15\%$ Sell Limit, $5\%$ Market, $5\%$ Cancel. | Deep accumulation on the Bid book (`bids_` grows large while `asks_` stays repeatedly swept empty). |
| **3** | `SELL_HEAVY`<br>Mostly Sell-Side Activity | $15\%$ Buy Limit, $75\%$ Sell Limit, $5\%$ Market, $5\%$ Cancel. | Deep accumulation on the Ask book (`asks_` grows large while `bids_` is repeatedly swept empty). |
| **4** | `AGGRESSIVE_MATCHING`<br>Frequent Matching | $45\%$ Crossing Limit Orders (Buy price $\ge P_0 + 10$, Sell price $\le P_0 - 10$), $25\%$ Market Orders, $30\%$ Passive Limit Orders. | Multi-level sweep loop efficiency, `Trade` generation, and frequent `PriceLevel` creation/destruction. |
| **5** | `CANCEL_HEAVY`<br>Frequent Cancellations | $50\%$ Passive Limit Orders placed away from mid-price, $45\%$ Cancellations of active resting orders, $5\%$ Crossing Orders. | Tests $\mathcal{O}(1)$ `unordered_map` + `std::list::iterator` erasure vs. linear vector scanning. |
| **6** | `CONCENTRATED_LEVELS`<br>Concentrated Price Levels | $90\%$ Limit Orders restricted to only $10$ discrete price ticks ($[9995, 10005]$), $10\%$ Cancel. | Very long `std::list<Order>` queues per `PriceLevel` with tiny `std::map` tree depth ($P \le 10$). |
| **7** | `WIDE_DISTRIBUTION`<br>Distributed Price Levels | $90\%$ Limit Orders uniformly spread across $5,000$ distinct price ticks ($[7500, 12500]$), $10\%$ Cancel. | Deep `std::map` red-black tree traversal ($\mathcal{O}(\log P)$ where $P \approx 2500$ per side) and cache misses. |

### 8.3 Baseline Comparison (`MapListOrderBook` vs. `VectorOrderBook`)
To provide a genuine systems programming insight, `atlas_bench` also includes `VectorOrderBook`, which stores price levels in a sorted `std::vector<FlatPriceLevel>` and orders inside each level in a `std::vector<Order>` without an iterator lookup map.
* On **Profile 6 (`CONCENTRATED_LEVELS`)** and **Profile 5 (`CANCEL_HEAVY`)**, the benchmark proves why `std::vector::erase` in the middle of deep queues degrades when thousands of orders rest at the same price or cancel frequently, whereas `MapListOrderBook` sustains consistent $\mathcal{O}(1)$ cancellation latency.
* On **Profile 4 (`AGGRESSIVE_MATCHING`)** with shallow queues, the comparison highlights the cache locality of contiguous vectors vs. node-based containers.

---

## 9. Testing Strategy and Correctness Invariants

### 9.1 Formal Correctness Invariants (`OrderBook::verify_invariants()`)
The `OrderBook` exposes a diagnostic method `bool verify_invariants(std::string* error_out = nullptr) const` that asserts all 8 structural invariants of a valid order book:
1. **Strict Price Ordering Invariant:** Every adjacent pair of price levels in `bids_` satisfies $P_i > P_{i+1}$, and every adjacent pair in `asks_` satisfies $P_i < P_{i+1}$.
2. **Uncrossed Book Invariant:** Whenever both `bids_` and `asks_` are non-empty after a matching cycle completes, the resting book is never crossed:
   $$\text{best\_bid} < \text{best\_ask}$$
3. **Non-Empty Level Invariant:** No `PriceLevel` in `bids_` or `asks_` has `orders.empty() == true` or `total_volume == 0`.
4. **Positive Remaining Quantity Invariant:** Every resting `Order` in every `PriceLevel` satisfies:
   $$0 < \text{order.remaining\_quantity} \le \text{order.initial\_quantity}$$
5. **Volume Conservation Invariant:** For every `PriceLevel`, `level.total_volume` equals the exact arithmetic sum of `order.remaining_quantity` across all orders in `level.orders`.
6. **FIFO Time-Priority Invariant:** Within any `PriceLevel`, for every order $A$ preceding order $B$ in `level.orders`, their monotonic arrival sequence numbers satisfy:
   $$A.\text{sequence} < B.\text{sequence}$$
7. **Price-Level Assignment Invariant:** Every resting `Order` inside `PriceLevel(P)` has `order.price == P` and `order.type == OrderType::Limit` (no `Market` order ever rests in the book).
8. **Bijective Lookup Index Invariant:** The size of `order_lookup_` equals the exact total number of resting orders across `bids_` and `asks_`, and every entry in `order_lookup_` points to a valid iterator whose `id`, `side`, and `price` match the resting `Order`.

### 9.2 Required Automated Unit Tests (`tests/test_matching_engine.cpp`)
Every test below must call `EXPECT_TRUE(engine.book().verify_invariants(&err)) << err;` upon completion:

1. `EmptyBookBuyResting`: Single Buy Limit order enters empty book; becomes Best Bid; 0 trades; status `New`.
2. `EmptyBookSellResting`: Single Sell Limit order enters empty book; becomes Best Ask; 0 trades; status `New`.
3. `IncompatibleOrdersDoNotMatch`: Buy `100 @ 10000` and Sell `100 @ 10050` both rest; spread is `50`.
4. `ExactCrossCompleteFill`: Buy `100 @ 10000` followed by Sell `100 @ 10000` executes 1 trade of `100 @ 10000`; book becomes empty; both orders `Filled`.
5. `ExecutionAtRestingOrderPrice`:
   * Case A: Resting Sell `100 @ 10000`, Incoming Buy `100 @ 10500` $\rightarrow$ Executes at `10000`.
   * Case B: Resting Buy `100 @ 10500`, Incoming Sell `100 @ 10000` $\rightarrow$ Executes at `10500`.
6. `PricePriorityEnforcement`: Resting Sells at `10100`, `9900`, `10000` submitted out of price order; Incoming Buy `250 @ 10100` matches `9900` first, then `10000`, then `10100`.
7. `TimePriorityFIFOEnforcement`: Three Buy Limit orders at the exact same price `10000` (`#1` for `50`, `#2` for `50`, `#3` for `50`). Incoming Sell `120 @ 10000` completely fills `#1`, completely fills `#2`, and partially fills `20` of `#3` (leaving `#3` resting with `30`).
8. `PartialFillPassiveResting`: Resting Sell `100 @ 10000`, Incoming Buy `40 @ 10000` $\rightarrow$ Trade of `40 @ 10000`, resting Sell remains at front of queue with `remaining_quantity = 60`.
9. `PartialFillAggressiveRestsRemainder`: Resting Sell `40 @ 10000`, Incoming Buy `100 @ 10000` $\rightarrow$ Trade of `40 @ 10000`, Ask book becomes empty, remaining `60` of Incoming Buy rests on Bid book at `10000` with status `PartiallyFilled`.
10. `MultiLevelSweepSingleOrder`: Five Ask levels (`10000` to `10040`, `50` units each). Single incoming Buy `220 @ 10050` generates 5 trades across the levels, removes the first 4 levels, and leaves `30` units at `10040`.
11. `CancelExistingRestingOrder`: Submit 3 orders at same price level; cancel the middle order; verify `total_volume` decreases, queue preserves FIFO between first and third order, and cancelling the remaining orders deletes the `PriceLevel` from the map.
12. `CancelNonExistentOrAlreadyFilledOrder`: Cancelling unknown ID `9999` or an already-filled Order ID returns `success = false` without mutating book state.
13. `MarketOrderFullFillAcrossLevels`: Resting Asks `50 @ 10000` and `50 @ 10100`. Incoming Market Buy for `80` fills `50 @ 10000` and `30 @ 10100`.
14. `MarketOrderInsufficientLiquidityCancelsRemainder`: Resting Ask `40 @ 10000`. Incoming Market Buy for `100` executes `40 @ 10000` and immediately cancels the remaining `60` (status `PartiallyFilled`, book becomes completely empty, Market order never rests).
15. `MarketOrderOnEmptyBookCancelsImmediately`: Market Buy or Market Sell on an empty opposing book generates `0` trades, returns `status = Cancelled`, and leaves book empty.
16. `ValidationRejectsInvalidOrders`: Tests `quantity == 0`, Limit `price <= 0`, `id == 0`, and duplicate active `OrderId` $\rightarrow$ all return `OrderStatus::Rejected` and leave book untouched.
17. `QuantityReconciliationAudit`: Runs 1,000 randomized valid commands (submits + cancels) and verifies that for every submitted order:
    $$\text{initial\_quantity} = \text{cumulative\_traded\_quantity} + \text{cancelled\_quantity} + \text{currently\_resting\_quantity}$$
18. `DeterministicReplayTest`: Runs the exact same 5,000-command workload twice on two independent `MatchingEngine` instances and verifies byte-for-byte identical trade vectors and final book snapshots.

---

## 10. Repository Structure

~~~text
project-atlas/
├── CMakeLists.txt                          # Root CMake build definition (C++20, FetchContent for GTest, httplib, json)
├── README.md                               # User documentation, build instructions, interview talking points
├── documents.md                            # This master architecture specification
├── PROGRESS.md                             # Codex autonomous execution tracker
├── .vscode/
│   ├── settings.json                       # CMake & C++20 IntelliSense settings
│   ├── tasks.json                          # One-click Build, Test, Benchmark, and Run Server tasks
│   └── launch.json                         # Debug configurations for atlas_server, atlas_tests, atlas_bench
├── include/
│   └── atlas/
│       ├── core/
│       │   ├── types.hpp                   # Fixed-point aliases, Side, OrderType, OrderStatus enums
│       │   ├── order.hpp                   # OrderRequest, Order, PriceLevel, OrderLocation structs
│       │   ├── trade.hpp                   # Trade, ActivityRecord, OrderResult, CancelResult, BookSnapshot
│       │   ├── order_book.hpp              # MapListOrderBook declaration & verify_invariants()
│       │   ├── vector_order_book.hpp       # VectorOrderBook baseline for benchmark comparison
│       │   └── matching_engine.hpp         # MatchingEngine class declaration
│       ├── bench/
│       │   ├── workload_generator.hpp      # 7 deterministic synthetic workload generators
│       │   └── benchmark_runner.hpp        # Latency/throughput profiler & CLI/JSON formatter
│       └── server/
│           └── http_server.hpp             # Mutex-guarded REST gateway wrapping MatchingEngine
├── src/
│   ├── core/
│   │   ├── order_book.cpp                  # MapListOrderBook implementation
│   │   ├── vector_order_book.cpp           # VectorOrderBook baseline implementation
│   │   └── matching_engine.cpp             # MatchingEngine state machine & history logs
│   ├── bench/
│   │   ├── workload_generator.cpp          # Deterministic PRNG workload creation
│   │   ├── benchmark_runner.cpp            # Timed hot loops & percentile calculators
│   │   └── main_bench.cpp                  # Standalone CLI benchmark executable entry point
│   └── server/
│       ├── http_server.cpp                 # REST routes + embedded fallback UI
│       └── main_server.cpp                 # Main application entry point (starts server on :8080)
├── tests/
│   └── test_matching_engine.cpp            # All 18 GoogleTest unit, invariant, and reconciliation tests
└── web/
    └── index.html                          # Single-page visual dashboard (Order Book, Trade Feed, Controls, Benchmarks)
~~~

---

## 11. Implementation Roadmap

Implementation is divided into **6 sequential phases**. Each phase has concrete acceptance criteria and verification commands.

### Phase 1: Project Scaffolding, CMake Setup & Core Domain Types
* **Objective:** Configure CMake for C++20 with `FetchContent` (`googletest`, `cpp-httplib`, `nlohmann/json`), create VS Code configuration files, and define all fundamental header types.
* **Components:** `CMakeLists.txt`, `.vscode/*`, `include/atlas/core/types.hpp`, `order.hpp`, `trade.hpp`.
* **Dependencies:** None.
* **Acceptance Criteria:** CMake configures cleanly without errors and compiles a minimal target with zero warnings.

### Phase 2: Primary Order Book (`MapListOrderBook`) & Invariant Auditor
* **Objective:** Implement `OrderBook` (`bids_`, `asks_`, `order_lookup_`) supporting best-price queries, resting insertions, $\mathcal{O}(1)$ iterator cancellations, snapshot extraction, and the 8-point `verify_invariants()` diagnostic checker.
* **Components:** `include/atlas/core/order_book.hpp`, `src/core/order_book.cpp`.
* **Dependencies:** Phase 1.
* **Acceptance Criteria:** `OrderBook` compiles cleanly inside `atlas_core` and passes `verify_invariants()` across insertions and cancellations.

### Phase 3: Deterministic Matching Engine & GoogleTest Verification Suite
* **Objective:** Implement `MatchingEngine` (order validation, price-time priority matching loop, maker-price execution, partial fills, multi-level sweeps, market-order IOC remainder cancellation, trade & activity ring buffers) and implement all 18 GoogleTest scenarios.
* **Components:** `include/atlas/core/matching_engine.hpp`, `src/core/matching_engine.cpp`, `tests/test_matching_engine.cpp`.
* **Dependencies:** Phase 2.
* **Acceptance Criteria:** `ctest --output-on-failure` passes 18/18 tests with `verify_invariants()` succeeding after every single test case.

### Phase 4: Synthetic Workloads, Baseline `VectorOrderBook` & Benchmark Runner
* **Objective:** Implement the 7 deterministic synthetic workloads, the baseline `VectorOrderBook`, and `BenchmarkRunner` measuring throughput and nanosecond latency percentiles ($\text{Mean}, p_{50}, p_{95}, p_{99}, p_{99.9}$). Build `atlas_bench` CLI executable.
* **Components:** `include/atlas/core/vector_order_book.hpp`, `src/core/vector_order_book.cpp`, `include/atlas/bench/*`, `src/bench/*`.
* **Dependencies:** Phase 3.
* **Acceptance Criteria:** Running `atlas_bench` executes all 7 profiles and prints a clean ASCII performance table comparing `MapListOrderBook` and `VectorOrderBook`.

### Phase 5: HTTP Gateway & Interactive Visual Interface (`web/index.html`)
* **Objective:** Implement the `cpp-httplib` REST gateway (`GET /api/state`, `POST /api/orders`, `DELETE /api/orders/:id`, `POST /api/sim/step`, `POST /api/reset`, `POST /api/benchmark`) and build the responsive dark-mode `web/index.html` dashboard displaying the two-sided Order Book ladder with depth bars, Market Info cards, Order Entry form, Resting Orders cancellation table, Live Trade Feed, Activity Log, Simulation Mode auto-stepper, and Interactive Benchmark modal.
* **Components:** `include/atlas/server/http_server.hpp`, `src/server/http_server.cpp`, `src/server/main_server.cpp`, `web/index.html`.
* **Dependencies:** Phase 4.
* **Acceptance Criteria:** `atlas_server` compiles, passes API smoke tests, and serves the interactive UI at `http://localhost:8080`.

### Phase 6: End-to-End Integration, Polish & Interview Documentation (`README.md`)
* **Objective:** Run full regression tests, verify zero compiler warnings, run `atlas_bench` to record real empirical metrics, and write a comprehensive `README.md` including quick-start instructions, architecture walk-through, and an "Interview Defense Guide" explaining every design decision.
* **Components:** `README.md`, `PROGRESS.md`.
* **Dependencies:** Phase 5.
* **Acceptance Criteria:** All 3 executables (`atlas_tests`, `atlas_bench`, `atlas_server`) build cleanly in `Release` mode, 100% of tests pass, and `PROGRESS.md` marks all phases `[COMPLETED]`.

---

## 12. Codex Autonomous Implementation Prompt & Self-Loop Protocol

Copy and paste the prompt block below directly into **Codex** (or instruct Codex to read `documents.md` and execute Section 12). It is engineered so Codex operates in a continuous, self-verifying loop without stopping to ask for human input.

~~~text
SYSTEM DIRECTIVE FOR CODEX: AUTONOMOUS CONTINUOUS IMPLEMENTATION LOOP
Project: PROJECT ATLAS — Miniature Exchange Matching Engine (C++20)
Specification Source of Truth: ./documents.md

You are an autonomous Staff C++ Systems Engineer. Your task is to implement the entirety of Project Atlas from start to finish in this workspace by strictly following the architecture, data models, algorithms, API schemas, benchmark specifications, and test suite defined in `./documents.md`.

### CRITICAL AUTONOMOUS EXECUTION RULES (NO HUMAN INTERVENTION)
1. DO NOT STOP TO ASK FOR PERMISSION OR CLARIFICATION. All architectural, data-structure, algorithmic, and UI decisions have already been made in `./documents.md`.
2. CONTINUOUS SELF-LOOP PROTOCOL:
   You must execute Phases 1 through 6 sequentially in a continuous self-driving loop:
   - Step A: Read `./documents.md` and check/update `./PROGRESS.md`.
   - Step B: Write all files required for the current Phase. Never leave `TODO` placeholders, stubbed functions, or truncated code.
   - Step C: Run the compiler and verification command for the current Phase.
   - Step D: If any compilation error, linker error, warning, or test failure occurs, diagnose the root cause immediately, fix the code, and re-run verification until it passes 100%.
   - Step E: Update `./PROGRESS.md` marking the current Phase as `[COMPLETED]` with the verification output, and immediately proceed to the next Phase in the same run (or next tool invocation) without waiting for user input.
3. WINDOWS & CROSS-PLATFORM TOOLCHAIN DETECTION:
   - At the start of Phase 1, probe available C++ compilers and build tools (`cmake --version`, `cl`, `g++`, `clang++`, `ninja`).
   - Configure CMake in `./build` using `cmake -B build -DCMAKE_BUILD_TYPE=Release` (or `-G "Ninja"` / Visual Studio generator depending on what is installed in the environment).
   - Ensure on Windows that `ws2_32` (`Ws2_32.lib`) is linked to `atlas_server` in `CMakeLists.txt` via `if(WIN32) target_link_libraries(atlas_server PRIVATE ws2_32) endif()` so `cpp-httplib` compiles and links cleanly on Windows.

### EXACT SPECIFICATIONS TO IMPLEMENT PER PHASE

#### PHASE 1: Scaffolding, CMake & Domain Headers
1. Create `./PROGRESS.md` with a checklist of Phases 1 through 6.
2. Create `./CMakeLists.txt`:
   - `cmake_minimum_required(VERSION 3.20)`
   - `project(ProjectAtlas VERSION 1.0.0 LANGUAGES CXX)`
   - Set `CMAKE_CXX_STANDARD 20`, `CMAKE_CXX_STANDARD_REQUIRED ON`, `CMAKE_CXX_EXTENSIONS OFF`.
   - Use `FetchContent` to fetch:
     * `googletest` (`https://github.com/google/googletest.git`, tag `v1.14.0`, set `gtest_force_shared_crt ON` for Windows MSVC compatibility).
     * `httplib` (`https://github.com/yhirose/cpp-httplib.git`, tag `v0.15.3`).
     * `nlohmann_json` (`https://github.com/nlohmann/json.git`, tag `v3.11.3`).
   - Define targets:
     * `atlas_core` (STATIC library: `src/core/order_book.cpp`, `src/core/vector_order_book.cpp`, `src/core/matching_engine.cpp`, `src/bench/workload_generator.cpp`, `src/bench/benchmark_runner.cpp`).
     * `atlas_tests` (EXECUTABLE: `tests/test_matching_engine.cpp`, linked with `atlas_core` and `GTest::gtest_main`).
     * `atlas_bench` (EXECUTABLE: `src/bench/main_bench.cpp`, linked with `atlas_core`).
     * `atlas_server` (EXECUTABLE: `src/server/http_server.cpp`, `src/server/main_server.cpp`, linked with `atlas_core`, `httplib::httplib`, `nlohmann_json::nlohmann_json`, and `ws2_32` on `WIN32`).
3. Create `.vscode/settings.json`, `.vscode/tasks.json`, and `.vscode/launch.json` configured for CMake build, CTest execution, running `atlas_bench`, and launching `atlas_server`.
4. Implement `include/atlas/core/types.hpp`, `include/atlas/core/order.hpp`, and `include/atlas/core/trade.hpp` exactly as specified in Section 5 of `documents.md`, including helper string converters (`to_string(Side)`, `to_string(OrderType)`, `to_string(OrderStatus)`, `to_string(ActionType)`).

#### PHASE 2: Primary OrderBook (`MapListOrderBook`) & Invariants
1. Implement `include/atlas/core/order_book.hpp` and `src/core/order_book.cpp`:
   - `bids_`: `std::map<Price, PriceLevel, std::greater<Price>>`
   - `asks_`: `std::map<Price, PriceLevel, std::less<Price>>`
   - `order_lookup_`: `std::unordered_map<OrderId, OrderLocation>`
   - Provide methods for inspecting `best_bid()`, `best_ask()`, `spread()`, `has_order(OrderId)`, `get_order(OrderId)`, `active_order_count()`, `bid_level_count()`, `ask_level_count()`, `insert_resting_order(const Order&)`, `cancel_order(OrderId)`, `match_against_book(Order& incoming, SequenceNumber& seq_counter, TradeId& trade_id_counter, TimestampNs now, std::vector<Trade>& out_trades)`, `get_snapshot(std::size_t depth = 20) const`, `clear()`, and `verify_invariants(std::string* error_out = nullptr) const`.
2. Ensure `verify_invariants()` rigorously checks all 8 invariants defined in Section 9.1 of `documents.md`.

#### PHASE 3: MatchingEngine & Complete GoogleTest Suite
1. Implement `include/atlas/core/matching_engine.hpp` and `src/core/matching_engine.cpp`:
   - Owns `OrderBook book_`, monotonic `next_sequence_{0}`, `next_trade_id_{0}`, `next_auto_order_id_{1}`, `EngineStats stats_`, bounded `std::deque<Trade> recent_trades_` (cap 200), and bounded `std::deque<ActivityRecord> recent_activity_` (cap 200).
   - Implements `submit_order(OrderRequest req)`, `cancel_order(OrderId id)`, `get_snapshot(std::size_t depth = 20) const`, `stats() const`, `recent_trades(std::size_t limit = 50) const`, `recent_activity(std::size_t limit = 50) const`, `reset()`, and `seed_demo_book()` (which populates a realistic initial 5-level Bid and 5-level Ask ladder around $100.00 / `10000` ticks so the UI immediately looks alive when opened or seeded).
2. Implement `tests/test_matching_engine.cpp` containing all 18 tests specified in Section 9.2 of `documents.md`.
3. Compile and run `ctest --output-on-failure`. Do not proceed to Phase 4 until all 18 tests pass 100%.

#### PHASE 4: Baseline `VectorOrderBook`, Workload Generator & Benchmark Suite
1. Implement `include/atlas/core/vector_order_book.hpp` and `src/core/vector_order_book.cpp`:
   - Implements a contiguous `std::vector<VectorPriceLevel>` order book where each level holds a `std::vector<Order>` and cancellations scan for the `OrderId` and erase from the vector.
   - Verify it produces identical trade counts and final book volumes on deterministic workloads as `MapListOrderBook`.
2. Implement `include/atlas/bench/workload_generator.hpp` and `src/bench/workload_generator.cpp`:
   - Generates all 7 deterministic workload profiles (`BALANCED`, `BUY_HEAVY`, `SELL_HEAVY`, `AGGRESSIVE_MATCHING`, `CANCEL_HEAVY`, `CONCENTRATED_LEVELS`, `WIDE_DISTRIBUTION`) using `std::mt19937_64(42)` into a pre-allocated `std::vector<WorkloadCommand>` prior to timing.
3. Implement `include/atlas/bench/benchmark_runner.hpp`, `src/bench/benchmark_runner.cpp`, and `src/bench/main_bench.cpp`:
   - Runs timed hot loops for both `MapListOrderBook` and `VectorOrderBook`, computing throughput (orders/sec) and latency percentiles (`avg`, `p50`, `p95`, `p99`, `p99.9` in nanoseconds).
   - `atlas_bench` CLI must support `--orders <N>` (default `50000` for quick runs, `100000` for full runs) and print a formatted ASCII table to stdout.
4. Build and run `atlas_bench --orders 25000` to verify accuracy and performance output.

#### PHASE 5: HTTP Gateway (`atlas_server`) & Visual Interface (`web/index.html`)
1. Implement `include/atlas/server/http_server.hpp`, `src/server/http_server.cpp`, and `src/server/main_server.cpp`:
   - Guard all engine access with `std::mutex engine_mutex_`.
   - Implement all 6 REST endpoints from Section 7.1 of `documents.md`:
     * `GET /api/state`
     * `POST /api/orders`
     * `DELETE /api/orders/:id`
     * `POST /api/sim/step`
     * `POST /api/reset`
     * `POST /api/benchmark`
   - Serve `web/index.html` on `GET /` (searching `./web/index.html`, `../web/index.html`, and falling back to an embedded HTML string if needed).
   - Seed the initial demo order book on server startup so opening the browser immediately displays active Bids and Asks.
2. Create `web/index.html` (and sync the fallback if used):
   - Clean, modern institutional dark-slate financial UI using Tailwind CSS CDN and vanilla JavaScript.
   - Must display:
     * **Top Telemetry Bar:** Best Bid, Best Ask, Bid-Ask Spread (both `$` and ticks), Last Traded Price & Qty, Total Volume Traded, Active Orders Count, and Engine Status indicator.
     * **Left Column — Order Entry & Simulation Controls:**
       - Side selector (`BUY` / `SELL`), Order Type (`LIMIT` / `MARKET`), Price input (with quick `$100.00` $\leftrightarrow$ `10000 ticks` helper), Quantity input, Submit button.
       - Simulation Controls: "Seed Demo Book", "Step +1 Order", "Step +10 Orders", "Auto-Sim Toggle (Play/Pause at 5 orders/sec)", and "Reset Empty Book".
     * **Center Column — Order Book Ladder & Active Resting Orders:**
       - Two-sided Order Book display (Bids on left sorted highest-first with green depth volume bars; Asks on right sorted lowest-first with red depth volume bars; Best Bid / Best Ask highlighted clearly).
       - Interactive **Active Resting Orders Queue** table showing individual resting orders (`Order ID`, `Side`, `Price`, `Remaining Qty`, `FIFO Seq #`) with a one-click **[Cancel]** button on every row so an interviewer can watch individual FIFO queue items and cancel any specific order!
     * **Right Column — Live Trade Feed & Order Activity Log:**
       - Real-time scrolling Trade Feed (`Trade ID`, `Price`, `Qty`, `Buy ID`, `Sell ID`, `Aggressor Side`).
       - Real-time Order Activity Log (`Seq #`, `Action: SUBMIT/TRADE/CANCEL/REJECT`, `Status`, `Details`).
     * **Bottom Panel — Engine Benchmark Suite:**
       - Workload selector (`All 7 Profiles` or individual profile), Order count selector (`10,000`, `50,000`, `100,000`), "Compare vs. VectorOrderBook Baseline" toggle, and **[Run C++ Benchmark]** button.
       - Renders the benchmark results table (`Workload`, `Implementation`, `Throughput ops/s`, `Avg ns`, `p50 ns`, `p95 ns`, `p99 ns`, `p99.9 ns`) with visual throughput comparison bars.

#### PHASE 6: End-to-End Verification & Documentation (`README.md`)
1. Write a comprehensive `README.md` containing:
   - Project overview & architecture diagram.
   - Exact build & run commands for Windows / VS Code (`atlas_server`, `atlas_tests`, `atlas_bench`).
   - Step-by-step **8-Step Live Interview Demonstration Script** matching the user's demonstration goals.
   - **Engineering Q&A / Interview Defense Guide** explaining why fixed-point ticks were chosen, why `std::map` + `std::list` + `std::unordered_map` achieves $\mathcal{O}(1)$ cancellation, how price-time priority and partial fills work, and what the benchmark results between `MapListOrderBook` and `VectorOrderBook` prove.
2. Run a final build of all targets, execute `ctest --output-on-failure`, run `atlas_bench`, verify zero errors, and mark all phases `[COMPLETED]` in `PROGRESS.md`.

BEGIN AUTONOMOUS EXECUTION IMMEDIATELY AT PHASE 1.
~~~