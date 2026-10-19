/* Standalone, deterministic browser workloads. No browser-specific APIs required. */
'use strict';
(() => {
  const query = new URLSearchParams(location.search);
  const stage = document.getElementById('stage');
  const progress = document.getElementById('progress');
  const button = document.getElementById('start');
  const rows = document.getElementById('resultRows');
  const engine = query.get('engine') || 'manual';
  const config = {
    width: Number(query.get('width') ?? (engine === 'manual' ? innerWidth : 1000)),
    height: Number(query.get('height') ?? (engine === 'manual' ? innerHeight : 700)),
    durationMs: Number(query.get('durationMs') ?? 2000),
    warmupMs: Number(query.get('warmupMs') ?? 500),
    samples: Number(query.get('samples') ?? 20),
  };
  const trial = Number(query.get('trial') ?? 0);
  const token = query.get('token');
  const environment = {
    userAgent: navigator.userAgent,
    viewport: { width: innerWidth, height: innerHeight },
    devicePixelRatio,
    visibilityState: document.visibilityState,
    hardwareConcurrency: navigator.hardwareConcurrency,
    screen: { width: screen.width, height: screen.height, availWidth: screen.availWidth, availHeight: screen.availHeight },
  };
  let promise;
  let active = false;
  let invalidation = '';
  let initialRatio;
  let result;
  document.getElementById('config').textContent = `${engine} · trial ${trial} · ${config.width}×${config.height} · ${config.durationMs} ms/scenario · ${config.samples} operation samples`;
  const assert = (condition, message) => { if (!condition) throw new Error(message); };
  function guard() {
    assert(!invalidation, invalidation);
    assert(document.visibilityState === 'visible', 'Document must remain visible');
    assert(innerWidth === config.width && innerHeight === config.height, `Viewport mismatch: requested ${config.width}×${config.height}, actual ${innerWidth}×${innerHeight}`);
    assert(devicePixelRatio === initialRatio, 'Device pixel ratio changed during benchmark');
  }
  addEventListener('resize', () => { if (active) invalidation = 'Viewport changed during benchmark'; });
  document.addEventListener('visibilitychange', () => { if (active && document.hidden) invalidation = 'Document became hidden during benchmark'; });
  const frame = () => new Promise(resolve => requestAnimationFrame(() => resolve(performance.now())));
  // A timer allows a hidden document to fail rather than hanging on suspended rAF.
  async function nextFrame() {
    let timer;
    try {
      const time = await Promise.race([frame(), new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error('No animation frame within 10 seconds; window may be hidden or suspended')), 10000);
      })]);
      guard();
      return time;
    } finally { clearTimeout(timer); }
  }
  async function settle() { await nextFrame(); await nextFrame(); }
  function percentile(values, fraction) {
    assert(values.length > 0 && values.every(Number.isFinite), 'Empty or nonfinite samples');
    const sorted = values.slice().sort((a, b) => a - b);
    const index = (sorted.length - 1) * fraction;
    const low = Math.floor(index);
    return sorted[low] + (sorted[Math.ceil(index)] - sorted[low]) * (index - low);
  }
  function timingMetrics(raw) {
    const metrics = {};
    for (const [key, prefix] of [['workMs', 'work'], ['opportunityMs', 'opportunity'], ['frameIntervalsMs', 'frame']]) {
      if (raw[key]) {
        metrics[`${prefix}P50Ms`] = percentile(raw[key], 0.5);
        metrics[`${prefix}P95Ms`] = percentile(raw[key], 0.95);
      }
    }
    if (raw.frameIntervalsMs) {
      const intervals = raw.frameIntervalsMs;
      const total = intervals.reduce((sum, value) => sum + value, 0);
      assert(total > 0, 'Nonpositive frame duration');
      metrics.rafFps = intervals.length * 1000 / total;
      metrics.over25MsPct = intervals.filter(value => value > 25).length * 100 / intervals.length;
    }
    assert(Object.values(metrics).every(Number.isFinite), 'Nonfinite metrics');
    return metrics;
  }
  function reset() { stage.replaceChildren(); stage.scrollTop = 0; }
  function makeRows(count) {
    const fragment = document.createDocumentFragment();
    for (let i = 0; i < count; i++) {
      const row = document.createElement('div');
      row.className = 'row';
      row.textContent = `Record ${i} · Project ${i % 37} · A deterministic rendering workload with realistic text · ${1000 + i}`;
      row.style.backgroundColor = i % 2 ? '#f0f5fb' : '#ffffff';
      fragment.appendChild(row);
    }
    return fragment;
  }
  async function operations(id, label, prepare, work, validate) {
    const raw = { workMs: [], opportunityMs: [] };
    let effect;
    // Warm-ups use the same operation, including layout, but are discarded.
    for (let sample = -3; sample < config.samples; sample++) {
      guard();
      prepare(sample + 3);
      await settle();
      const start = performance.now();
      const value = work(sample + 3);
      const end = performance.now();
      await settle();
      const opportunityEnd = performance.now();
      effect = validate(value, sample + 3);
      if (sample >= 0) {
        raw.workMs.push(end - start);
        raw.opportunityMs.push(opportunityEnd - start);
      }
    }
    return { id, label, kind: 'operation', metrics: timingMetrics(raw), raw, effect };
  }
  async function animation(id, label, setup, draw, validate) {
    reset();
    const state = setup();
    await settle();
    let index = 0;
    const warmStart = performance.now();
    do { await nextFrame(); draw(state, index++); } while (performance.now() - warmStart < config.warmupMs);
    const raw = { frameIntervalsMs: [], workMs: [] };
    let previous = await nextFrame();
    const start = previous;
    do {
      const now = await nextFrame();
      raw.frameIntervalsMs.push(now - previous);
      previous = now;
      const workStart = performance.now();
      draw(state, index++);
      raw.workMs.push(performance.now() - workStart);
    } while (previous - start < config.durationMs);
    guard();
    const effect = validate(state, index);
    return { id, label, kind: 'animation', metrics: timingMetrics(raw), raw, effect };
  }
  function showScenario(scenario) {
    const row = document.createElement('tr');
    for (const text of [scenario.label, Object.entries(scenario.metrics).map(([key, value]) => `${key}: ${value.toFixed(3)}`).join('\n'), JSON.stringify(scenario.effect)]) {
      const cell = document.createElement('td');
      const pre = document.createElement('pre');
      pre.textContent = text;
      cell.appendChild(pre);
      row.appendChild(cell);
    }
    rows.appendChild(row);
  }
  async function execute() {
    result = { schemaVersion: 1, status: 'error', engine, trial, config, environment, startedAt: new Date().toISOString(), scenarios: [] };
    window.renderBenchmark.result = result;
    button.disabled = true;
    try {
      assert(['mini-electron', 'chrome', 'manual'].includes(engine), 'Unknown engine');
      assert(Number.isInteger(trial) && trial >= 0, 'Trial must be a nonnegative integer');
      assert(Number.isInteger(config.width) && config.width > 0 && Number.isInteger(config.height) && config.height > 0, 'Invalid viewport dimensions');
      assert(Number.isFinite(config.durationMs) && config.durationMs > 0 && Number.isFinite(config.warmupMs) && config.warmupMs >= 0, 'Invalid timing configuration');
      assert(Number.isInteger(config.samples) && config.samples > 0, 'Samples must be a positive integer');
      // Chrome's automation host may resize before calling run().
      environment.viewport = { width: innerWidth, height: innerHeight };
      environment.devicePixelRatio = devicePixelRatio;
      environment.visibilityState = document.visibilityState;
      initialRatio = devicePixelRatio;
      active = true;
      guard();
      assert(stage.clientWidth > 100 && stage.clientHeight > 100, 'Viewport too small for measurement stage');
      window.scrollTo(0, 0);
      await settle();
      async function runScenario(label, run) {
        progress.textContent = label;
        reset();
        await settle();
        const scenario = await run();
        result.scenarios.push(scenario);
        showScenario(scenario);
      }
      await runScenario('1/6 · Empty rAF baseline', () => animation('raf-baseline', 'Empty rAF baseline', () => ({}), () => {}, () => {
        assert(stage.childElementCount === 0, 'Baseline stage must be empty');
        return { emptyStage: true };
      }));
      await runScenario('2/6 · DOM creation and layout', () => operations('dom-create', 'Create 2,000 text rows + layout', () => reset(), () => {
        stage.appendChild(makeRows(2000));
        return stage.lastElementChild.getBoundingClientRect().bottom;
      }, bottom => {
        assert(stage.childElementCount === 2000 && stage.lastElementChild.textContent.includes('Record 1999') && bottom > stage.getBoundingClientRect().bottom && stage.scrollHeight >= 60000, 'DOM creation/layout invariant failed');
        return { rows: 2000, contentHeight: stage.scrollHeight };
      }));
      let grid;
      let items;
      await runScenario('3/6 · Batched updates and grid layout', () => operations('layout-update', 'Update 1,000 grid items + layout', () => {
        reset();
        grid = document.createElement('div');
        grid.className = 'grid';
        items = Array.from({ length: 1000 }, (_, i) => {
          const item = document.createElement('div');
          item.className = 'item'; item.textContent = `Before ${i}`;
          grid.appendChild(item); return item;
        });
        stage.appendChild(grid);
      }, sample => {
        grid.style.gridTemplateColumns = `repeat(${sample % 2 ? 7 : 9}, minmax(0, 1fr))`;
        for (let i = 0; i < items.length; i++) {
          items[i].textContent = `Updated ${sample}:${i} · ${i % 19}`;
          items[i].style.padding = `${4 + (i + sample) % 3}px`;
          items[i].style.backgroundColor = (i + sample) % 2 ? '#bdd8f4' : '#f6d5ad';
        }
        return items[999].getBoundingClientRect().top;
      }, (top, sample) => {
        assert(grid.childElementCount === 1000 && items[999].textContent.startsWith(`Updated ${sample}:999`) && top > items[0].getBoundingClientRect().top && grid.offsetHeight > stage.clientHeight, 'Grid update/layout invariant failed');
        return { items: 1000, gridHeight: grid.offsetHeight, columns: sample % 2 ? 7 : 9 };
      }));
      await runScenario('4/6 · Canvas2D repaint', () => animation('canvas-paint', 'Canvas2D: 1,000 moving primitives', () => {
        const canvas = document.createElement('canvas');
        const width = stage.clientWidth, height = stage.clientHeight;
        canvas.width = Math.round(width * devicePixelRatio); canvas.height = Math.round(height * devicePixelRatio);
        canvas.style.width = `${width}px`; canvas.style.height = `${height}px`;
        stage.appendChild(canvas);
        const context = canvas.getContext('2d');
        assert(context, 'Canvas2D context unavailable');
        context.scale(devicePixelRatio, devicePixelRatio);
        return { canvas, context, width, height, last: -1 };
      }, (state, index) => {
        const { context: context, width, height } = state;
        context.fillStyle = index % 2 ? '#15273b' : '#233b55';
        context.fillRect(0, 0, width, height);
        for (let i = 0; i < 1000; i++) {
          context.fillStyle = `hsl(${(i * 47 + index * 3) % 360},70%,60%)`;
          context.fillRect((i * 37 + index * 3) % width, (i * 61 + index * 2) % height, 6 + i % 13, 5 + i % 11);
        }
        // A known corner marker permits an exact post-timing pixel check.
        context.fillStyle = index % 2 ? '#ff0000' : '#00ff00';
        context.fillRect(0, 0, 4, 4);
        state.last = index;
      }, state => {
        const pixel = state.context.getImageData(0, 0, 1, 1).data;
        const odd = state.last % 2;
        assert(pixel[0] === (odd ? 255 : 0) && pixel[1] === (odd ? 0 : 255) && pixel[3] === 255, 'Canvas pixel invariant failed');
        return { primitivesPerFrame: 1000, backingWidth: state.canvas.width, backingHeight: state.canvas.height, pixel: Array.from(pixel) };
      }));
      await runScenario('5/6 · Transform animation', () => animation('transform-animation', 'Transform 150 cards each frame', () => {
        const cards = [];
        for (let i = 0; i < 150; i++) {
          const card = document.createElement('div');
          card.className = 'card'; card.textContent = `Card ${i}`;
          card.style.backgroundColor = `hsl(${i * 47 % 360},65%,40%)`;
          stage.appendChild(card); cards.push(card);
        }
        return { cards, width: stage.clientWidth, height: stage.clientHeight, last: -1 };
      }, (state, index) => {
        for (let i = 0; i < state.cards.length; i++) {
          const x = (i * 67 + index * 2) % (state.width - 80);
          const y = (i * 41 + index * 3) % (state.height - 34);
          state.cards[i].style.transform = `translate(${x}px, ${y}px)`;
        }
        state.last = index;
      }, state => {
        const first = state.cards[0].getBoundingClientRect();
        const bounds = stage.getBoundingClientRect();
        const expectedX = state.last * 2 % (state.width - 80);
        assert(stage.childElementCount === 150 && Math.abs(first.left - bounds.left - stage.clientLeft - expectedX) < 1 && first.top >= bounds.top && first.bottom <= bounds.bottom, 'Transform position invariant failed');
        return { cards: 150, firstCardX: expectedX, lastFrame: state.last };
      }));
      await runScenario('6/6 · Programmatic scroll', () => animation('programmatic-scroll', 'Scroll 3,000 text rows', () => {
        const scroller = document.createElement('div');
        scroller.style.cssText = 'height:100%;overflow:auto;scroll-behavior:auto;overflow-anchor:none';
        scroller.appendChild(makeRows(3000)); stage.appendChild(scroller);
        const range = scroller.scrollHeight - scroller.clientHeight;
        assert(range > 1000 && scroller.childElementCount === 3000, 'Scroll content invariant failed');
        return { scroller, range, expected: 0, changed: 0, previous: 0 };
      }, (state, index) => {
        // Constant-speed jumps exercise visible new rows even on high-refresh displays.
        state.expected = ((index + 1) * 47) % state.range;
        state.scroller.scrollTop = state.expected;
        const actual = state.scroller.scrollTop;
        assert(Math.abs(actual - state.expected) <= 1, 'Programmatic scroll did not take effect');
        if (actual !== state.previous) state.changed++;
        state.previous = actual;
      }, state => {
        assert(state.changed > 1 && Math.abs(state.scroller.scrollTop - state.expected) <= 1 && state.scroller.lastElementChild.textContent.includes('Record 2999'), 'Scroll invariant failed');
        return { rows: 3000, scrollRange: state.range, finalScrollTop: state.scroller.scrollTop, changedFrames: state.changed };
      }));
      guard();
      result.status = 'complete';
      progress.textContent = 'Complete · results below';
    } catch (error) {
      result.status = 'error';
      result.error = error instanceof Error ? error.message : String(error);
      progress.textContent = `Error: ${result.error}`;
      progress.classList.add('error');
    } finally {
      active = false;
      window.renderBenchmark.result = result;
    }
    // Reporting never overlaps timed workload execution.
    if (token) {
      try {
        const response = await fetch(`/result?token=${encodeURIComponent(token)}`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(result) });
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
      } catch (error) {
        progress.textContent += ` · Result delivery failed: ${error.message}`;
        progress.classList.add('error');
      }
    }
    return result;
  }
  window.renderBenchmark = { run: () => { if (!promise) promise = execute(); return promise; }, result: null };
  button.addEventListener('click', () => { void window.renderBenchmark.run(); });
  if (query.get('autostart') === '1') {
    // Defer until load; run() performs its own settled-frame wait.
    if (document.readyState === 'complete') void window.renderBenchmark.run();
    else addEventListener('load', () => { void window.renderBenchmark.run(); }, { once: true });
  }
})();
