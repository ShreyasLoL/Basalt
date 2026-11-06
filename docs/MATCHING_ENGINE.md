# Matching engine evolution

`MatchingEngine` remains the compatibility single-book API. `MultiInstrumentEngine` adds an isolated price-time book per positive `InstrumentId`; order identifiers are unique across all active books, and trades carry the instrument, account and participant for both sides.

## Order handling

`AdvancedOrderType` accepts `LIMIT`, `MARKET`, `IOC`, `FOK`, `POST_ONLY`, `STOP_MARKET` and `STOP_LIMIT`. A market order sweeps all available levels and cancels its remainder. IOC and FOK are price-capped limit orders. IOC cancels unfilled quantity. FOK checks eligible displayed quantity, price and self-trade policy before changing any book or sequence; a failed pre-check returns sequence zero and does not allocate a trade ID. Post-only rejects any order that would take liquidity. Stop orders stay outside the continuous book until a reference trade for the same instrument reaches the trigger; stop-market then sweeps, while stop-limit joins/matches at its declared limit.

Every match executes at the resting order's price, consumes best price then FIFO, assigns one deterministic trade ID and sequence, and records instrument/account/participant identities. Cross-instrument liquidity is never considered. `verify_invariants` checks price crossing, per-level FIFO, active-order lookup consistency, instrument ownership, and stop registry validity.

## Amendments and self-trade prevention

Only active resting orders can be amended. Quantity decreases that leave a positive remainder at the same price preserve sequence priority. A price change or total-quantity increase removes and appends the order at the new level's tail with a new sequence. A change that would cross the opposite book is rejected without mutation. Modified total quantity includes already-filled units.

The incoming order's STP policy applies when account IDs or participant IDs match (empty identities do not match):

- `CANCEL_NEWEST`: keep the resting order and cancel the incoming remainder.
- `CANCEL_OLDEST`: remove the resting order and continue processing the incoming order.
- `CANCEL_BOTH`: remove the resting order and cancel the incoming remainder.

These are simulation engine semantics; this component has no broker route. See `tests/test_multi_instrument_engine.cpp` for order, priority, STP, stop trigger, FOK atomicity and instrument-isolation fixtures.
