# QuickJS 属性槽位提示：React 大规模重排优化

## 结论与边界

保留经过槽位校验、实际内联的普通字段读取路径。生产代码仅修改 `third_party/quickjs/quickjs.c`；不修改 React/Vue、负载、DOM、callback ABI、realm/security、GC roots、Clipboard、ThreadPool 或 HWND/input。

React 10k 首轮 commit 降低 **22.84%**，独立反向复测降低 **23.48%**；keyed reorder 分别降低 **31.66% / 31.89%**。整轮 wall 分别降低约 **15.4% / 14.4%**。Vue 10k commit 两轮下降，但 wall 一轮增加、一轮下降，**不声称 Vue 大场景整轮有稳定收益**。React 100 首轮 commit 增加 4.82%，复测反而减少 1.95%，小场景不作稳定改善或稳定退化结论。

## 实现与安全条件

`OP_get_field` / `OP_get_field2` 使用私有 `js_get_field()`。仅普通、非 exotic 对象进入提示路径；其他对象沿用原逻辑。

- 每个 `JSContext` 内有 256 个 `uint32_t` 提示，额外 **1,024 bytes/context**，没有新增单独分配。
- 提示仅保存属性槽位索引加一；不保存 atom 引用、对象、shape 指针、值或负查找结果。context 的既有清零初始化令 0 表示未命中。
- 每次先检查索引在当前 shape 范围内，再检查当前槽位的 atom 和 data-property flags，成功才读取当前 value 并执行原 `JS_DupValue`。
- 删除／重排／compact／原型变化／atom 复用不会让旧提示直接返回旧值；不匹配就重新查找。getter、varref、autoinit、host hook 和 Proxy 仍由通用路径处理。
- 只有新 helper 在 Windows 使用 `__forceinline`；其他编译优化级别未变。Windows 现有 `force_inline` 宏只有普通 `inline`。第一、二版的 linker map 有独立 helper，最终版不再出现该符号。
- 这不是 JIT 或完整 shape inline cache，不改字节码格式；动态 bracket 读取不在本轮优化范围。

## 语义验证

`runtime_checks.cc` 增加真实 JS 字段读取回归：多 shape、属性删除／重新定义、getter receiver 与自删除、原型替换、shape compaction、超过提示表容量的不同字段交替读取、方法 receiver、Proxy get/throw/revoke、null/undefined、临时对象返回值生命周期。旧库和候选库均通过；这是防退化验证，不是失败前／通过后的 bug 修复声明。已有 native accessor/interceptor、cross-realm、GC 与 Blob 检查继续运行。

## 固定负载与首轮全矩阵

Vue 3.5.13、React 18.3.1，原生产资产和完整 keyed reverse workload 未改。100/1k/5k/10k；2 warmups + 7 measured iterations；1280×720、DPR 1、900000 ms phase timeout。正式性能过程串行，未与编译／其他压测并跑。

commit 为每次迭代七操作之和的中位数，**不是各操作中位数之和**。wall 为整轮 workload wall；正 reduction 表示耗时下降。

| 场景 | before commit ms | after commit ms | reduction | wall s before → after |
|---|---:|---:|---:|---:|
| vue-100 | 141.7 | 135.0 | 4.73% | 3.525 → 2.950 |
| vue-1000 | 1078.2 | 935.4 | 13.24% | 17.364 → 15.042 |
| vue-5000 | 4990.9 | 4673.3 | 6.36% | 79.634 → 75.678 |
| vue-10000 | 10068.7 | 9829.2 | 2.38% | 155.047 → 157.349 |
| react-100 | 80.9 | 84.8 | -4.82% | 2.722 → 2.597 |
| react-1000 | 582.8 | 539.7 | 7.40% | 11.206 → 10.569 |
| react-5000 | 4412.8 | 3740.4 | 15.24% | 67.448 → 61.543 |
| react-10000 | 12972.8 | 10010.2 | 22.84% | 175.244 → 148.243 |

全部八格 complete，正常退出；每格 64 个 DOM transition assertions，frame generations 均 126→126。原始数据见 `before/`、`after/` 和 `before-after/`。

## 独立反向复测

最终候选再跑一次，然后旧库再跑一次，每格新进程、仍为 2+7 iterations；不把两轮迭代混池。这是在 A→简单快路径→槽位提示→最终内联版之后进行的 B2→A2 检验，不冒称无中间实验的严格 ABBA。

| 场景 | commit ms before → after | reduction | wall s before → after |
|---|---:|---:|---:|
| vue-100 | 142.2 → 135.6 | 4.64% | 3.330 → 3.235 |
| vue-10000 | 9940.4 → 9415.8 | 5.28% | 156.187 → 155.225 |
| react-100 | 81.9 → 80.3 | 1.95% | 2.729 → 2.668 |
| react-10000 | 12931.8 → 9895.6 | 23.48% | 170.168 → 145.623 |

React 10k reorder：首轮 **9032.8→6173.3 ms**，复测 **8999.6→6130.0 ms**。React 5k 首轮 **2445.1→1767.8 ms**。收益集中在被指定的重排路径，而非删减绘制。

两个独立进程对不足以提供稳定置信区间。内存是进程 private bytes，不等于 live heap；本次不是内存优化，提示本身反而增加固定 1 KiB/context。

## 未保留的中间方案

1. 只在普通自有数据属性上调用小 helper、没有槽位提示：React 5k commit +0.10%，10k −1.00%，没有足够收益。
2. 加槽位提示但没有显式 Windows 强制内联：React 5k −7.72%、10k −10.23%；被实测更好的最终版替代。

