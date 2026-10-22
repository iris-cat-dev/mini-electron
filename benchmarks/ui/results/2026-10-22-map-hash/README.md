# QuickJS 对象键哈希优化：Vue / React 实测

本轮只保留一项生产修改：`third_party/quickjs/quickjs.c` 中 `map_hash_key()` 的对象／Symbol 地址混合。没有改框架资产、减少行数／迭代／绘制，也没有扩大 C++ `/O2`、改 GC 阈值或削弱 roots。

正式 Ninja `host` 构建、最终 runtime consumer、正式 SDK 的 Vue/React 1k 重跑均 exit 0；真实 Chrome 默认／软件两组共 16 cells 也重新完成。完整命令、退出码及压缩日志见 [final-validation.json](final-validation.json)。

## 问题与修改

旧代码先将指针乘以 3163，再与 `hash_size - 1` 相与。乘奇数不会消除地址对齐的低零位，导致对象键只能落入部分桶；例如 16-byte 对齐且桶数至少为 16 时，同一 tag 至多使用 1/16 的桶。身份比较仍正确，但碰撞链变长。Vue 的响应式集合会实际走到这里。

新代码折叠指针高低位，再使用 MurmurHash3 的 32-bit finalizer 混合，最后仍走原来的 tag、桶索引及 SameValueZero 比较。没有新分配、缓存、外部依赖或针对框架的判断。Map/Set 的插入顺序链、WeakMap/WeakSet 的引用语义没有改动；字符串和数值键哈希保持不变。

真实 runtime consumer 新增 4,096 个对象与 4,096 个 Symbol 的集合边界回归：扩容后取值、相同描述的不同 Symbol、不同对象身份、活迭代器期间删除／重插、WeakMap/WeakSet 删除、Proxy 不触发属性陷阱、NaN、±0 和 clear。旧 SDK 与候选 SDK 均通过；它是语义保护，不是声称修复前功能错误。

## 固定口径

沿用 [上一轮](../2026-10-22-p1-p2/README.md) 的离线 Vue 3.5.13 / React 18.3.1、100/1k/5k/10k 行、预热 2 次、测量 7 次、1280×720、DPR 1。正式 A/B 均用原有大规模运行上限 900,000 ms，严格比较脚本通过。

`commit` 是每次迭代七项 JS/DOM 提交操作之和的中位数；不是七个中位数相加，也不是隔离的解释器 CPU 时间。wall 是完整 workload。每格正式矩阵各一个独立进程，七次测量不等于七个独立进程；不报告跨进程置信区间。1k、5k 的前置候选试跑作为补充记录，不与正式矩阵混池。

## 本轮相对上一版的增量

正的“下降”表示改善，负值表示回退。以下不是重复计算上一轮已经取得的收益。

| 框架/行数 | commit 前 → 后（ms） | commit 下降 | 整轮前 → 后（s） | wall 下降 |
|---|---:|---:|---:|---:|
| Vue 100 | 151.3 → 140.2 | 7.3% | 3.165 → 3.239 | **−2.3%** |
| Vue 1,000 | 1,058.2 → 994.5 | 6.0% | 16.647 → 16.172 | 2.9% |
| Vue 5,000 | 5,489.6 → 4,908.3 | 10.6% | 82.882 → 78.299 | 5.5% |
| Vue 10,000 | 11,799.3 → 9,855.7 | 16.5% | 171.675 → 158.862 | 7.5% |
| React 100 | 86.4 → 85.1 | 1.5% | 2.939 → 2.808 | 4.5% |
| React 1,000 | 603.1 → 590.6 | 2.1% | 11.303 → 10.861 | 3.9% |
| React 5,000 | 4,489.4 → 4,402.1 | 1.9% | 67.548 → 67.618 | **−0.1%** |
| React 10,000 | 12,930.0 → 13,081.9 | **−1.2%** | 173.410 → 174.141 | **−0.4%** |

**Vue 有明确收益；React 没有一致改善。** Vue 10k 进程 CPU 从 175.250 s 降到 161.688 s（7.7%），并非只把工作挪出 commit 区间。React 10k CPU 从 175.078 s 到 175.891 s（增加约 0.5%）。

内存没有一致下降，不作为本次收益：Vue 5k private 2,023.4 → 2,042.5 MiB，10k 3,638.7 → 3,671.9 MiB；React 1k 232.3 → 252.7 MiB，10k 1,573.2 → 1,402.3 MiB。private 是 workload/capture 末端进程值，不是 live heap 或 peak working set。完整 CPU、内存、分操作、p95、Tick、销毁数据见 [before-after/before-after.json](before-after/before-after.json)。

## 真实输出未缩水

