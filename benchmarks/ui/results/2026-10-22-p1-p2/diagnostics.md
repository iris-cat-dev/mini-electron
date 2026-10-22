# 热点定位与方案取舍

这些是独立诊断和中间版本试验，不能与最终无插桩 matrix 混算。所有 workload 使用原固定 Vue 3.5.13 / React 18.3.1 assets。

## 阶段归因

`phase-before-dispatch.json` / `phase-after-dispatch.json`：1,000 行、预热 2 次、测量 7 次。前者包含 P1/RegExp 修复，后者增加无 handler 快路径；两者都早于最后的 native-job/property 边界修复。

| 框架/版本 | JS 入口，含 bridge/native 回调 | native callback 子区间 | layout lifecycle | 软件绘制 | host 联合 GC |
|---|---:|---:|---:|---:|---:|
| React，快路径前 | 7,348.5 ms | 1,722.9 ms | 3,283.2 ms | 803.2 ms | 559.2 ms |
| React，快路径后 | 6,315.1 ms | 922.2 ms | 3,317.1 ms | 833.7 ms | 418.6 ms |
| Vue，快路径前 | 12,043.7 ms | 1,407.2 ms | 3,227.8 ms | 832.2 ms | 2,642.4 ms |
| Vue，快路径后 | 11,329.7 ms | 1,124.7 ms | 3,097.4 ms | 759.3 ms | 2,306.5 ms |

计时覆盖实际 `JS_CallInternal`，不仅是 facade ExecutionScope。各类别在自身最外层累加，跨类别仍有嵌套：native callback 包含在 JS 执行中，callback 也可重入 JS。不能将这些列相加，或简单相减得到“纯解释器 CPU”。插桩有开销；QPC pair 的低分辨率下限为零不表示零开销。

React callback 准备次数 **3,879,176 → 1,023,376（−73.6%）**；Vue **2,108,360 → 1,016,905（−51.8%）**。实际 handler 缺席时，不再构造 CallbackState/PropertyDescriptor；访问安全检查和普通属性语义仍执行。

独立无插桩 O1 的 1,000 行对照：

| 框架 | commit 中位数，前 → 后 | whole workload 变化 |
|---|---:|---:|
| React | 713.8 → 610.2 ms | −6.1% |
| Vue | 1,195.9 → 1,211.0 ms | −0.4% |

Vue 没有稳定收益；两项单次试验的末端 private bytes 都上升，详见 `dispatch-o1-comparison.json`，不能只报更好的一列。这里的 commit 是每次迭代七项操作之和的中位数，不是各操作中位数之和。

**优先级：JS/bridge，其次 layout；纯软件 raster 不是当前首要开销。** GC 对 Vue 仍有显著成本，须和保留内存一起权衡。

## 采用的实现与边界

- 原生属性按 `JSAtom` 索引，保留 atom/descriptor 所有权；getter 重入后重新查找并验证 generation；clone/delete/redefine 同步迁移。
- 常数时间观察 QuickJS/native heap/handle arena 分配量，只在 host 安全点触发联合 GC。采用最小 64 MiB、与保留堆同量的增长预算；不是增量 GC，也不承诺硬暂停上限。保留联合 JS → native → 联合 JS 与保守 native 栈 roots。
- `js_regexp_exec` 释放取得的 constructor owned reference，并传播 getter/define 失败；没有断开合法 realm/root。匹配 React 1,000 行、2+7 workload 的销毁诊断：**6,240 wrappers / 6,239 edges → 0 / 0**。额外定位记录确认 RegExp constructor 的 188 个多余引用来自该调用点。
- Handle arena 只释放完全空闲的固定数组，每池留一个空块，不移动活对象地址。真实回归容量 **1,554,432 → 241,664 bytes**，64 个活 handle 保留。
- 只对 `third_party/quickjs/quickjs.c` 使用 `/O2`；C++ facade 保持 `/O1`。
- 复核发现的 production `DrainJobs` native-job 活跃标记、native readonly redefinition、setter 拒写和 query 异常问题均有真实 failing-before 回归：`runtime-checks-boundaries-before/runtime-checks.json` 记录 17 项失败。最终验证结果另见主报告，不把源码审查当作通过证据。

## 没有采用或没有宣称解决的项

### 过激的 GC 预算

32 MiB / 50% 方案使 React 5,000 行整轮耗时比原基线回退约 13%，即使 commit 变好。改用 64 MiB / 100% 并重测。保留 `gc-budget-decision.json` 与 `experiments/`，没有隐藏回退。

### C++ facade `/O2`

全 hot-TU `/O2` 和 facade-only `/O2` 的实际失败均保留，不算成功配置。中间版本 facade-only React5k 出现 `0xC0000409`、subcode 7 (`FAST_FAIL_FATAL_APP_EXIT`)。精确 link map 将 CDB 栈定位到：

- `Adopt` 的 `IsolateFor` 失败分支，经 `Setup` / `HostGet`；
- 另一轮的 `HandleScope` / `CheckIsolate`，经 `HostGet`。

不能因 Windows 通用异常文字就断言 stack-buffer overrun，也没有证据证明是编译器 bug。更深的生命周期来源尚未查清，因此未启用 C++ facade O2。第二次诊断没有命中预设 Adopt breakpoint，而是在另一 invariant 失败；其 register dump 不被当作 Adopt context 证据。见 `o2-*-cdb.*` 和 symbolized JSON。

仅 QuickJS C `/O2` 的 1k/5k 四组先行试验全部完成；最终全矩阵另见主报告。

### JS WeakMap 环

旧版本独立诊断中，live WeakMap + dead key/value/native back-reference 环的释放断言失败。本轮未修复这一行为，也未把 native cppgc ephemeron 的通过冒充 JS WeakMap 全覆盖。保留 `known-limitations/weakmap-cycle-diagnostic.json` 及原诊断源；它不属于宣称通过的最终 consumer。

不引入 JIT、跳过布局/绘制、弱化访问安全或删除合法 roots，也不修改框架 workload 减少工作。