两版也执行了完整矩阵与语义检查，记录保存在 `experiments/`；不是只报告最佳一次运行。

## 解释限制与下一方向

上轮 native 的 `JS_GetPropertyImpl` / `JS_CallInternal` 样本是动机，不是本轮优化后的比例。普通读取移入解释器后，函数级采样归属本来就会变化，不能把独立 helper 消失解释为成本归零。本文没有新独占 CPU 采样或 hit-rate 声明。

React `Ci` 的 host-sibling 反复扫描和真实 reverse 操作仍然存在；本次降低单次字段读取成本，没有声称把遍历算法改成线性。下一步需要重新采样剩余 VM/属性写入/DOM 工作，不能用旧热点百分比直接决定下一项修改。已知 C++ facade O2 生命周期问题及独立 JS WeakMap dead-key cycle 问题未在本轮修复。

## 刷新后的真实 Chrome 对照

本轮重新启动已安装的 headed Google Chrome，使用同一框架／数据／2+7 iterations；默认和 `--disable-gpu` 各八格，均正常完成。不是沿用上一轮 Chrome 时间。

| 场景 | native commit ms | Chrome 默认 ms | Chrome 禁用 GPU ms | native / 默认 |
|---|---:|---:|---:|---:|
| vue 100 | 135.0 | 7.3 | 7.1 | 18.5× |
| vue 1000 | 935.4 | 41.1 | 40.7 | 22.8× |
| vue 5000 | 4673.3 | 231.3 | 227.2 | 20.2× |
| vue 10000 | 9829.2 | 469.2 | 537.0 | 20.9× |
| react 100 | 84.8 | 4.6 | 4.8 | 18.4× |
| react 1000 | 539.7 | 23.9 | 25.3 | 22.6× |
| react 5000 | 3740.4 | 131.7 | 165.1 | 28.4× |
| react 10000 | 10010.2 | 373.8 | 439.9 | 26.8× |

React 10k 目前仍需默认 Chrome **26.8 倍** commit 时间。上轮报告的 34.6× 是上轮独立 Chrome 控制结果，不能把浏览器波动当成本轮优化收益；优化收益以本报告同 native baseline 的配对数据为准。禁用 GPU 的 Chrome 仍显著更快。

## 产物与实际验证

- 正式 Ninja `host` 构建成功；正式库重新链接的 runtime regression 成功。
- 正式 SDK 额外跑 Vue/React 1k：commit **953.8 / 528.0 ms**，wall **16.082 / 10.121 s**，各 126 frames。独立列在 `production-smoke/`，不拿它替换矩阵中的时间。
- 完整矩阵与复测共 **12 对 native 截图 RGB 像素完全相同**。实际查看了最终 React 10k native 和 Chrome dashboard，均显示 10000 rows / 1000 selected / 64 assertions；不声称 native 和 Chrome 彼此像素相同。
- 完整矩阵候选 host 与正式库重新链接 host 的全部 **10 个 PE section 内容相同**，见 `executable-section-identity.json`；两个 archive 的 SHA 不同，不冒称同一个文件。
- Baseline SDK SHA-256：`c02cf3da4a07981541d2c0039c6ff752a1c30d029ada03adc118e66af61f7b2d`。
- 最终候选 SDK SHA-256：`893fa6f28961e60ef4ccfc1e6699ca1b19ba6cef588f6a23693a12d8bfa6df42`。
- 正式 `out/ui-windows-x64/mini-ui.lib` SHA-256：`535ff5f4b421d9ec124c10aa5e6cf80bc3ed62eb7fbe647c2e8a0ab8ecf81c62`，412,670,380 bytes。

原始 JSON/CSV、run metadata、输入/产物 identity、PNG captures、六次 regression 结果及压缩构建日志均留存。完整源码变更范围是 VM 一个文件、回归一个文件以及 benchmark 文档／记录；没有清理或改动其他 owner 的历史源文件。

## 复现

```text
ninja -f out/ui-windows-x64/build.ninja -j4 host
python benchmarks/ui/check_runtime.py --out-dir out/ui-slot-runtime-checks
python benchmarks/ui/run.py --build --timeout-ms 900000 --out-dir out/ui-slot-native
python benchmarks/ui/chrome.py --out-dir out/ui-slot-chrome
python benchmarks/ui/chrome.py --disable-gpu --out-dir out/ui-slot-chrome-software
python benchmarks/ui/before_after.py --before benchmarks/ui/results/2026-10-22-property-slots/before --after out/ui-slot-native --out-dir out/ui-slot-delta
python benchmarks/ui/compare.py --mini-out out/ui-slot-native --chrome-out out/ui-slot-chrome --chrome-software-out out/ui-slot-chrome-software --out-dir out/ui-slot-comparison
```

Ninja 不在 PATH 时使用 `validation/ui-field-final-validation/validation.json` 中的真实 VS Ninja 路径；Chrome runner 使用已安装 websocket-client 的 benchmark venv。各命令串行执行。原始记录路径保留执行时的名称；临时目录清理后读取本报告中的归档副本。机器保持开机。

## 清理与文件范围

已删除本轮 27 个自建临时目录，释放约 **2.116 GiB**。正式 SDK、最后的 consumer host、正式 runtime-check 结果和本报告证据保留；此前产物／报告未删除。详见 [cleanup.json](cleanup.json)。

源码及 baseline fingerprints 见 [source-manifest.json](source-manifest.json)，正式产物见 [build-identity.json](build-identity.json)，像素等价证据见 [capture-comparison.json](capture-comparison.json)。生产实现只保留最终槽位提示＋内联版，中间方案没有并存分支。
