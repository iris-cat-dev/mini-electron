# P1/P2 与深入优化：最终 Vue / React 压测

**最终构建、原生回归和完整矩阵均通过。** 原基线不覆盖；实际失败、中间方案及已知限制保留。

- Native：8/8 cells 完成，normal exit 0；每格 64 项 DOM 状态断言及 1 项最终 capture 验证通过。
- 八格优化前后 workload software frame generation 均为 **126 → 126**，没有通过少绘制帧获得收益。
- 真实 Google Chrome：默认渲染 8/8、`--disable-gpu` 8/8 完成，normal exit 0。
- 新增边界回归：修复前实际 17 项失败；修复后 exit 0。包括 production `DrainJobs` native-job 重入/异常/arena 安全点，以及 native readonly property、SameValue、setter 拒写和 query 异常。
- 最终 native consumer 还覆盖 getter 重入、clone/delete/redefine、访问安全、microtask 边界、稳定 handle、native ephemeron、RegExp 引用平衡和真实异步 Blob 读取期间的联合 GC。
- 已读取最终 Vue/React 10k native captures 和真实 Chrome captures：页面、数据、选中行与完成状态真实绘制。不是像素等同声明；Chrome scrollbar/gutter 与原生离屏 surface 存在差异。

## 固定口径

原基线：[2026-10-22](../2026-10-22/README.md)。同一固定 Vue **3.5.13** / React **18.3.1** 离线生产构建，同一 workload、100/1k/5k/10k 行、预热 2 次/测量 7 次、1280×720、DPR 1。严格比较脚本验证 assets/config/machine/power 等条件；不更改案例减少工作。

机器为 Windows 10 x64、Core i9-13900K、32 GiB RAM、RTX 3090、卓越性能电源方案。Chrome 为真实有界面 **155.0.8059.85 / V8 15.5.35.25**，使用自有临时 profile，退出后清理，不干扰用户 profile。

`commit` 是每次迭代七项操作耗时之和的中位数，包含框架 JS、bridge 与同步 DOM 提交，不是纯解释器 CPU。整轮 wall/CPU/内存同时报告，避免把移出 commit 区间的 GC 当免费收益。7 个样本的 nearest-rank p95 就是最大样本；每格仅一次独立进程运行，不声称稳定长尾或跨进程置信区间。

## 相对原始基线

“下降”列正值为改善，负值为回退。private 为 workload/capture 末端的进程 private bytes，**不是 live heap，也不是 peak working set**。

| 框架/行数 | commit，前 → 后（ms） | commit 下降 | 整轮 wall，前 → 后（s） | wall 下降 | private，前 → 后（MiB） | private 下降 |
|---|---:|---:|---:|---:|---:|---:|
| Vue 100 | 313.5 → 150.7 | 51.9% | 4.98 → 3.45 | 30.8% | 185.5 → 127.3 | 31.4% |
| Vue 1,000 | 2,327.6 → 1,066.7 | 54.2% | 26.92 → 16.86 | 37.4% | 884.6 → 404.8 | 54.2% |
| Vue 5,000 | 11,901.9 → 5,587.5 | 53.1% | 131.25 → 85.26 | 35.0% | 4,040.1 → 2,212.3 | 45.2% |
| Vue 10,000 | 24,178.2 → 11,347.9 | 53.1% | 264.44 → 166.47 | 37.0% | 8,023.7 → 4,267.1 | 46.8% |
| React 100 | 138.2 → 79.4 | 42.5% | 3.33 → 2.34 | 29.6% | 86.6 → 93.1 | **−7.5%** |
| React 1,000 | 940.5 → 575.4 | 38.8% | 14.43 → 10.57 | 26.8% | 355.1 → 254.2 | 28.4% |
| React 5,000 | 6,092.6 → 4,424.1 | 27.4% | 78.94 → 67.15 | 14.9% | 1,523.2 → 693.2 | 54.5% |
| React 10,000 | 16,643.0 → 12,936.8 | 22.3% | 198.04 → 172.54 | 12.9% | 2,986.7 → 1,379.3 | 53.8% |

