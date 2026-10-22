(function () {
  'use strict';

  var WORKLOAD_VERSION = 1;
  var PREPEND_COUNT = 100;
  var OPERATION_NAMES = [
    'mount',
    'update_selected',
    'keyed_reorder',
    'prepend',
    'filter',
    'restore',
    'unmount'
  ];

  function rowFor(id, order) {
    return {
      id: id,
      order: order,
      label: 'Record ' + id,
      group: 'Group ' + (id % 7),
      score: (Math.imul(id, 48271) + 17) % 10000,
      detail: 'Deterministic detail ' + id + ' / ' + ((id * 13) % 97)
    };
  }

  function makeRows(count, firstId) {
    var rows = new Array(count);
    for (var i = 0; i < count; ++i)
      rows[i] = rowFor(firstId + i, i);
    return rows;
  }

  function reorderRows(rows) {
    var length = rows.length;
    var reordered = new Array(length);
    if (!length)
      return reordered;
    for (var i = 0; i < length; ++i) {
      var source = i === 0 ? length - 1 : (i === length - 1 ? 0 : length - 1 - i);
      var row = rows[source];
      reordered[i] = {
        id: row.id,
        order: i,
        label: row.label,
        group: row.group,
        score: row.score,
        detail: row.detail
      };
    }
    return reordered;
  }

  function prependRows(rows, baseCount) {
    var additions = makeRows(PREPEND_COUNT, baseCount + 1);
    var result = new Array(additions.length + rows.length);
    var i;
    for (i = 0; i < additions.length; ++i) {
      additions[i].order = i;
      result[i] = additions[i];
    }
    for (i = 0; i < rows.length; ++i) {
      var row = rows[i];
      result[additions.length + i] = {
        id: row.id,
        order: additions.length + i,
        label: row.label,
        group: row.group,
        score: row.score,
        detail: row.detail
      };
    }
    return result;
  }

  function visibleRows(model) {
    var rows = model.filtered
      ? model.rows.filter(function (row) { return row.id % 10 === 0; })
      : model.rows.slice();
    rows.sort(function (left, right) { return left.order - right.order; });
    return rows;
  }

  function idHash(rows) {
    var hash = 2166136261;
    for (var i = 0; i < rows.length; ++i)
      hash = Math.imul((hash ^ rows[i].id) >>> 0, 16777619) >>> 0;
    return hash >>> 0;
  }

  function fail(message) {
    throw new Error('benchmark assertion: ' + message);
  }

  function validate(model, operation) {
    var host = document.getElementById('bench-app');
    if (!host)
      fail('missing #bench-app after ' + operation);
    if (operation === 'unmount') {
      if (host.childElementCount !== 0)
        fail('unmount retained ' + host.childElementCount + ' root children');
      return;
    }

    var expected = visibleRows(model);
    var summary = host.querySelector('.bench-summary');
    if (!summary)
      fail('missing summary after ' + operation);
    if (Number(summary.getAttribute('data-visible-count')) !== expected.length)
      fail(operation + ' visible count is ' + summary.getAttribute('data-visible-count') + ', expected ' + expected.length);
    if (Number(summary.getAttribute('data-total-count')) !== model.rows.length)
      fail(operation + ' total count is ' + summary.getAttribute('data-total-count') + ', expected ' + model.rows.length);

    var elements = host.querySelectorAll('.bench-row');
    if (elements.length !== expected.length)
      fail(operation + ' DOM row count is ' + elements.length + ', expected ' + expected.length);
    var hash = 2166136261;
    var selectedVisible = 0;
    for (var i = 0; i < elements.length; ++i) {
      var id = Number(elements[i].getAttribute('data-row-id'));
      hash = Math.imul((hash ^ id) >>> 0, 16777619) >>> 0;
      if (elements[i].classList.contains('selected'))
        ++selectedVisible;
    }
    if ((hash >>> 0) !== idHash(expected))
      fail(operation + ' keyed DOM order hash is ' + (hash >>> 0) + ', expected ' + idHash(expected));
    var expectedSelected = expected.reduce(function (count, row) {
      return count + (model.selected.has(row.id) ? 1 : 0);
    }, 0);
    if (selectedVisible !== expectedSelected)
      fail(operation + ' selected visible count is ' + selectedVisible + ', expected ' + expectedSelected);
    if (host.querySelectorAll('.row-detail').length !== expectedSelected)
      fail(operation + ' conditional detail count is incorrect');
    if (Number(summary.getAttribute('data-selected-count')) !== model.selected.size)
      fail(operation + ' summary selected count is incorrect');
    if (host.querySelectorAll('.bench-toolbar button').length !== 4)
      fail(operation + ' toolbar event controls are missing');

    if (expected.length) {
      var probes = [0, Math.floor(expected.length / 2), expected.length - 1];
      for (var p = 0; p < probes.length; ++p) {
        var index = probes[p];
        var element = elements[index];
        if (element.getAttribute('data-row-id') !== String(expected[index].id))
          fail(operation + ' row key mismatch at ' + index);
        var label = element.querySelector('.row-label');
        var score = element.querySelector('.row-score');
        if (!label || label.textContent !== expected[index].label)
          fail(operation + ' row label mismatch at ' + index);
        if (!score || score.textContent !== String(expected[index].score))
          fail(operation + ' row score mismatch at ' + index);
      }
    }
  }

  function nextFrame() {
    return new Promise(function (resolve) { requestAnimationFrame(resolve); });
  }

  function errorValue(error) {
    return {
      name: error && error.name ? String(error.name) : 'Error',
      message: error && error.message ? String(error.message) : String(error),
      stack: error && error.stack ? String(error.stack) : ''
    };
  }

  function setVisibleStatus(status, detail) {
    var element = document.getElementById('bench-status');
    if (!element)
      return;
    element.setAttribute('data-status', status);
    element.textContent = detail ? status + ': ' + detail : status;
  }

  function install(adapter, framework) {
    if (!adapter || typeof adapter.mount !== 'function')
      throw new Error('invalid benchmark adapter');

    var state = {
      status: 'ready',
      done: false,
      captureReady: false,
      captureStatus: 'not_started',
      config: null,
      error: null,
      resultValue: null
    };

    async function execute(config) {
      var samples = {};
      OPERATION_NAMES.forEach(function (name) { samples[name] = []; });
      var assertionCount = 0;
      var model = null;
      var totalIterations = config.warmups + config.iterations;

      async function operation(name, measured, action) {
        await nextFrame();
        var start = performance.now();
        await action();
        var elapsed = performance.now() - start;
        validate(model, name);
        ++assertionCount;
        if (measured)
          samples[name].push(elapsed);
        await nextFrame();
      }

      for (var iteration = 0; iteration < totalIterations; ++iteration) {
        var measured = iteration >= config.warmups;
        model = { rows: makeRows(config.rows, 1), selected: new Set(), filtered: false };
        await operation('mount', measured, function () { return adapter.mount(model.rows); });

        var selectedIds = [];
        for (var i = 9; i < model.rows.length; i += 10)
          selectedIds.push(model.rows[i].id);
        model.selected = new Set(selectedIds);
        await operation('update_selected', measured, function () { return adapter.updateSelected(selectedIds); });

        model.rows = reorderRows(model.rows);
        await operation('keyed_reorder', measured, function () { return adapter.reorder(model.rows); });

        model.rows = prependRows(model.rows, config.rows);
        await operation('prepend', measured, function () { return adapter.prepend(model.rows.slice(0, PREPEND_COUNT)); });

        model.filtered = true;
        await operation('filter', measured, function () { return adapter.setFiltered(true); });

        model.filtered = false;
        await operation('restore', measured, function () { return adapter.setFiltered(false); });

        await operation('unmount', measured, function () { return adapter.unmount(); });
      }


      return {
        schema_version: 1,
        workload_version: WORKLOAD_VERSION,
        framework: framework,
        config: {
          rows: config.rows,
          warmups: config.warmups,
          iterations: config.iterations,
          prepend_rows: PREPEND_COUNT,
          selected_stride: 10,
          filter_divisor: 10,
          final_capture_state: 'mounted full row count with every tenth row selected'
        },
        operation_js_commit_ms: samples,
        correctness_assertions: assertionCount,
        capture_verification_assertions: 0,
        assertion_failures: []
      };
    }

    async function prepareCaptureState() {
      var config = state.config;
      var model = { rows: makeRows(config.rows, 1), selected: new Set(), filtered: false };
      await adapter.mount(model.rows);
      var selectedIds = [];
      for (var i = 9; i < model.rows.length; i += 10)
        selectedIds.push(model.rows[i].id);
      model.selected = new Set(selectedIds);
      await adapter.updateSelected(selectedIds);
      await nextFrame();
      validate(model, 'capture_verification');
      ++state.resultValue.correctness_assertions;
      state.resultValue.capture_verification_assertions = 1;
      await nextFrame();
    }

    window.uiBench = {
      get status() { return state.status; },
      get done() { return state.done; },
      get captureReady() { return state.captureReady; },
      get captureStatus() { return state.captureStatus; },
      start: function (config) {
        if (state.status !== 'ready')
          throw new Error('benchmark can only be started once');
        if (!config || !Number.isInteger(config.rows) || config.rows < 1 || config.rows > 100000)
          throw new Error('rows must be an integer from 1 through 100000');
        if (!Number.isInteger(config.warmups) || config.warmups < 0 || config.warmups > 1000)
          throw new Error('warmups must be an integer from 0 through 1000');
        if (!Number.isInteger(config.iterations) || config.iterations < 1 || config.iterations > 1000)
          throw new Error('iterations must be an integer from 1 through 1000');
        state.config = config;
        state.status = 'running';
        setVisibleStatus('running', framework.name + ' ' + framework.version + ', ' + config.rows + ' rows');
        execute(config).then(function (result) {
          state.resultValue = result;
          state.status = 'complete';
          state.done = true;
          setVisibleStatus('complete', assertionCountText(result.correctness_assertions));
        }, function (error) {
          state.error = errorValue(error);
          state.resultValue = {
            schema_version: 1,
            workload_version: WORKLOAD_VERSION,
            framework: framework,
            config: config,
            operation_js_commit_ms: null,
            correctness_assertions: null,
            assertion_failures: [state.error]
          };
          state.status = 'error';
          state.done = true;
          setVisibleStatus('error', state.error.message);
        });
        return true;
      },
      prepareCapture: function () {
        if (state.status !== 'complete' || state.captureStatus !== 'not_started')
          throw new Error('capture verification can only start after one completed workload');
        state.captureStatus = 'preparing';
        prepareCaptureState().then(function () {
          state.captureStatus = 'ready';
          state.captureReady = true;
          setVisibleStatus('complete', assertionCountText(state.resultValue.correctness_assertions));
        }, function (error) {
          state.error = errorValue(error);
          state.resultValue.assertion_failures.push(state.error);
          state.status = 'error';
          state.captureStatus = 'error';
          state.captureReady = true;
          setVisibleStatus('error', state.error.message);
        });
        return true;
      },
      result: function () {
        if (!state.done)
          return null;
        return state.resultValue;
      }
    };
    setVisibleStatus('ready', framework.name + ' ' + framework.version);
  }

  function assertionCountText(count) {
    return count + ' DOM transition assertions passed';
  }

  function failBootstrap(frameworkName, error) {
    var value = errorValue(error);
    window.uiBench = {
      status: 'error',
      done: true,
      start: function () { throw error; },
      result: function () {
        return {
          schema_version: 1,
          workload_version: WORKLOAD_VERSION,
          framework: { name: frameworkName, version: 'unavailable' },
          config: null,
          operation_js_commit_ms: null,
          correctness_assertions: null,
          assertion_failures: [value]
        };
      }
    };
    setVisibleStatus('error', value.message);
  }

  window.MiniUIBench = {
    install: install,
    failBootstrap: failBootstrap
  };
}());