- 正式 A/B 两边均 8/8 cells complete、normal exit 0。
- 每格 64 项 DOM transition 断言与独立 populated capture 验证通过。
- 八格 software workload generation 全部 **126 → 126**。
- 八格前后 1280×720 capture 的 **RGB 像素完全相同**，见 [capture-comparison.json](capture-comparison.json)。这是本轮 native 前后对比，不是与 Chrome 像素等同的声明。
- 已实际查看 [Vue 10k](after/captures/vue-10000.png) 与 [React 10k](after/captures/react-10000.png)：完整 dashboard、10,000 条记录、1,000 条选择状态及完成提示真实绘制。

## 与刷新后的真实 Chrome 对照

两组 Chrome 本轮都重新运行：Chrome 155.0.8059.85 / V8 15.5.35.25。默认有界面浏览器及 `--disable-gpu` 软件控制均使用相同固定资产和工作量；不是旧 Chrome 数字冒充本轮测量。

| 框架/行数 | native commit ms | Chrome default ms | Chrome software ms | native/default |
|---|---:|---:|---:|---:|
| Vue 100 | 140.2 | 7.6 | 7.7 | 18.4× |
| Vue 1,000 | 994.5 | 42.0 | 39.8 | 23.7× |
| Vue 5,000 | 4,908.3 | 218.0 | 227.5 | 22.5× |
| Vue 10,000 | 9,855.7 | 453.6 | 500.3 | 21.7× |
| React 100 | 85.1 | 4.6 | 4.8 | 18.5× |
| React 1,000 | 590.6 | 23.7 | 24.1 | 24.9× |
| React 5,000 | 4,402.1 | 157.6 | 136.9 | 27.9× |
| React 10,000 | 13,081.9 | 378.6 | 422.0 | 34.6× |

Vue 10k 整轮 native/default/software 为 **158.862 / 15.870 / 16.275 s**；React 10k 为 **174.141 / 14.671 / 15.299 s**。差距仍大，不能宣称接近 Chrome；软件 Chrome 同样领先。完整分操作及控制条件见 [chrome-comparison/comparison.json](chrome-comparison/comparison.json)。

## 采样证据与边界


系统 WPR CPU 采样因权限策略 `0xc5585011` 失败，未将其记为成功。随后只对自己创建的 benchmark 子进程主线程做 `SuspendThread` / `GetThreadContext` 采样，随机间隔 3–7 ms，始终恢复线程。它覆盖启动、2 次预热、7 次测量、capture、销毁；只用于诊断，时间不进入正式性能表。

基线 Vue 1k：3,277 个样本；候选 3,059。React 分别 2,094 / 2,047。四次均 exit 0，采样错误列表为空。按各自真实 executable/module base 和匹配 linker map 符号化。

| 主线程采样占比 | Vue 前 → 后 | React 前 → 后 |
|---|---:|---:|
| `JS_CallInternal` | 18.13% → 19.39% | 16.43% → 16.27% |
| `JS_GetPropertyImpl` | 6.04% → 6.11% | 6.11% → 6.11% |
| `map_` / `js_map_` 全部命名函数合计 | 4.27% → 1.77% | 0 个样本 → 1 个样本 |

修改后编译器内联了 `map_find_record`，它不再单独出现在 linker map；因此不能说这个函数“耗时降为零”。表中将查询、修改和 GC 的 Map 命名函数按同一口径合并。占比下降与实测收益方向一致，但不是独立的精确成本归因。

限制：主线程 RIP 抽样不是 ETW exclusive CPU 时间，没有完整调用栈，含等待，不覆盖 worker；linker ICF 会使部分共享地址存在多个符号别名。NTDLL 的约 16% 模块样本包含等待，且仅有导出符号近邻解析，**不能全部归因于 heap allocation**。原始样本及解码结果在 `profiling/`。

## 本轮放弃的实验

以下改动均已回退，专属辅助代码也未进入最终生产版本：

1. 直接复用 VM atom-index 查询，替代 facade 的 atom→string→UTF-8→decimal 转换：1k ABBA 未显示稳定整轮收益，Vue wall 有回退。
2. 已知 callback Object/primitive 的类型直达路径：首轮没有可信收益，未继续叠加。
3. HostGet 在无 getter 时跳过 handler selection：首轮没有可信整轮收益，未保留。

对应原始数据在 `experiments/`。首个大规模 hash 试跑漏用了 900 秒上限，Vue/React 10k 均在默认 120 秒超时；失败原始记录保留于 `experiments/ui-map-large-after/`，没有改工作量掩盖失败，也没有把未完成格计入成功矩阵。

## 剩余优化方向

React 的问题不能用这次 Map 改动来解释或宣称已解决。正式 after 中：

- React keyed-reorder 中位数：1k **165.5 ms**、5k **2,435.1 ms**、10k **9,053.8 ms**。
- React 10k 每次对齐迭代里，reorder 的占比中位数为 **69.24%**。
- 相比之下 Vue reorder 为 194.6 / 1,013.5 / 2,060.7 ms。