Vue 进程 CPU 总量下降约 **35.1–43.1%**；React 约 **13.5–28.5%**。分操作、p95、Tick、冷启动和销毁数据见 [before-after/before-after.json](before-after/before-after.json)。React 100 行内存上升不是被忽略的误差，当前配置保留这一取舍。

### 相对上一轮已优化版本的额外收益

参照 `experiments/ui-opt-regexp-full`：它已包含 GC64、属性索引、arena 回收和 RegExp 修复，但尚无本轮无 handler 快路径及 QuickJS-C-only O2。下面是完整最终组合的增量，不单独归因给其中一个修改。

| 框架/行数 | 上轮 → 最终 commit（ms） | 额外 commit 下降 | 额外整轮 wall 下降 |
|---|---:|---:|---:|
| Vue 1,000 | 1,195.9 → 1,066.7 | 10.8% | 7.4% |
| Vue 10,000 | 12,613.4 → 11,347.9 | 10.0% | 8.6% |
| React 1,000 | 713.8 → 575.4 | 19.4% | 13.3% |
| React 10,000 | 14,138.6 → 12,936.8 | 8.5% | 7.9% |

全部八格见 [deeper-optimization-delta.json](deeper-optimization-delta.json)。

## 与真实 Chrome 的剩余差距

两组 Chrome 均重新实跑。最后一列为 native / Chrome default / Chrome software 的整轮秒数。

| 框架/行数 | native commit ms | Chrome default ms | Chrome software ms | native/default | 整轮 wall s |
|---|---:|---:|---:|---:|---:|
| Vue 100 | 150.7 | 7.7 | 7.2 | 19.6× | 3.45 / 0.97 / 0.88 |
| Vue 1,000 | 1,066.7 | 42.3 | 43.0 | 25.2× | 16.86 / 1.74 / 1.74 |
| Vue 5,000 | 5,587.5 | 221.8 | 222.3 | 25.2× | 85.26 / 7.72 / 7.92 |
| Vue 10,000 | 11,347.9 | 484.4 | 530.4 | 23.4× | 166.47 / 16.34 / 17.60 |
| React 100 | 79.4 | 4.7 | 6.7 | 16.9× | 2.34 / 0.98 / 0.88 |
| React 1,000 | 575.4 | 24.3 | 28.2 | 23.7× | 10.57 / 1.59 / 1.89 |
| React 5,000 | 4,424.1 | 156.5 | 156.7 | 28.3× | 67.15 / 7.58 / 7.99 |
| React 10,000 | 12,936.8 | 396.1 | 387.6 | 32.7× | 172.54 / 15.67 / 15.29 |

**仍显著慢于 Chrome，不能宣称接近 Chrome。** 软件 Chrome 同样领先，说明缺少 GPU 不是主要解释。独立阶段计时显示 JS/bridge 是首要开销，layout 次之，纯软件 raster 较小；具体边界、重叠和插桩影响见 [diagnostics.md](diagnostics.md)。这不是“QuickJS 纯解释器慢多少倍”的隔离实验：DOM bridge、同步操作、进程模型、帧调度和渲染后端均不同。

## 实现与风险

- `JSAtom` 原生属性索引；正确的 atom 所有权、重入 generation、delete/redefine/clone。
- host 安全点的分配预算联合 GC，64 MiB / 100% retained-headroom；不自动启动单堆 GC，不移除保守 roots。
- 修复 RegExp constructor owned-reference 泄漏；匹配 React1k 销毁图 **6,240 wrappers / 6,239 edges → 0 / 0**。
- 只释放完全空闲 handle arena 数组，活地址稳定。
- 无实际 handler 时跳过 callback/descriptor 准备；保留真实访问检查和正常属性路径。
- 仅 QuickJS C VM 使用 `/O2`；**C++ facade O2 实验失败，未采用**。
- native job 与属性协议边界已通过真实 failing-before / passing-after 回归。

