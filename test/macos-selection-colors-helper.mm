#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <node_api.h>
#include <string>

static napi_value Color(napi_env env, NSColor* color) {
  color = [color colorUsingColorSpace:NSColorSpace.deviceRGBColorSpace];
  napi_value result, value;
  napi_create_array_with_length(env, 4, &result);
  const CGFloat channels[] = {color.redComponent, color.greenComponent,
                             color.blueComponent, color.alphaComponent};
  for (uint32_t index = 0; index < 4; ++index) {
    napi_create_uint32(env, std::lround(channels[index] * 255), &value);
    napi_set_element(env, result, index, value);
  }
  return result;
}

static napi_value NativeColors(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg, result;
  napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr);
  bool dark = false;
  napi_get_value_bool(env, arg, &dark);
  napi_create_object(env, &result);
  NSAppearance* appearance = [NSAppearance appearanceNamed:
      dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
  [appearance performAsCurrentDrawingAppearance:^{
    napi_set_named_property(env, result, "active", Color(env, NSColor.selectedTextBackgroundColor));
    napi_set_named_property(env, result, "inactive", Color(env, NSColor.secondarySelectedControlColor));
  }];
  return result;
}

static NSView* FindRenderer(NSView* view) {
  if ([NSStringFromClass(view.class) isEqualToString:@"MiniElectronRendererView"])
    return view;
  for (NSView* child in view.subviews)
    if (NSView* renderer = FindRenderer(child)) return renderer;
  return nil;
}

static napi_value Snapshot(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  char title[256], output[1024];
  napi_get_value_string_utf8(env, args[0], title, sizeof(title), nullptr);
  napi_get_value_string_utf8(env, args[2], output, sizeof(output), nullptr);
  NSView* view = nil;
  for (NSWindow* window in NSApp.windows) {
    if ([window.title isEqualToString:[NSString stringWithUTF8String:title]]) {
      view = FindRenderer(window.contentView);
      break;
    }
  }
  if (!view) {
    napi_throw_error(env, nullptr, "No matching native renderer view");
    return nullptr;
  }
  // Capture the actual native view's Blink frame, not DOM computed styles or
  // a synthesized color. AppKit supplies the renderer view's flipped context.
  NSBitmapImageRep* bitmap = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
  [view cacheDisplayInRect:view.bounds toBitmapImageRep:bitmap];
  if (!bitmap || ![[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}]
        writeToFile:[NSString stringWithUTF8String:output] atomically:YES]) {
    napi_throw_error(env, nullptr, "Cannot persist native selection screenshot");
    return nullptr;
  }
  const double scaleX = bitmap.pixelsWide / view.bounds.size.width;
  const double scaleY = bitmap.pixelsHigh / view.bounds.size.height;
  uint32_t count = 0;
  napi_get_array_length(env, args[1], &count);
  napi_value result;
  napi_create_array_with_length(env, count, &result);
  for (uint32_t box = 0; box < count; ++box) {
    napi_value rectangle, value;
    napi_get_element(env, args[1], box, &rectangle);
    double components[4];
    for (uint32_t channel = 0; channel < 4; ++channel) {
      napi_get_element(env, rectangle, channel, &value);
      napi_get_value_double(env, value, &components[channel]);
    }
    const int left = std::max(0, static_cast<int>(std::ceil(components[0] * scaleX)));
    const int top = std::max(0, static_cast<int>(std::ceil(components[1] * scaleY)));
    const int right = std::min(static_cast<int>(bitmap.pixelsWide),
        static_cast<int>(std::floor((components[0] + components[2]) * scaleX)));
    const int bottom = std::min(static_cast<int>(bitmap.pixelsHigh),
        static_cast<int>(std::floor((components[1] + components[3]) * scaleY)));
    std::map<uint32_t, size_t> histogram;
    for (int y = top; y < bottom; ++y) {
      for (int x = left; x < right; ++x) {
        NSColor* color = [[bitmap colorAtX:x y:y] colorUsingColorSpace:NSColorSpace.deviceRGBColorSpace];
        const uint32_t rgb = (static_cast<uint32_t>(std::lround(color.redComponent * 255)) << 16)
            | (static_cast<uint32_t>(std::lround(color.greenComponent * 255)) << 8)
            | static_cast<uint32_t>(std::lround(color.blueComponent * 255));
        ++histogram[rgb];
      }
    }
    if (histogram.empty()) {
      napi_throw_error(env, nullptr, "Selection sample lies outside the native view");
      return nullptr;
    }
    const auto dominant = std::max_element(histogram.begin(), histogram.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });
    napi_value pixel;
    napi_create_array_with_length(env, 3, &pixel);
    for (uint32_t channel = 0; channel < 3; ++channel) {
      napi_create_uint32(env, (dominant->first >> (16 - channel * 8)) & 255, &value);
      napi_set_element(env, pixel, channel, value);
    }
    napi_set_element(env, result, box, pixel);
  }
  return result;
}

NAPI_MODULE_INIT() {
  napi_value function;
  napi_create_function(env, "nativeColors", NAPI_AUTO_LENGTH, NativeColors, nullptr, &function);
  napi_set_named_property(env, exports, "nativeColors", function);
  napi_create_function(env, "snapshot", NAPI_AUTO_LENGTH, Snapshot, nullptr, &function);
  napi_set_named_property(env, exports, "snapshot", function);
  return exports;
}
