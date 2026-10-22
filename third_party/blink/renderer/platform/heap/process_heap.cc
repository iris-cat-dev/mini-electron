// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/heap/process_heap.h"
#if defined(MINI_UI_QUICKJS)
#include "runtime/ui/cppgc_platform.h"
#else
#include "gin/public/cppgc.h"
#endif

namespace blink {

// static
void ProcessHeap::Init()
{
#if defined(MINI_UI_QUICKJS)
    mini_ui::InitializeCppgcProcess();
#else
    gin::InitializeCppgcFromV8Platform();
#endif
}

} // namespace blink