没有修改 callback ABI、Clipboard/Broker/security、owned/borrowed ThreadPool、HWND/PointerLock/input 合同。已知独立 **JS WeakMap dead-key 环释放问题未修复**；不以 native cppgc ephemeron 的通过覆盖它。失败诊断与更深的 O2 生命周期不确定性详见 [diagnostics.md](diagnostics.md)。

## 构建身份

| 项目 | 原始基线 | 最终 |
|---|---:|---:|
| `mini-ui.lib` bytes | 412,390,008 | 412,670,738 |
| `mini-ui-example.exe` bytes | 39,905,280 | 40,128,512 |

库增加 280,730 bytes；示例增加 223,232 bytes，不是大小优化结果。

- 基线 library SHA-256：`ac9ecbfad62e72af6e59d1c4071d6509fdf8702f9cdd5847d8bbe2b7c17ca39a`
- 最终 library SHA-256：`75a46b9cab7b1413046f64c65904ef70d254bef434364c14b2ca1db1d19dd2ba`
- 最终 regression source SHA-256：`b1bf26ff294df0e536d9732fa3f98fdc1b8a1f15a5c42976742908f95d5cc2a9`

完整命令、normal exit 和日志位置见 [final-validation.json](final-validation.json)。源码指纹见 [source-manifest.json](source-manifest.json)。

## 复现与证据

```text
python build.py --ui --ui-configure-only --out out/ui-windows-x64
ninja -f out/ui-windows-x64/build.ninja -j4 host
python benchmarks/ui/check_runtime.py --out-dir out/ui-runtime-checks
python benchmarks/ui/run.py --build --out-dir out/ui-new-native --timeout-ms 900000
python benchmarks/ui/chrome.py --out-dir out/ui-new-chrome
python benchmarks/ui/chrome.py --disable-gpu --out-dir out/ui-new-chrome-software
python benchmarks/ui/before_after.py --before benchmarks/ui/results/2026-10-22/mini-ui --after out/ui-new-native --out-dir out/ui-new-before-after
python benchmarks/ui/compare.py --mini-out out/ui-new-native --chrome-out out/ui-new-chrome --chrome-software-out out/ui-new-chrome-software --out-dir out/ui-new-comparison
```

使用项目已配置的 Windows C++/LLVM 工具链及生成器依赖；Chrome runner 依赖见 `benchmarks/ui/requirements-chrome.txt`。只安装浏览器依赖的 venv 不自动具备 Jinja 等构建生成器依赖。

- `mini-ui/`、`chrome-default/`、`chrome-software/`：24 格完整 raw/metadata/aggregate/captures。Native BMP 的无损 PNG 转换与 hash 记录在 `mini-ui/capture-manifest.json`。
- `before-after/`、`chrome-comparison/`：严格比较输出与逐操作 CSV。
- `runtime-checks-final/`：最终 passing consumer；`runtime-checks-regexp-before/`、`runtime-checks-boundaries-before/`：保留实际失败。
- `react-roots-*.json`、`phase-*.json`、`dispatch-o1-comparison.json`：分离的 root/阶段/快路径诊断。
- `experiments/`：预算与编译选项中间方案及其成功、失败 raw；不混入最终结果。
- `final-artifacts.json`、`final-validation-*.log`：最终产物大小/hash 与实际执行日志。`final-build.json` 是边界修复前的窄 O2 构建记录，不代表最终库身份；最终以 `final-validation.json` 和 `final-artifacts.json` 为准。
- `cleanup.json`：已归档测量证据后删除 26 个本轮私有试验/临时构建目录，共 5,453,050,813 文件字节（约 5.08 GiB）。正式 SDK、最终 runnable consumer、最终矩阵和原基线保留；没有删除用户文件。

Native 正确性 consumer 的 `/OPT:NOICF` 仅为覆盖 secondary ApiView；正式性能 host 使用生产 linker settings。所有运行均串行，不把编译、CDB 或另一轮压测与性能样本并跑。**未关机。**
