(() => {
    const $ = (id) => document.getElementById(id);
    const fmtPrice = (ticks) => ticks == null ? '—' : `$${(ticks / 100).toFixed(2)}`;
    const fmtNumber = (value) => Number(value || 0).toLocaleString();
    let autoTimer = null;
    let simCount = 0;
    let lastTradeId = 0;

    function setConnected(connected) {
      $('connectionStatus').textContent = connected ? 'Connected' : 'Reconnecting';
      $('statusDot').className = `h-2 w-2 rounded-full ${connected ? 'bg-mint' : 'bg-amber-400'}`;
    }
    function setMessage(text, good = true) {
      const node = $('orderMessage');
      node.textContent = text;
      node.className = `mt-3 rounded-lg border p-2.5 text-xs ${good ? 'border-mint/30 text-mint bg-mint/5' : 'border-coral/30 text-coral bg-coral/5'}`;
      node.classList.remove('hidden');
      setTimeout(() => node.classList.add('hidden'), 5000);
    }
    async function api(path, options = {}) {
      const response = await fetch(path, { headers: { 'Content-Type': 'application/json' }, ...options });
      const data = await response.json().catch(() => ({}));
      if (!response.ok) throw new Error(data.message || data.error || `HTTP ${response.status}`);
      return data;
    }
    function renderLadder(target, levels, side, bestPrice) {
      const root = $(target); root.replaceChildren();
      const max = Math.max(1, ...levels.map((level) => level.total_volume));
      levels.forEach((level) => {
        const row = document.createElement('div');
        row.className = 'depth-cell grid grid-cols-[1fr_auto] gap-2 px-2 py-1.5 rounded text-xs metric';
        const bar = document.createElement('span');
        bar.className = `depth-bar ${side === 'BUY' ? 'bg-mint right-0' : 'bg-coral left-0'}`;
        bar.style.width = `${Math.max(3, level.total_volume / max * 100)}%`;
        const price = document.createElement('span');
        price.className = `${side === 'BUY' ? 'text-mint' : 'text-coral'} ${level.price === bestPrice ? 'font-bold' : ''}`;
        price.textContent = `${fmtPrice(level.price)}  ·  ${level.price}`;
        const qty = document.createElement('span'); qty.className = 'text-slate-300';
        qty.textContent = `${fmtNumber(level.total_volume)} / ${level.order_count}`;
        row.append(bar, price, qty); root.append(row);
      });
      if (!levels.length) { const empty = document.createElement('div'); empty.className = 'py-3 text-xs text-slate-600'; empty.textContent = 'No resting liquidity'; root.append(empty); }
    }
    function renderOrders(orders) {
      const body = $('ordersTable'); body.replaceChildren();
      $('queueCount').textContent = `(${orders.length})`;
      orders.forEach((order) => {
        const row = document.createElement('tr'); row.className = 'border-b border-line/60 hover:bg-white/[.025]';
        const values = [order.id, order.side, `${fmtPrice(order.price)} · ${order.price}`, fmtNumber(order.remaining_quantity), order.sequence];
        values.forEach((value, index) => {
          const cell = document.createElement('td'); cell.className = `py-2 ${index === 1 ? (order.side === 'BUY' ? 'text-mint' : 'text-coral') : 'text-slate-300'} metric`;
          cell.textContent = String(value); row.append(cell);
        });
        const action = document.createElement('td'); action.className = 'py-1 text-right';
        const cancel = document.createElement('button'); cancel.className = 'rounded border border-coral/30 px-2 py-1 text-[10px] text-coral hover:bg-coral/10'; cancel.textContent = 'Cancel';
        cancel.addEventListener('click', async () => { try { await api(`/api/orders/${order.id}`, { method: 'DELETE' }); await refresh(); } catch (error) { setMessage(error.message, false); } });
        action.append(cancel); row.append(action); body.append(row);
      });
    }
    function renderTrades(trades) {
      const root = $('tradeFeed'); root.replaceChildren();
      $('tradeFeedCount').textContent = `${trades.length} records`;
      trades.forEach((trade) => {
        const card = document.createElement('div'); card.className = 'rounded-lg border border-line/70 bg-slate-950/30 p-2.5 text-[11px]';
        const top = document.createElement('div'); top.className = 'flex justify-between items-center';
        const id = document.createElement('span'); id.className = 'text-slate-500 metric'; id.textContent = `#${trade.trade_id} · seq ${trade.sequence}`;
        const side = document.createElement('span'); side.className = trade.aggressor_side === 'BUY' ? 'text-mint font-semibold' : 'text-coral font-semibold'; side.textContent = `${trade.aggressor_side} aggressor`;
        const detail = document.createElement('div'); detail.className = 'mt-1.5 flex justify-between text-slate-300 metric';
        const px = document.createElement('span'); px.textContent = `${fmtPrice(trade.price)} × ${fmtNumber(trade.quantity)}`;
        const ids = document.createElement('span'); ids.className = 'text-slate-500'; ids.textContent = `B${trade.buy_order_id} / S${trade.sell_order_id}`;
        top.append(id, side); detail.append(px, ids); card.append(top, detail);
        if (trade.trade_id > lastTradeId) { card.classList.add('trade-flash'); lastTradeId = trade.trade_id; }
        root.append(card);
      });
      if (!trades.length) { const empty = document.createElement('div'); empty.className = 'py-4 text-xs text-slate-600'; empty.textContent = 'Trades will appear here as orders cross.'; root.append(empty); }
    }
    function renderActivity(records) {
      const root = $('activityLog'); root.replaceChildren();
      records.forEach((record) => {
        const row = document.createElement('div'); row.className = 'grid grid-cols-[48px_48px_1fr] gap-2 rounded-md px-2 py-1.5 bg-slate-950/25 text-[10px]';
        const seq = document.createElement('span'); seq.className = 'metric text-slate-500'; seq.textContent = record.sequence;
        const action = document.createElement('span'); action.className = record.action === 'REJECT' ? 'text-coral' : record.action === 'TRADE' ? 'text-blueglow' : record.action === 'CANCEL' ? 'text-amber-300' : 'text-slate-300'; action.textContent = record.action;
        const detail = document.createElement('span'); detail.className = 'text-slate-400 truncate'; detail.title = record.message; detail.textContent = record.message;
        row.append(seq, action, detail); root.append(row);
      });
      if (!records.length) { const empty = document.createElement('div'); empty.className = 'py-3 text-xs text-slate-600'; empty.textContent = 'No engine events yet.'; root.append(empty); }
    }
    function renderState(state) {
      const m = state.market; const b = state.book;
      $('bestBid').textContent = m.has_best_bid ? fmtPrice(m.best_bid) : '—';
      $('bestBidQty').textContent = m.has_best_bid ? `${fmtNumber(m.best_bid_volume)} units · ${m.best_bid} ticks` : 'No bid liquidity';
      $('bestAsk').textContent = m.has_best_ask ? fmtPrice(m.best_ask) : '—';
      $('bestAskQty').textContent = m.has_best_ask ? `${fmtNumber(m.best_ask_volume)} units · ${m.best_ask} ticks` : 'No ask liquidity';
      $('spread').textContent = m.has_spread ? fmtPrice(m.spread) : '—';
      $('spreadTicks').textContent = m.has_spread ? `${m.spread} ticks` : 'Spread unavailable';
      $('lastTrade').textContent = m.has_last_trade_price ? fmtPrice(m.last_trade_price) : '—';
      $('lastTradeQty').textContent = m.has_last_trade_price ? `${fmtNumber(m.last_trade_quantity)} units · ${m.last_trade_price} ticks` : 'No executions yet';
      $('volumeTraded').textContent = fmtNumber(m.total_volume_traded);
      $('tradeCount').textContent = `${fmtNumber(m.total_trades_executed)} trades`;
      $('activeOrders').textContent = fmtNumber(m.active_order_count);
      $('levelCounts').textContent = `${m.bid_level_count} bid / ${m.ask_level_count} ask levels`;
      renderLadder('bidLadder', b.bids, 'BUY', m.best_bid);
      renderLadder('askLadder', b.asks, 'SELL', m.best_ask);
      renderOrders(b.resting_orders);
      renderTrades(state.trades);
      renderActivity(state.activity);
    }
    async function refresh() {
      try { const state = await api('/api/state?depth=12'); renderState(state); setConnected(true); }
      catch (error) { setConnected(false); }
    }
    async function step(count) {
      const result = await api('/api/sim/step', { method: 'POST', body: JSON.stringify({ count }) });
      simCount += result.executed; $('simCount').textContent = fmtNumber(simCount); await refresh();
    }

    $('priceTicks').addEventListener('input', () => { const value = Number($('priceTicks').value); if (Number.isFinite(value)) $('priceDollars').value = (value / 100).toFixed(2); });
    $('priceDollars').addEventListener('input', () => { const value = Number($('priceDollars').value); if (Number.isFinite(value)) $('priceTicks').value = String(Math.round(value * 100)); });
    $('orderType').addEventListener('change', () => { const market = $('orderType').value === 'MARKET'; $('priceTicks').disabled = market; $('priceDollars').disabled = market; });
    $('orderForm').addEventListener('submit', async (event) => {
      event.preventDefault();
      const type = $('orderType').value;
      const body = { side: $('orderSide').value, type, price: type === 'MARKET' ? 0 : Number($('priceTicks').value), quantity: Number($('orderQty').value) };
      try { const result = await api('/api/orders', { method: 'POST', body: JSON.stringify(body) }); setMessage(`Order #${result.order_id}: ${result.status} · filled ${fmtNumber(result.filled_quantity)} / ${fmtNumber(result.initial_quantity)}`); await refresh(); }
      catch (error) { setMessage(error.message, false); }
    });
    $('seedButton').addEventListener('click', async () => { try { await api('/api/reset', { method: 'POST', body: JSON.stringify({ seed_demo: true }) }); await refresh(); } catch (error) { setMessage(error.message, false); } });
    $('resetButton').addEventListener('click', async () => { try { await api('/api/reset', { method: 'POST', body: JSON.stringify({ seed_demo: false }) }); simCount = 0; $('simCount').textContent = '0'; await refresh(); } catch (error) { setMessage(error.message, false); } });
    $('stepOne').addEventListener('click', () => step(1).catch((error) => setMessage(error.message, false)));
    $('stepTen').addEventListener('click', () => step(10).catch((error) => setMessage(error.message, false)));
    $('autoButton').addEventListener('click', () => {
      if (autoTimer) { clearInterval(autoTimer); autoTimer = null; $('autoButton').textContent = '▶ Start auto-sim · 5 orders/sec'; $('autoButton').classList.remove('bg-mint/10'); }
      else { autoTimer = setInterval(() => step(1).catch((error) => setMessage(error.message, false)), 200); $('autoButton').textContent = 'Ⅱ Pause auto-sim'; $('autoButton').classList.add('bg-mint/10'); }
    });
    $('runBenchmark').addEventListener('click', async () => {
      const button = $('runBenchmark'); const status = $('benchmarkStatus'); button.disabled = true; button.textContent = 'Running…'; status.textContent = 'Running isolated C++ workloads. This can take a few seconds.'; status.classList.remove('hidden');
      try {
        const profile = $('benchProfile').value;
        const result = await api('/api/benchmark', { method: 'POST', body: JSON.stringify({ profile, order_count: Number($('benchOrders').value), compare_baseline: $('compareBaseline').checked }) });
        renderBenchmarks(result.results); status.textContent = `Completed ${result.results.length} benchmark runs.`;
      } catch (error) { status.textContent = error.message; }
      finally { button.disabled = false; button.textContent = 'Run C++ benchmark'; }
    });
    function renderBenchmarks(results) {
      const body = $('benchmarkRows'); body.replaceChildren();
      const peak = Math.max(1, ...results.map((result) => result.throughput_ops_per_sec));
      results.forEach((result) => {
        const row = document.createElement('tr'); row.className = 'border-b border-line/50';
        const workload = document.createElement('td'); workload.className = 'py-2.5 pr-3 text-slate-300'; workload.textContent = result.profile_name;
        const impl = document.createElement('td'); impl.className = 'pr-3 text-slate-400'; impl.textContent = result.implementation;
        const throughput = document.createElement('td'); throughput.className = 'pr-3 metric text-blueglow'; throughput.textContent = fmtNumber(Math.round(result.throughput_ops_per_sec));
        const bar = document.createElement('div'); bar.className = 'h-1 rounded bg-gradient-to-r from-blueglow to-mint mt-1'; bar.style.width = `${Math.max(2, result.throughput_ops_per_sec / peak * 100)}%`; throughput.append(bar);
        const values = [Math.round(result.avg_latency_ns), result.p50_latency_ns, result.p95_latency_ns, result.p99_latency_ns, result.p999_latency_ns];
        row.append(workload, impl, throughput);
        values.forEach((value) => { const cell = document.createElement('td'); cell.className = 'pr-3 metric text-slate-300'; cell.textContent = fmtNumber(value); row.append(cell); });
        body.append(row);
      });
    }
    function updateClock() { $('clock').textContent = new Date().toLocaleTimeString(); }
    updateClock(); setInterval(updateClock, 1000); refresh(); setInterval(refresh, 1000);
  })();