(function () {
  'use strict';
  try {
    var h = React.createElement;
    var Fragment = React.Fragment;
    var current = React.createRef();
    var host = document.getElementById('bench-app');
    var root = null;

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

    function Toolbar(props) {
      return h('header', { className: 'bench-toolbar' },
        h('h1', null, 'React deterministic records'),
        h('button', { type: 'button', onClick: props.onSelect }, 'Select stride'),
        h('button', { type: 'button', onClick: props.onReorder }, 'Reorder keys'),
        h('button', { type: 'button', onClick: props.onPrepend }, 'Prepend 100'),
        h('button', { type: 'button', onClick: props.onFilter }, props.filtered ? 'Restore' : 'Filter'));
    }

    function Summary(props) {
      var scoreTotal = React.useMemo(function () {
        return props.visible.reduce(function (sum, row) { return sum + row.score; }, 0);
      }, [props.visible]);
      return h('section', {
        className: 'bench-summary',
        'data-visible-count': String(props.visible.length),
        'data-total-count': String(props.total),
        'data-selected-count': String(props.selectedCount)
      },
      h('div', { className: 'summary-card' }, 'Visible', h('span', { className: 'summary-value' }, String(props.visible.length))),
      h('div', { className: 'summary-card' }, 'Total', h('span', { className: 'summary-value' }, String(props.total))),
      h('div', { className: 'summary-card' }, 'Selected', h('span', { className: 'summary-value' }, String(props.selectedCount))),
      h('div', { className: 'summary-card' }, 'Visible score', h('span', { className: 'summary-value' }, String(scoreTotal))));
    }

    function Detail(props) {
      return h('tr', { className: 'row-detail' },
        h('td', { colSpan: 4 }, props.row.detail + ' · selected with keyed identity ' + props.row.id));
    }

    var RecordRow = React.memo(function RecordRow(props) {
      var row = props.row;
      return h(Fragment, null,
        h('tr', {
          className: 'bench-row' + (props.selected ? ' selected' : ''),
          'data-row-id': String(row.id),
          onClick: function () { props.onToggle(row.id); }
        },
        h('td', { className: 'row-id' }, String(row.id)),
        h('td', { className: 'row-label' }, row.label),
        h('td', { className: 'row-group' }, row.group),
        h('td', { className: 'row-score' }, String(row.score))),
        props.selected ? h(Detail, { row: row }) : null);
    });

    function RecordsTable(props) {
      return h('table', { className: 'bench-table' },
        h('thead', null, h('tr', null,
          h('th', null, 'Id'), h('th', null, 'Label'), h('th', null, 'Group'), h('th', null, 'Score'))),
        h('tbody', null,
          props.rows.length ? props.rows.map(function (row) {
            return h(RecordRow, {
              key: row.id,
              row: row,
              selected: props.selected.has(row.id),
              onToggle: props.onToggle
            });
          }) : h('tr', { className: 'empty-row' }, h('td', { colSpan: 4 }, 'No matching rows'))));
    }

    var App = React.forwardRef(function App(props, ref) {
      var rowsState = React.useState(props.initialRows);
      var rows = rowsState[0];
      var setRows = rowsState[1];
      var selectedState = React.useState(function () { return new Set(); });
      var selected = selectedState[0];
      var setSelected = selectedState[1];
      var filterState = React.useState(false);
      var filtered = filterState[0];
      var setFiltered = filterState[1];

      var visible = React.useMemo(function () {
        var computed = filtered
          ? rows.filter(function (row) { return row.id % 10 === 0; })
          : rows.slice();
        computed.sort(function (left, right) { return left.order - right.order; });
        return computed;
      }, [rows, filtered]);

      var toggle = React.useCallback(function (id) {
        setSelected(function (previous) {
          var next = new Set(previous);
          if (next.has(id)) next.delete(id); else next.add(id);
          return next;
        });
      }, []);

      React.useImperativeHandle(ref, function () {
        return {
          updateSelected: function (ids) { setSelected(new Set(ids)); },
          reorder: function (nextRows) { setRows(nextRows); },
          prepend: function (additions) {
            setRows(function (previous) {
              return additions.concat(previous.map(function (row, index) {
                return cloneWithOrder(row, additions.length + index);
              }));
            });
          },
          setFiltered: function (value) { setFiltered(value); }
        };
      }, []);

      var selectStride = React.useCallback(function () {
        setSelected(new Set(rows.filter(function (_, index) { return (index + 1) % 10 === 0; }).map(function (row) { return row.id; })));
      }, [rows]);
      var reorder = React.useCallback(function () {
        setRows(function (previous) {
          return previous.slice().reverse().map(function (row, index) { return cloneWithOrder(row, index); });
        });
      }, []);
      var prepend = React.useCallback(function () {
        setRows(function (previous) {
          var maxId = previous.reduce(function (value, row) { return Math.max(value, row.id); }, 0);
          var additions = new Array(100);
          for (var i = 0; i < additions.length; ++i) {
            var id = maxId + i + 1;
            additions[i] = { id: id, order: i, label: 'Record ' + id, group: 'Group ' + (id % 7), score: (Math.imul(id, 48271) + 17) % 10000, detail: 'Deterministic detail ' + id + ' / ' + ((id * 13) % 97) };
          }
          return additions.concat(previous.map(function (row, index) { return cloneWithOrder(row, 100 + index); }));
        });
      }, []);

      return h('main', { className: 'bench-shell' },
        h(Toolbar, {
          filtered: filtered,
          onSelect: selectStride,
          onReorder: reorder,
          onPrepend: prepend,
          onFilter: function () { setFiltered(function (value) { return !value; }); }
        }),
        h(Summary, { visible: visible, total: rows.length, selectedCount: selected.size }),
        h(RecordsTable, { rows: visible, selected: selected, onToggle: toggle }));
    });

    var adapter = {
      mount: function (rows) {
        root = ReactDOM.createRoot(host);
        ReactDOM.flushSync(function () { root.render(h(App, { initialRows: rows, ref: current })); });
        if (!current.current)
          throw new Error('React component ref was not committed');
      },
      updateSelected: function (ids) {
        ReactDOM.flushSync(function () { current.current.updateSelected(ids); });
      },
      reorder: function (rows) {
        ReactDOM.flushSync(function () { current.current.reorder(rows); });
      },
      prepend: function (rows) {
        ReactDOM.flushSync(function () { current.current.prepend(rows); });
      },
      setFiltered: function (value) {
        ReactDOM.flushSync(function () { current.current.setFiltered(value); });
      },
      unmount: function () {
        ReactDOM.flushSync(function () { root.unmount(); });
        root = null;
      }
    };

    MiniUIBench.install(adapter, {
      name: 'react',
      version: React.version,
      renderer: 'react-dom ' + ReactDOM.version,
      distribution: 'production UMD'
    });
  } catch (error) {
    MiniUIBench.failBootstrap('react', error);
  }
}());
