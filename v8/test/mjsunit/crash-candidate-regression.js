// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Flags: --allow-natives-syntax --experimental-wasm-revectorize --no-liftoff --no-turboshaft-wasm
// Also run with --trace-wasm-revectorize and, independently, with
// --turboshaft-from-maglev in a build without V8_ENABLE_MAGLEV.

// Self-contained because this checkout does not include the mjsunit harness.
(function TestAdjacentSimdStores() {
  function section(id, bytes) {
    return [id, bytes.length, ...bytes];
  }
  const body = [
    0,                       // No locals.
    0x20, 0, 0x20, 0,        // Destination/source base.
    0xfd, 0, 4, 0,           // v128.load base + 0.
    0xfd, 11, 4, 32,         // v128.store base + 32.
    0x20, 0, 0x20, 0,
    0xfd, 0, 4, 16,          // v128.load base + 16.
    0xfd, 11, 4, 48,         // v128.store base + 48.
    0x0b,
  ];
  const bytes = new Uint8Array([
    0, 97, 115, 109, 1, 0, 0, 0,
    ...section(1, [1, 0x60, 1, 0x7f, 0]),
    ...section(3, [1, 0]),
    ...section(5, [1, 0, 1]),
    ...section(7, [2, 4, 99, 111, 112, 121, 0, 0,
                     6, 109, 101, 109, 111, 114, 121, 2, 0]),
    ...section(10, [1, body.length, ...body]),
  ]);
  const {copy, memory} = new WebAssembly.Instance(
      new WebAssembly.Module(bytes)).exports;
  const view = new Uint8Array(memory.buffer);
  for (let iteration = 0; iteration < 100; ++iteration) {
    for (let i = 0; i < 32; ++i) view[i] = (i * 7 + iteration) & 255;
    view.fill(0, 32, 64);
    copy(0);
    for (let i = 0; i < 32; ++i) {
      if (view[i + 32] !== view[i]) throw new Error('SIMD copy mismatch');
    }
  }
})();

(function TestGraphBuildingOrBailout() {
  function sum(values) {
    let result = 0;
    for (let i = 0; i < values.length; ++i) result += values[i];
    return result;
  }
  %PrepareFunctionForOptimization(sum);
  const input = [1, 2, 3, 4];
  for (let i = 0; i < 10; ++i) {
    if (sum(input) !== 10) throw new Error('Warmup result mismatch');
  }
  %OptimizeFunctionOnNextCall(sum);
  if (sum(input) !== 10) throw new Error('Optimized/bailout result mismatch');
  if (sum([5, 6]) !== 11) throw new Error('Subsequent result mismatch');
})();
