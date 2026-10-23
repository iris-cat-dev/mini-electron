/*
 *  Copyright (C) 2012 Samsung Electronics
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 */

#include "third_party/blink/renderer/modules/vibration/vibration_controller.h"

#include "third_party/blink/renderer/bindings/core/v8/v8_union_unsignedlong_unsignedlongsequence.h"

namespace blink {

namespace {

// Maximum number of entries in a vibration pattern.
constexpr unsigned kVibrationPatternLengthMax = 99;

// Maximum duration of a vibration is 10 seconds.
constexpr unsigned kVibrationDurationMsMax = 10000;

}  // namespace

// static
VibrationController::VibrationPattern
VibrationController::SanitizeVibrationPattern(
    const VibrationPattern& pattern) {
  VibrationPattern sanitized = pattern;
  wtf_size_t length = sanitized.size();

  if (length > kVibrationPatternLengthMax) {
    sanitized.Shrink(kVibrationPatternLengthMax);
    length = kVibrationPatternLengthMax;
  }

  for (wtf_size_t i = 0; i < length; ++i) {
    if (sanitized[i] > kVibrationDurationMsMax)
      sanitized[i] = kVibrationDurationMsMax;
  }

  // A trailing pause has no observable effect.
  if (length && !(length % 2))
    sanitized.pop_back();

  return sanitized;
}

// static
VibrationController::VibrationPattern
VibrationController::SanitizeVibrationPattern(
    const V8UnionUnsignedLongOrUnsignedLongSequence* input) {
  switch (input->GetContentType()) {
    case V8UnionUnsignedLongOrUnsignedLongSequence::ContentType::kUnsignedLong: {
      VibrationPattern pattern;
      pattern.push_back(input->GetAsUnsignedLong());
      return SanitizeVibrationPattern(pattern);
    }
    case V8UnionUnsignedLongOrUnsignedLongSequence::ContentType::
        kUnsignedLongSequence:
      return SanitizeVibrationPattern(input->GetAsUnsignedLongSequence());
  }
  NOTREACHED();
}

}  // namespace blink
