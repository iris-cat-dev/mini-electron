(function () {
  'use strict';
  try {
    var h = Vue.h;
    var Fragment = Vue.Fragment;
    var application = null;
    var instance = null;
    var host = document.getElementById('bench-app');

    function cloneWithOrder(row, order) {
      return {
        id: row.id,
        order: order,
        label: row.label,
        group: row.group,
        score: row.score,
        detail: row.detail
      };
    }

    var Toolbar = {
      props: ['filtered'],
      emits: ['select', 'reorder', 'prepend', 'filter'],
      setup: function (props, context) {
        return function () {
          return h('header', { class: 'bench-toolbar' }, [
            h('h1', null, 'Vue deterministic records'),
            h('button', { type: 'button', onClick: function () { context.emit('select'); } }, 'Select stride'),
            h('button', { type: 'button', onClick: function () { context.emit('reorder'); } }, 'Reorder keys'),
            h('button', { type: 'button', onClick: function () { context.emit('prepend'); } }, 'Prepend 100'),
            h('button', { type: 'button', onClick: function () { context.emit('filter'); } }, props.filtered ? 'Restore' : 'Filter')
          ]);
        };
      }
    };

    var Summary = {
      props: ['visible', 'total', 'selectedCount'],
      setup: function (props) {
        var scoreTotal = Vue.computed(function () {
          return props.visible.reduce(function (sum, row) { return sum + row.score; }, 0);
        });
        return function () {
          return h('section', {
            class: 'bench-summary',
            'data-visible-count': String(props.visible.length),
            'data-total-count': String(props.total),
            'data-selected-count': String(props.selectedCount)
          }, [
            h('div', { class: 'summary-card' }, ['Visible', h('span', { class: 'summary-value' }, String(props.visible.length))]),
            h('div', { class: 'summary-card' }, ['Total', h('span', { class: 'summary-value' }, String(props.total))]),
            h('div', { class: 'summary-card' }, ['Selected', h('span', { class: 'summary-value' }, String(props.selectedCount))]),
            h('div', { class: 'summary-card' }, ['Visible score', h('span', { class: 'summary-value' }, String(scoreTotal.value))])
          ]);
        };
      }
    };

    var Detail = {
      props: ['row'],
      setup: function (props) {
        return function () {
          return h('tr', { class: 'row-detail' }, [
            h('td', { colspan: 4 }, props.row.detail + ' · selected with keyed identity ' + props.row.id)
          ]);
        };
      }
    };

    var RecordRow = {
      props: ['row', 'selected'],
      emits: ['toggle'],
      setup: function (props, context) {
        return function () {
          return h(Fragment, null, [
            h('tr', {
              class: 'bench-row' + (props.selected ? ' selected' : ''),
              'data-row-id': String(props.row.id),
              onClick: function () { context.emit('toggle', props.row.id); }
            }, [
              h('td', { class: 'row-id' }, String(props.row.id)),
              h('td', { class: 'row-label' }, props.row.label),
              h('td', { class: 'row-group' }, props.row.group),
              h('td', { class: 'row-score' }, String(props.row.score))
            ]),
            props.selected ? h(Detail, { row: props.row }) : null
          ]);
        };
      }
    };

    var RecordsTable = {
      props: ['rows', 'selected'],
      emits: ['toggle'],
      setup: function (props, context) {
        return function () {
          var body = props.rows.length ? props.rows.map(function (row) {
            return h(RecordRow, {
              key: row.id,
              row: row,
              selected: props.selected.has(row.id),
              onToggle: function (id) { context.emit('toggle', id); }
            });
          }) : [h('tr', { class: 'empty-row' }, [h('td', { colspan: 4 }, 'No matching rows')])];
          return h('table', { class: 'bench-table' }, [
            h('thead', null, [h('tr', null, [h('th', null, 'Id'), h('th', null, 'Label'), h('th', null, 'Group'), h('th', null, 'Score')])]),
            h('tbody', null, body)
          ]);
        };
      }
    };

    var App = {
      props: ['initialRows'],
      setup: function (props, context) {
        var rows = Vue.ref(props.initialRows);
        var selected = Vue.ref(new Set());
        var filtered = Vue.ref(false);
        var visible = Vue.computed(function () {
          var computedRows = filtered.value
            ? rows.value.filter(function (row) { return row.id % 10 === 0; })
            : rows.value.slice();
          computedRows.sort(function (left, right) { return left.order - right.order; });
          return computedRows;
        });

        function updateSelected(ids) { selected.value = new Set(ids); }
        function reorder(nextRows) { rows.value = nextRows; }
        function prepend(additions) {
          rows.value = additions.concat(rows.value.map(function (row, index) {
            return cloneWithOrder(row, additions.length + index);
          }));
        }
        function setFiltered(value) { filtered.value = value; }
        function toggle(id) {
          var next = new Set(selected.value);
          if (next.has(id)) next.delete(id); else next.add(id);
          selected.value = next;
        }
        function selectStride() {
          selected.value = new Set(rows.value.filter(function (_, index) { return (index + 1) % 10 === 0; }).map(function (row) { return row.id; }));
        }
        function localReorder() {
          rows.value = rows.value.slice().reverse().map(function (row, index) { return cloneWithOrder(row, index); });
        }
        function localPrepend() {
          var maxId = rows.value.reduce(function (value, row) { return Math.max(value, row.id); }, 0);
          var additions = new Array(100);
          for (var i = 0; i < additions.length; ++i) {
            var id = maxId + i + 1;
            additions[i] = { id: id, order: i, label: 'Record ' + id, group: 'Group ' + (id % 7), score: (Math.imul(id, 48271) + 17) % 10000, detail: 'Deterministic detail ' + id + ' / ' + ((id * 13) % 97) };
          }
          prepend(additions);
        }

        context.expose({
          updateSelected: updateSelected,
          reorder: reorder,
          prepend: prepend,
          setFiltered: setFiltered
        });

        return function () {
          return h('main', { class: 'bench-shell' }, [
            h(Toolbar, {
              filtered: filtered.value,
              onSelect: selectStride,
              onReorder: localReorder,
              onPrepend: localPrepend,
              onFilter: function () { filtered.value = !filtered.value; }
            }),
            h(Summary, { visible: visible.value, total: rows.value.length, selectedCount: selected.value.size }),
            h(RecordsTable, { rows: visible.value, selected: selected.value, onToggle: toggle })
          ]);
        };
      }
    };

    var adapter = {
      mount: async function (rows) {
        application = Vue.createApp(App, { initialRows: rows });
        instance = application.mount(host);
        if (!instance)
          throw new Error('Vue component instance was not mounted');
        await Vue.nextTick();
      },
      updateSelected: async function (ids) {
        instance.updateSelected(ids);
        await Vue.nextTick();
      },
      reorder: async function (rows) {
        instance.reorder(rows);
        await Vue.nextTick();
      },
      prepend: async function (rows) {
        instance.prepend(rows);
        await Vue.nextTick();
      },
      setFiltered: async function (value) {
        instance.setFiltered(value);
        await Vue.nextTick();
      },
      unmount: async function () {
        application.unmount();
        application = null;
        instance = null;
        await Vue.nextTick();
      }
    };

    MiniUIBench.install(adapter, {
      name: 'vue',
      version: Vue.version,
      renderer: 'vue-dom',
      distribution: 'production global'
    });
  } catch (error) {
    MiniUIBench.failBootstrap('vue', error);
  }
}());
