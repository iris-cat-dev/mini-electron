// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#import <AppKit/AppKit.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "base/at_exit.h"
#include "base/check.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "skia/ext/skia_utils_mac.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/color/color_id.h"
#include "ui/color/color_mixer.h"
#include "ui/color/color_provider.h"
#include "ui/color/color_provider_key.h"
#include "ui/color/color_provider_manager.h"
#include "ui/color/color_recipe.h"
#include "ui/gfx/color_analysis.h"
#include "ui/native_theme/native_theme.h"
#include "ui/native_theme/native_theme_observer.h"

namespace {

// Observe the real native theme on a renderer-like sequence while AppKit
// delivers appearance changes on the main thread. No global defaults change.
class ThemeRecorder : public ui::NativeThemeObserver {
 public:
  explicit ThemeRecorder(scoped_refptr<base::SingleThreadTaskRunner> runner)
      : runner_(std::move(runner)) {}

  void Start() {
    CHECK(runner_->RunsTasksInCurrentSequence());
    theme_ = ui::NativeTheme::GetInstanceForNativeUi();
    auto& manager = ui::ColorProviderManager::Get();
    manager.AppendColorProviderInitializer(base::BindRepeating(
        [](std::atomic<SkColor>* palette, ui::ColorProvider* provider,
           const ui::ColorProviderKey&) {
          provider->AddMixer()[ui::kColorAccent] = {palette->load()};
        }, base::Unretained(&palette_color)));
    CHECK_EQ(manager.GetColorProviderFor(ui::ColorProviderKey())->GetColor(ui::kColorAccent),
             palette_color.load());
    theme_->AddObserver(this);
    dark = theme_->ShouldUseDarkColors();
    ready = true;
  }

  void Stop() {
    CHECK(runner_->RunsTasksInCurrentSequence());
    theme_->RemoveObserver(this);
    ui::ColorProviderManager::Get().ResetColorProviderInitializerList();
    stopped = true;
  }

  void OnNativeThemeUpdated(ui::NativeTheme* theme) override {
    CHECK(runner_->RunsTasksInCurrentSequence());
    CHECK_EQ(theme, theme_);
    auto& manager = ui::ColorProviderManager::Get();
    // A stable key must not retain an embedder color from before invalidation.
    CHECK_EQ(manager.GetColorProviderFor(ui::ColorProviderKey())->GetColor(ui::kColorAccent),
             palette_color.load());
    const auto key = theme->GetColorProviderKey(nullptr);
    CHECK((key.color_mode == ui::ColorProviderKey::ColorMode::kDark) ==
          theme->ShouldUseDarkColors());
    const auto color = manager.GetColorProviderFor(key)->GetColor(ui::kColorCssSystemWindow);
    CHECK_EQ(SkColorGetA(color), 255u);
    const auto brightness = SkColorGetR(color) + SkColorGetG(color) + SkColorGetB(color);
    CHECK((brightness < 3 * 128) == theme->ShouldUseDarkColors());
    CHECK_EQ(ui::NativeTheme::GetInstanceForWeb()->ShouldUseDarkColors(),
             theme->ShouldUseDarkColors());
    dark = theme->ShouldUseDarkColors();
    ++updates;
  }

  std::atomic<bool> ready{false};
  std::atomic<bool> stopped{false};
  std::atomic<bool> dark{false};
  std::atomic<int> updates{0};
  std::atomic<SkColor> palette_color{SK_ColorBLUE};

 private:
  scoped_refptr<base::SingleThreadTaskRunner> runner_;
  ui::NativeTheme* theme_ = nullptr;
};

template <typename Predicate>
void PumpUntil(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!predicate()) {
    CHECK(std::chrono::steady_clock::now() < deadline)
        << "Native theme transition timed out";
    [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
  }
}

}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
    NSAppearance* original = NSApp.appearance;
    NSApp.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
    // AppKit resolves effectiveAppearance lazily on its main thread.
    NSAppearanceName initial_appearance =
        [NSApp.effectiveAppearance bestMatchFromAppearancesWithNames:@[
          NSAppearanceNameAqua, NSAppearanceNameDarkAqua
        ]];
    CHECK([initial_appearance isEqual:NSAppearanceNameAqua]);

    base::Thread owner("NativeThemeOwner");
    CHECK(owner.Start());
    ThemeRecorder recorder(owner.task_runner());
    CHECK(owner.task_runner()->PostTask(
        FROM_HERE, base::BindOnce(&ThemeRecorder::Start, base::Unretained(&recorder))));
    PumpUntil([&] { return recorder.ready.load(); });
    CHECK(!recorder.dark.load())
        << "configured=" << [NSApp.appearance.name UTF8String]
        << " effective=" << [NSApp.effectiveAppearance.name UTF8String];

    for (int transition = 0; transition < 20; ++transition) {
      const bool dark = transition % 2 == 0;
      const int previous = recorder.updates.load();
      recorder.palette_color = dark ? SK_ColorRED : SK_ColorBLUE;
      NSApp.appearance = [NSAppearance appearanceNamed:
          dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
      PumpUntil([&] {
        return recorder.updates.load() > previous && recorder.dark.load() == dark;
      });
    }

    CHECK(owner.task_runner()->PostTask(
        FROM_HERE, base::BindOnce(&ThemeRecorder::Stop, base::Unretained(&recorder))));
    PumpUntil([&] { return recorder.stopped.load(); });
    NSApp.appearance = original;
    owner.Stop();

    NSBitmapImageRep* image_rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:nullptr pixelsWide:2 pixelsHigh:1
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
        colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:8 bitsPerPixel:32];
    CHECK(image_rep);
    constexpr unsigned char rgba[] = {255, 0, 0, 255, 0, 0, 255, 255};
    std::memcpy(image_rep.bitmapData, rgba, sizeof(rgba));
    image_rep = [image_rep bitmapImageRepByRetaggingWithColorSpace:NSColorSpace.sRGBColorSpace];
    const SkBitmap converted = skia::NSImageRepToSkBitmap(image_rep, NSMakeSize(2, 1), true);
    CHECK_EQ(converted.getColor(0, 0), SK_ColorRED);
    CHECK_EQ(converted.getColor(1, 0), SK_ColorBLUE);

    SkBitmap bitmap;
    bitmap.allocN32Pixels(3, 2);
    bitmap.eraseColor(SK_ColorRED);
    CHECK_EQ(color_utils::CalculateKMeanColorOfBitmap(bitmap), SK_ColorRED);
    std::printf("PASS: %d native appearance notifications, owner-sequence delivery, "
                "web theme propagation, color cache invalidation, native bitmap channels, dominant color\n",
                recorder.updates.load());
  }
  return 0;
}
