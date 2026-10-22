// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/paint/timing/media_record_id.h"

#include "base/hash/hash.h"
#include "third_party/blink/renderer/core/dom/node.h"
#if !defined(MINI_UI_QUICKJS)
#include "third_party/blink/renderer/core/html/media/html_video_element.h"
#endif
#include "third_party/blink/renderer/core/layout/layout_object.h"

namespace blink {

MediaRecordId::MediaRecordId(const LayoutObject* layout, const MediaTiming* media)
    : layout_object_(layout)
    , media_timing_(media)
    , hash_(GenerateHash(layout, media))
{
}

// This hash is used as a key where previously MediaRecordId was used directly.
// That helps us avoid storing references to the GCed LayoutObject and
// MediaTiming, as that can be unsafe when using regular WTF containers. It also
// helps us avoid needlessly allocating MediaRecordId on the heap.
MediaRecordIdHash MediaRecordId::GenerateHash(const LayoutObject* layout, const MediaTiming* media)
{
    bool is_video = false;
#if !defined(MINI_UI_QUICKJS)
    if (Node* node = layout->GetNode(); IsA<HTMLVideoElement>(node))
        is_video = true;
#endif
    return base::HashInts(reinterpret_cast<MediaRecordIdHash>(layout), reinterpret_cast<MediaRecordIdHash>(is_video ? nullptr : media));
}

} // namespace blink