下一优先级应是 React 大规模重排的 JS/DOM 调用链和属性读取，而不是继续优先调整 raster 或增加 GPU。[INFERENCE] 原型／shape 查找缓存可能有价值，但尚未实现，也不能仅凭 1k 采样认定具体重排算法是原因。

已知 C++ facade `/O2` 生命周期失败与独立 JS WeakMap dead-key 环释放问题仍未修复；本次不改变其结论。callback ABI、realm/security、Clipboard/Broker、ThreadPool、HWND/PointerLock/input 合同均未改。**不关机。**

### React 10k 的进一步实测

单独诊断、不混入正式表：正式 SDK 主线程采样 33,907 次，exit 0、无采样错误。`JS_CallInternal` 占 **26.56%**，`JS_GetPropertyImpl` 占 **19.53%**；相较 1k，属性读取占比明显增大。联合 GC 的 `WorkKey` hash lookup 也出现在热点中（约 3.42%），但不是此次已解决的对象。

真实 Chrome 另跑了一次 React 10k JS CPU profile。固定 ReactDOM 资产中的 `Ci`（`react-dom.production.min.js:166–167`）有 831 个样本，约 **7.17%**；该函数在 placement 时沿 fiber `.sibling` / `.return` / `.flags` 搜索可用 host sibling。资产中 `Ha` 对 placement 调用它；固定 workload 真正反转 keyed rows。重复扫描是具体候选，不需要篡改 React 或削减重排 workload。

这些证据支持优先验证 VM 普通属性读取／shape 查找与重排遍历成本；尚不能将 native 两个函数的全部样本归给 `Ci`，也不能把 Chrome 的 `(program)` 样本全部算作渲染。没有植入未经验证的 inline cache、没有启用失败的 C++ O2。见 `profiling/react-10000-native/` 与 `profiling/react-10000-chrome/`。

## 正式产物身份与复现

- A/B baseline SDK：`75a46b9cab7b1413046f64c65904ef70d254bef434364c14b2ca1db1d19dd2ba`
- 完整 after 矩阵的私有候选 SDK：`adb7407b65962d4ccfdf59b497cf73a6447b6425b0e81967f177c6dd883525b5`
- 正式重新构建 SDK：`c02cf3da4a07981541d2c0039c6ff752a1c30d029ada03adc118e66af61f7b2d`

没有把两个不同的 archive hash 说成相同。完整矩阵候选 host 与正式重新链接 host 的 **全部 10 个 PE section 内容逐字节相同**；首个 header page 的差异只在 COFF link timestamp。证据：[executable-section-identity.json](executable-section-identity.json)。正式 SDK 自身又实际执行了 runtime regression 和两格 1k 框架：Vue commit 976.2 ms / wall 15.876 s，React 575.7 ms / 10.937 s；两格均 126 frames。这组额外重跑单列在 `production-smoke/`，不混池替换正式 A/B。

正式库 412,669,916 bytes，示例 40,128,000 bytes；本次不是体积优化声明。源码及产物 SHA-256 见 [build-identity.json](build-identity.json)、[source-manifest.json](source-manifest.json)。

```text
ninja -f out/ui-windows-x64/build.ninja -j4 host
python benchmarks/ui/check_runtime.py --out-dir out/ui-map-runtime-checks
python benchmarks/ui/run.py --build --timeout-ms 900000 --out-dir out/ui-map-new-native
python benchmarks/ui/chrome.py --out-dir out/ui-map-new-chrome
python benchmarks/ui/chrome.py --disable-gpu --out-dir out/ui-map-new-chrome-software
python benchmarks/ui/before_after.py --before benchmarks/ui/results/2026-10-22-map-hash/before --after out/ui-map-new-native --out-dir out/ui-map-new-delta
python benchmarks/ui/compare.py --mini-out out/ui-map-new-native --chrome-out out/ui-map-new-chrome --chrome-software-out out/ui-map-new-chrome-software --out-dir out/ui-map-new-comparison
```

Ninja 不在 PATH 时使用本机 Visual Studio 附带的实际路径，已记录在 `final-validation.json`。正式性能、采样、构建和浏览器控制没有相互并跑。

已删除本轮 35 个自建临时目录，释放约 2.909 GiB，详见 [cleanup.json](cleanup.json)。保留正式 SDK、最终 consumer host、runtime-check 结果和本报告中的原始数据、截图、采样、压缩构建日志；上一轮 baseline/报告不覆盖。原始 JSON 中的路径是执行时路径，临时输出清理后以本报告的归档副本为准。三个被否决试验的生产源码已还原，其 SHA-256 与上一轮一致，见 `source-manifest.json`。机器保持开机。
