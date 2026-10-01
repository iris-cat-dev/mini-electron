// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef PLATFORM_MACOS_ELECTRON_OMP_DESKTOP_RUNTIME_H_
#define PLATFORM_MACOS_ELECTRON_OMP_DESKTOP_RUNTIME_H_

#import <AppKit/AppKit.h>

#include "runtime/engine/public/engine_api.h"

// Application-specific host for the exported OMP Desktop web application.
// It deliberately implements only the bridge used by OMP Desktop; it is not a
// general Electron compatibility layer.
@interface OmpDesktopRuntime : NSObject

- (instancetype)initWithResourcesPath:(NSString*)resourcesPath;
- (NSString*)applicationURL;
- (void)configureWebView:(mini_electron_web_view)webView window:(NSWindow*)window;

@end

#endif  // PLATFORM_MACOS_ELECTRON_OMP_DESKTOP_RUNTIME_H_
