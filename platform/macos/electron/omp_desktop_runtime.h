// Copyright 2026 The miniblink132 Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef PLATFORM_MACOS_ELECTRON_OMP_DESKTOP_RUNTIME_H_
#define PLATFORM_MACOS_ELECTRON_OMP_DESKTOP_RUNTIME_H_

#import <AppKit/AppKit.h>

#include "mbvip/core/mb.h"

// Application-specific host for the exported OMP Desktop web application.
// It deliberately implements only the bridge used by OMP Desktop; it is not a
// general Electron compatibility layer.
@interface OmpDesktopRuntime : NSObject

- (instancetype)initWithResourcesPath:(NSString*)resourcesPath;
- (NSString*)applicationURL;
- (void)configureWebView:(mbWebView)webView window:(NSWindow*)window;

@end

#endif  // PLATFORM_MACOS_ELECTRON_OMP_DESKTOP_RUNTIME_H_
