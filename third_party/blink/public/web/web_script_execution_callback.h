// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_SCRIPT_EXECUTION_CALLBACK_H_
#define THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_SCRIPT_EXECUTION_CALLBACK_H_

#if !defined(MINI_UI_QUICKJS)
#include <optional>
#endif
#include "base/functional/callback.h"
#if defined(MINI_UI_QUICKJS)
#include "v8/include/v8-forward.h"
#endif

namespace base {
class TimeTicks;
#if !defined(MINI_UI_QUICKJS)
class Value;
#endif
}

namespace blink {

#if defined(MINI_UI_QUICKJS)
// Returns the actual last result. JavaScript undefined and null are nonempty;
// exceptions, awaited rejection and invalidated contexts return an empty Local.
// The callback runs once inside a HandleScope. Persist the result explicitly
// when it must outlive that callback scope.
// The timestamp is evaluation start time, or zero for an invalidated context.
using WebScriptExecutionCallback = base::OnceCallback<void(v8::Local<v8::Value>, base::TimeTicks)>;
#else
// Non-nullopt `base::Value` is passed to the callback if
// -`WantResultOption::kWantResult` or
//  `WantResultOption::kWantResultDateAndRegexpAllowed` is used,
// - The script evaluation was successful, and
// - V8ValueConverter successfully converts the resulting `v8::Local<v8::Value>`
//   to `base::Value`.
// Otherwise, `std::nullopt` is passed.
//
// `base::TimeTicks` is the time when the script evaluation is started.
//
// When multiple scripts are given, the result of the last script is passed.
// This semantics is inherited from `ScriptInjection` (the only non-test caller
// that executes multiple scripts at once): "We use the final result, since it
// is the most meaningful (the result after running all scripts). Additionally,
// the final script can reference values from the previous scripts, so could
// return them if desired."
//
// TODO(https://crbug.com/1323953): Consider improving this documentation,
// because methods called subsequently after `WebScriptExecutionCallback`s don't
// necessarily have the `WebScriptExecutionCallback` signature and thus this
// comment is a bit hard to reach.
//
// TODO(https://crbug.com/1323953): Consider introducing a specific type for
// representing the arguments and invariants.
using WebScriptExecutionCallback = base::OnceCallback<void(std::optional<base::Value>, base::TimeTicks)>;

#endif
} // namespace blink

#endif // THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_SCRIPT_EXECUTION_CALLBACK_H_
