// Copyright 2026 The miniblink132 Authors
// Use of this source code is governed by the Apache-2.0 license.

#import "platform/macos/electron/omp_desktop_runtime.h"

#import <Foundation/Foundation.h>

#include <cstdio>
#include <string>

extern "C" void mbMacSetStoragePaths(mbWebView webView,
                                     const char* cookiePath,
                                     const char* localStoragePath);

namespace {

constexpr char kApplicationOrigin[] = "http://127.0.0.1:17913";
constexpr int kBridgeMessage = 0x4f4d50;

NSString* JsonString(id value) {
  if (!value)
    value = [NSNull null];
  NSError* error = nil;
  NSData* data =
      [NSJSONSerialization dataWithJSONObject:value
                                      options:NSJSONWritingFragmentsAllowed
                                        error:&error];
  if (!data) {
    NSDictionary* fallback = @{
      @"ok" : @NO,
      @"error" : error.localizedDescription ?: @"JSON serialization failed",
    };
    data = [NSJSONSerialization dataWithJSONObject:fallback options:0 error:nil];
  }
  return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
}

NSDictionary* JsonObject(NSString* value, NSError** error) {
  NSData* data = [value dataUsingEncoding:NSUTF8StringEncoding];
  id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:error];
  return [object isKindOfClass:[NSDictionary class]] ? object : nil;
}

NSString* ReadTextFile(NSString* path) {
  return [NSString stringWithContentsOfFile:path
                                   encoding:NSUTF8StringEncoding
                                      error:nil] ?: @"";
}

NSString* MimeType(NSString* path) {
  static NSDictionary<NSString*, NSString*>* types;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    types = @{
      @"css" : @"text/css",
      @"gif" : @"image/gif",
      @"html" : @"text/html",
      @"ico" : @"image/x-icon",
      @"jpeg" : @"image/jpeg",
      @"jpg" : @"image/jpeg",
      @"js" : @"text/javascript",
      @"json" : @"application/json",
      @"map" : @"application/json",
      @"mjs" : @"text/javascript",
      @"png" : @"image/png",
      @"svg" : @"image/svg+xml",
      @"ttf" : @"font/ttf",
      @"txt" : @"text/plain",
      @"wasm" : @"application/wasm",
      @"webp" : @"image/webp",
      @"woff" : @"font/woff",
      @"woff2" : @"font/woff2",
    };
  });
  return types[path.pathExtension.lowercaseString] ?: @"application/octet-stream";
}

NSDictionary* DefaultSettings() {
  return @{
    @"releaseChannel" : @"stable",
    @"notifications" : @{ @"playSound" : @YES },
    @"daemon" : @{
      @"manageBuiltInDaemon" : @YES,
      @"keepRunningAfterQuit" : @NO,
    },
    @"window" : @{ @"closeBehavior" : @"quit" },
  };
}

NSDictionary* MergeDictionary(NSDictionary* base, NSDictionary* patch) {
  NSMutableDictionary* result = [base mutableCopy];
  [patch enumerateKeysAndObjectsUsingBlock:^(id key, id value, BOOL* stop) {
    id current = result[key];
    if ([current isKindOfClass:[NSDictionary class]] &&
        [value isKindOfClass:[NSDictionary class]]) {
      result[key] = MergeDictionary(current, value);
    } else if (value != [NSNull null]) {
      result[key] = value;
    }
  }];
  return result;
}

NSString* BridgeScript(NSString* loginShell) {
  NSString* shellJSON = JsonString(loginShell ?: @"/bin/zsh");
  static const char kSource[] = R"JS(
(() => {
  if (window.paseoDesktop || typeof window.mbQuery !== 'function') return;

  const listeners = new Map();
  const callNative = (scope, method, args) => new Promise((resolve, reject) => {
    window.mbQuery(%d, JSON.stringify({ scope, method, args: args ?? null }), (_message, response) => {
      try {
        const envelope = JSON.parse(response);
        if (envelope.ok) resolve(envelope.value);
        else reject(new Error(envelope.error || 'OMP Desktop native request failed'));
      } catch (error) {
        reject(error);
      }
    });
  });
  const on = (event, handler) => {
    let handlers = listeners.get(event);
    if (!handlers) listeners.set(event, handlers = new Set());
    handlers.add(handler);
    return Promise.resolve(() => handlers.delete(handler));
  };
  window.__ompDesktopDispatch = (event, payload) => {
    for (const handler of listeners.get(event) || []) handler(payload);
  };

  Object.defineProperty(window, 'paseoDesktop', {
    configurable: false,
    enumerable: true,
    writable: false,
    value: {
      platform: 'darwin',
      loginShell: %@,
      invoke: (command, args) => callNative('invoke', command, args),
      getPendingOpenProject: () => Promise.resolve(null),
      agentNavigation: { ready: () => Promise.resolve(null) },
      remoteSsh: {
        start: (input) => callNative('remoteSsh', 'start', input),
        writeInput: (input) => callNative('remoteSsh', 'writeInput', input),
        cancel: (input) => callNative('remoteSsh', 'cancel', input),
        getProfile: (serverId) => callNative('remoteSsh', 'getProfile', { serverId }),
        saveProfile: (input) => callNative('remoteSsh', 'saveProfile', input),
        removeProfile: (serverId) => callNative('remoteSsh', 'removeProfile', { serverId }),
      },
      events: { on },
      window: {
        openNew: (options) => callNative('window', 'openNew', options),
        closeChoice: {
          ready: () => Promise.resolve(null),
          respond: () => Promise.resolve(false),
        },
        getCurrentWindow: () => ({
          toggleMaximize: () => callNative('window', 'toggleMaximize'),
          setFullscreen: (fullscreen) => callNative('window', 'setFullscreen', { fullscreen }),
          isFullscreen: () => callNative('window', 'isFullscreen'),
          isMaximized: () => callNative('window', 'isMaximized'),
          beginWindowDrag: () => Promise.resolve(),
          moveWindowDrag: () => {},
          endWindowDrag: () => {},
          updateWindowControls: () => Promise.resolve(),
          onResized: (handler) => on('window:resized', handler),
          setBadgeCount: (count) => callNative('window', 'setBadgeCount', { count }),
        }),
      },
      dialog: {
        ask: (message, options) => callNative('dialog', 'ask', { message, options }),
        askWithCheckbox: (message, options) =>
          callNative('dialog', 'askWithCheckbox', { message, options }),
        open: (options) => callNative('dialog', 'open', options),
      },
      notification: {
        isSupported: () => Promise.resolve(false),
        sendNotification: () => Promise.resolve(false),
      },
      opener: { openUrl: (url) => callNative('opener', 'openUrl', { url }) },
      editor: {
        listTargets: () => Promise.resolve([]),
        openTarget: (input) => callNative('editor', 'openTarget', input),
      },
      webUtils: {
        getPathForFile: (file) => file?.path || file?.webkitRelativePath || file?.name || '',
      },
      menu: {
        showContextMenu: () => Promise.resolve(null),
        setCapturingShortcut: () => Promise.resolve(),
      },
      browser: {
        setShortcutPolicy: () => Promise.resolve(),
        registerAttachedBrowser: () => Promise.resolve(),
        unregisterWorkspaceBrowser: () => Promise.resolve(),
        setWorkspaceActiveBrowser: () => Promise.resolve(),
        focus: () => Promise.resolve(false),
        openDevTools: () => Promise.resolve({ ok: false, error: 'DevTools unavailable' }),
        clearProfile: () => Promise.resolve(),
        executeAutomationCommand: () => Promise.reject(new Error('Browser automation unavailable')),
        captureElement: () => Promise.resolve(null),
        copyElement: () => Promise.resolve(false),
      },
    },
  });
})();
)JS";
  NSString* source = [NSString stringWithUTF8String:kSource];
  NSString* result = [NSString stringWithFormat:source, kBridgeMessage, shellJSON];
#if !__has_feature(objc_arc)
  [shellJSON release];
#endif
  return result;
}

}  // namespace

@interface OmpDesktopRuntime () {
  NSString* _resourcesPath;
  NSString* _appDistPath;
  NSString* _nodePath;
  NSString* _cliPath;
  NSString* _ompPath;
  NSString* _cliShimPath;
  NSString* _homePath;
  NSString* _listenAddress;
  NSWindow* _window;
}
@end

@implementation OmpDesktopRuntime

- (instancetype)initWithResourcesPath:(NSString*)resourcesPath {
  self = [super init];
  if (self) {
    _resourcesPath = [[resourcesPath stringByStandardizingPath] copy];
    _appDistPath = [[_resourcesPath stringByAppendingPathComponent:@"app-dist"] copy];
    _nodePath = [[_resourcesPath stringByAppendingPathComponent:@"node/bin/node"] copy];
    _cliPath = [[_resourcesPath
        stringByAppendingPathComponent:@"backend/node_modules/@omp-desktop/cli/dist/index.js"] copy];
    _ompPath = [[_resourcesPath stringByAppendingPathComponent:@"bin/omp"] copy];
    _cliShimPath = [[_resourcesPath stringByAppendingPathComponent:@"bin/omp-desktop"] copy];
    NSDictionary* environment = NSProcessInfo.processInfo.environment;
    NSString* configuredHome = environment[@"OMP_DESKTOP_HOME"];
    NSString* home = [configuredHome length]
                         ? configuredHome
                         : [NSHomeDirectory()
                               stringByAppendingPathComponent:@".omp-desktop"];
    _homePath = [home copy];
    NSString* configuredListen = environment[@"PASEO_LISTEN"];
    NSString* listen = [configuredListen length] ? configuredListen
                                                 : @"127.0.0.1:6770";
    _listenAddress = [listen copy];
  }
  return self;
}

- (void)dealloc {
#if !__has_feature(objc_arc)
  [_resourcesPath release];
  [_appDistPath release];
  [_nodePath release];
  [_cliPath release];
  [_ompPath release];
  [_cliShimPath release];
  [_homePath release];
  [_listenAddress release];
  [super dealloc];
#endif
}

- (NSString*)applicationURL {
  return [NSString stringWithUTF8String:kApplicationOrigin];
}

- (NSDictionary*)taskEnvironment {
  NSMutableDictionary* environment = [NSProcessInfo.processInfo.environment mutableCopy];
  NSString* binaryPath = [_resourcesPath stringByAppendingPathComponent:@"bin"];
  NSString* oldPath = environment[@"PATH"] ?: @"/usr/bin:/bin:/usr/sbin:/sbin";
  environment[@"PATH"] = [NSString stringWithFormat:@"%@:%@", binaryPath, oldPath];
  environment[@"OMP_DESKTOP_HOME"] = _homePath;
  environment[@"PASEO_LISTEN"] = _listenAddress;
  environment[@"PASEO_DESKTOP_MANAGED"] = @"1";
  environment[@"PASEO_WEB_UI_ENABLED"] = @"false";
  environment[@"PASEO_CLI"] = _cliShimPath;
  environment[@"OMP_COMMAND"] = _ompPath;
  return environment;
}

- (NSString*)runCli:(NSArray<NSString*>*)arguments error:(NSString**)errorText {
  if (![[NSFileManager defaultManager] isExecutableFileAtPath:_nodePath] ||
      ![[NSFileManager defaultManager] fileExistsAtPath:_cliPath]) {
    if (errorText)
      *errorText = @"Packaged OMP Desktop backend runtime is missing";
    return nil;
  }

  NSTask* task = [[NSTask alloc] init];
  NSPipe* outputPipe = [NSPipe pipe];
  NSPipe* errorPipe = [NSPipe pipe];
  task.executableURL = [NSURL fileURLWithPath:_nodePath];
  task.arguments = [@[ _cliPath ] arrayByAddingObjectsFromArray:arguments];
  task.environment = [self taskEnvironment];
  task.standardOutput = outputPipe;
  task.standardError = errorPipe;

  NSError* launchError = nil;
  if (![task launchAndReturnError:&launchError]) {
    if (errorText)
      *errorText = launchError.localizedDescription;
#if !__has_feature(objc_arc)
    [task release];
#endif
    return nil;
  }
  [task waitUntilExit];
  NSData* outputData = [[outputPipe fileHandleForReading] readDataToEndOfFile];
  NSData* errorData = [[errorPipe fileHandleForReading] readDataToEndOfFile];
  NSString* output = [[NSString alloc] initWithData:outputData encoding:NSUTF8StringEncoding];
  NSString* taskError = [[NSString alloc] initWithData:errorData encoding:NSUTF8StringEncoding];
  if (task.terminationStatus != 0) {
    if (errorText) {
      NSString* detail = [taskError stringByTrimmingCharactersInSet:
                                       NSCharacterSet.whitespaceAndNewlineCharacterSet];
      *errorText = [detail length]
                       ? detail
                       : [NSString stringWithFormat:@"OMP Desktop CLI exited with status %d",
                                                    task.terminationStatus];
    }
#if !__has_feature(objc_arc)
    [output release];
    [taskError release];
    [task release];
#endif
    return nil;
  }
#if !__has_feature(objc_arc)
  [taskError release];
  [task release];
  return [output autorelease];
#else
  return output;
#endif
}

- (NSDictionary*)normalizedDaemonStatus:(NSDictionary*)raw error:(NSString*)errorText {
  NSString* local = [raw[@"localDaemon"] isKindOfClass:[NSString class]]
                        ? raw[@"localDaemon"]
                        : @"stopped";
  NSString* connected = [raw[@"connectedDaemon"] isKindOfClass:[NSString class]]
                            ? raw[@"connectedDaemon"]
                            : @"not_probed";
  NSString* status = @"stopped";
  if ([local isEqualToString:@"running"])
    status = @"running";
  else if ([local isEqualToString:@"unresponsive"])
    status = @"errored";

  return @{
    @"serverId" : [raw[@"serverId"] isKindOfClass:[NSString class]] ? raw[@"serverId"] : @"",
    @"status" : status,
    @"listen" : [raw[@"listen"] isKindOfClass:[NSString class]] ? raw[@"listen"] : [NSNull null],
    @"hostname" : [raw[@"hostname"] isKindOfClass:[NSString class]] ? raw[@"hostname"] : [NSNull null],
    @"pid" : [raw[@"pid"] isKindOfClass:[NSNumber class]] ? raw[@"pid"] : [NSNull null],
    @"home" : [raw[@"home"] isKindOfClass:[NSString class]] ? raw[@"home"] : _homePath,
    @"version" : [raw[@"daemonVersion"] isKindOfClass:[NSString class]]
                       ? raw[@"daemonVersion"]
                       : [NSNull null],
    @"desktopManaged" : [raw[@"desktopManaged"] isKindOfClass:[NSNumber class]]
                              ? raw[@"desktopManaged"]
                              : @NO,
    @"error" : [errorText length] ? errorText : [NSNull null],
  };
}

- (NSDictionary*)daemonStatus {
  NSString* errorText = nil;
  NSString* output = [self runCli:@[ @"daemon", @"status", @"--home", _homePath, @"--json" ]
                             error:&errorText];
  if (!output)
    return [self normalizedDaemonStatus:@{} error:errorText];
  NSError* jsonError = nil;
  NSDictionary* raw = JsonObject(output, &jsonError);
  return [self normalizedDaemonStatus:raw ?: @{}
                                error:jsonError ? jsonError.localizedDescription : nil];
}

- (BOOL)configureDaemonForApplicationOrigin:(NSString**)errorText
                                     changed:(BOOL*)changed {
  NSString* path = [_homePath stringByAppendingPathComponent:@"config.json"];
  NSData* existing = [NSData dataWithContentsOfFile:path];
  NSError* error = nil;
  NSDictionary* parsed =
      existing ? [NSJSONSerialization JSONObjectWithData:existing
                                                 options:0
                                                   error:&error]
               : @{};
  if (existing && ![parsed isKindOfClass:[NSDictionary class]]) {
    if (errorText)
      *errorText = error.localizedDescription ?: @"Invalid daemon configuration";
    return NO;
  }
  NSMutableDictionary* config = [parsed mutableCopy];
  NSDictionary* daemonSource =
      [config[@"daemon"] isKindOfClass:[NSDictionary class]]
          ? config[@"daemon"]
          : @{};
  NSMutableDictionary* daemon = [daemonSource mutableCopy];
  NSDictionary* corsSource =
      [daemon[@"cors"] isKindOfClass:[NSDictionary class]] ? daemon[@"cors"] : @{};
  NSMutableDictionary* cors = [corsSource mutableCopy];
  cors[@"allowedOrigins"] = @[ @"omp-desktop://app", [self applicationURL] ];
  daemon[@"cors"] = cors;
  daemon[@"listen"] = _listenAddress;
  config[@"daemon"] = daemon;
  NSDictionary* appSource =
      [config[@"app"] isKindOfClass:[NSDictionary class]] ? config[@"app"] : @{};
  NSMutableDictionary* app = [appSource mutableCopy];
  app[@"baseUrl"] = [self applicationURL];
  config[@"app"] = app;
  if (changed)
    *changed = ![config isEqualToDictionary:parsed];
  NSData* encoded =
      [NSJSONSerialization dataWithJSONObject:config
                                      options:NSJSONWritingPrettyPrinted
                                        error:&error];
  BOOL success = encoded && [encoded writeToFile:path options:NSDataWritingAtomic
                                           error:&error];
#if !__has_feature(objc_arc)
  [config release];
  [daemon release];
  [cors release];
  [app release];
#endif
  if (!success && errorText)
    *errorText = error.localizedDescription ?: @"Could not write daemon configuration";
  return success;
}

- (NSDictionary*)startDaemon:(NSString**)errorText {
  NSDictionary* status = [self daemonStatus];
  BOOL configurationChanged = NO;
  if (![self configureDaemonForApplicationOrigin:errorText
                                         changed:&configurationChanged])
    return nil;
  if ([status[@"status"] isEqualToString:@"running"] &&
      !configurationChanged)
    return status;
  if ([status[@"status"] isEqualToString:@"running"] &&
      ![self runCli:@[ @"daemon", @"stop", @"--home", _homePath, @"--force" ]
                error:errorText])
    return nil;
  NSArray* arguments = @[
    @"daemon", @"start", @"--home", _homePath, @"--listen", _listenAddress,
    @"--no-web-ui", @"--no-mcp", @"--no-inject-mcp",
  ];
  if (![self runCli:arguments error:errorText])
    return nil;
  status = [self daemonStatus];
  if (![status[@"status"] isEqualToString:@"running"]) {
    if (errorText)
      *errorText = status[@"error"] == [NSNull null]
                       ? @"Daemon did not become ready"
                       : status[@"error"];
    return nil;
  }
  std::printf("OMP_DESKTOP_DAEMON_READY listen=%s pid=%s\n",
              [_listenAddress UTF8String],
              status[@"pid"] == [NSNull null]
                  ? "unknown"
                  : [[status[@"pid"] stringValue] UTF8String]);
  std::fflush(stdout);
  return status;
}

- (NSDictionary*)stopDaemon:(NSString**)errorText {
  if (![self runCli:@[ @"daemon", @"stop", @"--home", _homePath, @"--force" ]
                error:errorText])
    return nil;
  return [self daemonStatus];
}

- (NSDictionary*)settings {
  NSDictionary* stored = [[NSUserDefaults standardUserDefaults]
      dictionaryForKey:@"OMPDesktopSettings"];
  return stored ? MergeDictionary(DefaultSettings(), stored) : DefaultSettings();
}

- (NSDictionary*)patchSettings:(NSDictionary*)patch {
  NSDictionary* next = MergeDictionary([self settings], patch ?: @{});
  [[NSUserDefaults standardUserDefaults] setObject:next forKey:@"OMPDesktopSettings"];
  return next;
}

- (NSDictionary*)writeAttachmentData:(NSData*)data arguments:(NSDictionary*)arguments {
  NSString* identifier = [arguments[@"attachmentId"] isKindOfClass:[NSString class]]
                             ? arguments[@"attachmentId"]
                             : NSUUID.UUID.UUIDString;
  NSString* extension = [arguments[@"extension"] isKindOfClass:[NSString class]]
                            ? arguments[@"extension"]
                            : @"";
  NSString* directory = [_homePath stringByAppendingPathComponent:@"attachments"];
  [[NSFileManager defaultManager] createDirectoryAtPath:directory
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];
  NSString* name = [extension length]
                       ? [identifier stringByAppendingPathExtension:extension]
                       : identifier;
  NSString* path = [directory stringByAppendingPathComponent:name];
  if (![data writeToFile:path atomically:YES])
    return nil;
  return @{ @"path" : path, @"byteSize" : @(data.length) };
}

- (id)handleInvoke:(NSString*)command arguments:(NSDictionary*)arguments error:(NSString**)errorText {
  if ([command isEqualToString:@"get_desktop_settings"])
    return [self settings];
  if ([command isEqualToString:@"patch_desktop_settings"])
    return [self patchSettings:arguments];
  if ([command isEqualToString:@"migrate_legacy_desktop_settings"])
    return [self settings];
  if ([command isEqualToString:@"desktop_daemon_status"])
    return [self daemonStatus];
  if ([command isEqualToString:@"start_desktop_daemon"])
    return [self startDaemon:errorText];
  if ([command isEqualToString:@"stop_desktop_daemon"])
    return [self stopDaemon:errorText];
  if ([command isEqualToString:@"restart_desktop_daemon"]) {
    if (![self stopDaemon:errorText])
      return nil;
    return [self startDaemon:errorText];
  }
  if ([command isEqualToString:@"desktop_daemon_logs"] ||
      [command isEqualToString:@"desktop_app_logs"]) {
    NSString* path = [_homePath stringByAppendingPathComponent:@"daemon.log"];
    return @{ @"logPath" : path, @"contents" : ReadTextFile(path) };
  }
  if ([command isEqualToString:@"cli_daemon_status"])
    return [self runCli:@[ @"daemon", @"status", @"--home", _homePath ] error:errorText];
  if ([command isEqualToString:@"desktop_get_runtime_info"])
    return @{ @"appVersion" : @"0.3.6", @"runningUnderARM64Translation" : @NO };
  if ([command isEqualToString:@"desktop_get_system_idle_time"])
    return @0;
  if ([command isEqualToString:@"get_local_daemon_version"]) {
    NSDictionary* status = [self daemonStatus];
    return @{ @"version" : status[@"version"], @"error" : status[@"error"] };
  }
  if ([command isEqualToString:@"write_attachment_base64"]) {
    NSString* encoded = [arguments[@"base64"] isKindOfClass:[NSString class]]
                            ? arguments[@"base64"]
                            : @"";
    NSData* data = [[NSData alloc] initWithBase64EncodedString:encoded options:0];
    id result = data ? [self writeAttachmentData:data arguments:arguments] : nil;
#if !__has_feature(objc_arc)
    [data release];
#endif
    if (!result && errorText)
      *errorText = @"Could not write attachment";
    return result;
  }
  if ([command isEqualToString:@"write_attachment_bytes"]) {
    id rawBytes = arguments[@"bytes"];
    NSMutableData* data = [NSMutableData data];
    if ([rawBytes isKindOfClass:[NSArray class]]) {
      for (NSNumber* byte in rawBytes) {
        unsigned char value = byte.unsignedCharValue;
        [data appendBytes:&value length:1];
      }
    } else if ([rawBytes isKindOfClass:[NSDictionary class]]) {
      NSArray* keys = [[rawBytes allKeys]
          sortedArrayUsingComparator:^NSComparisonResult(NSString* left, NSString* right) {
            return [left integerValue] < [right integerValue] ? NSOrderedAscending : NSOrderedDescending;
          }];
      for (NSString* key in keys) {
        unsigned char value = [rawBytes[key] unsignedCharValue];
        [data appendBytes:&value length:1];
      }
    }
    id result = [self writeAttachmentData:data arguments:arguments];
    if (!result && errorText)
      *errorText = @"Could not write attachment";
    return result;
  }
  if ([command isEqualToString:@"copy_attachment_file"]) {
    NSString* source = [arguments[@"sourcePath"] isKindOfClass:[NSString class]]
                           ? arguments[@"sourcePath"]
                           : @"";
    NSData* data = [NSData dataWithContentsOfFile:source];
    id result = data ? [self writeAttachmentData:data arguments:arguments] : nil;
    if (!result && errorText)
      *errorText = @"Could not copy attachment";
    return result;
  }
  if ([command isEqualToString:@"read_file_base64"]) {
    NSString* path = [arguments[@"path"] isKindOfClass:[NSString class]] ? arguments[@"path"] : @"";
    NSData* data = [NSData dataWithContentsOfFile:path];
    if (!data && errorText)
      *errorText = @"Could not read file";
    return data ? [data base64EncodedStringWithOptions:0] : nil;
  }
  if ([command isEqualToString:@"delete_attachment_file"]) {
    NSString* path = [arguments[@"path"] isKindOfClass:[NSString class]] ? arguments[@"path"] : @"";
    return @([[NSFileManager defaultManager] removeItemAtPath:path error:nil]);
  }
  if ([command isEqualToString:@"garbage_collect_attachment_files"])
    return @0;
  if ([command isEqualToString:@"get_launch_at_login"] ||
      [command isEqualToString:@"set_launch_at_login"])
    return @{ @"enabled" : @NO };
  if ([command isEqualToString:@"get_cli_install_status"] ||
      [command isEqualToString:@"get_omp_shortcut_install_status"] ||
      [command isEqualToString:@"install_cli"] ||
      [command isEqualToString:@"install_omp_shortcut"] ||
      [command isEqualToString:@"uninstall_omp_shortcut"])
    return @{ @"installed" : @NO };
  if ([command isEqualToString:@"read_legacy_skill_selection"])
    return [NSNull null];
  if ([command isEqualToString:@"delete_legacy_skill_selection"])
    return [NSNull null];

  if (errorText)
    *errorText = [NSString stringWithFormat:@"Unsupported OMP Desktop command: %@", command];
  return nil;
}

- (id)handleUiScope:(NSString*)scope
             method:(NSString*)method
          arguments:(NSDictionary*)arguments
              error:(NSString**)errorText {
  if ([scope isEqualToString:@"window"]) {
    if ([method isEqualToString:@"toggleMaximize"]) {
      [_window zoom:nil];
      return [NSNull null];
    }
    if ([method isEqualToString:@"setFullscreen"]) {
      BOOL requested = [arguments[@"fullscreen"] boolValue];
      BOOL current = (_window.styleMask & NSWindowStyleMaskFullScreen) != 0;
      if (requested != current)
        [_window toggleFullScreen:nil];
      return [NSNull null];
    }
    if ([method isEqualToString:@"isFullscreen"])
      return @((_window.styleMask & NSWindowStyleMaskFullScreen) != 0);
    if ([method isEqualToString:@"isMaximized"])
      return @(_window.isZoomed);
    if ([method isEqualToString:@"setBadgeCount"]) {
      NSNumber* count = [arguments[@"count"] isKindOfClass:[NSNumber class]]
                            ? arguments[@"count"]
                            : nil;
      NSApp.dockTile.badgeLabel = count && count.integerValue > 0 ? count.stringValue : nil;
      return [NSNull null];
    }
    if ([method isEqualToString:@"openNew"])
      return [NSNull null];
  }

  if ([scope isEqualToString:@"dialog"]) {
    if ([method isEqualToString:@"open"]) {
      NSOpenPanel* panel = [NSOpenPanel openPanel];
      NSDictionary* options = arguments ?: @{};
      panel.canChooseDirectories = [options[@"directory"] boolValue];
      panel.canChooseFiles = !panel.canChooseDirectories;
      panel.allowsMultipleSelection = [options[@"multiple"] boolValue];
      panel.canCreateDirectories = [options[@"createDirectory"] boolValue];
      if ([options[@"title"] isKindOfClass:[NSString class]])
        panel.title = options[@"title"];
      if ([panel runModal] != NSModalResponseOK)
        return [NSNull null];
      NSArray* paths = [panel.URLs valueForKey:@"path"];
      return panel.allowsMultipleSelection ? paths : (paths.firstObject ?: [NSNull null]);
    }
    if ([method isEqualToString:@"ask"] ||
        [method isEqualToString:@"askWithCheckbox"]) {
      NSAlert* alert = [[NSAlert alloc] init];
      NSString* message = [arguments[@"message"] isKindOfClass:[NSString class]]
                              ? arguments[@"message"]
                              : @"";
      NSDictionary* options = [arguments[@"options"] isKindOfClass:[NSDictionary class]]
                                  ? arguments[@"options"]
                                  : @{};
      alert.messageText = [options[@"title"] isKindOfClass:[NSString class]]
                              ? options[@"title"]
                              : @"OMP Desktop";
      alert.informativeText = message;
      [alert addButtonWithTitle:[options[@"okLabel"] isKindOfClass:[NSString class]]
                                    ? options[@"okLabel"]
                                    : @"OK"];
      [alert addButtonWithTitle:[options[@"cancelLabel"] isKindOfClass:[NSString class]]
                                    ? options[@"cancelLabel"]
                                    : @"Cancel"];
      BOOL withCheckbox = [method isEqualToString:@"askWithCheckbox"];
      NSButton* checkbox = nil;
      if (withCheckbox) {
        checkbox = [NSButton checkboxWithTitle:options[@"checkboxLabel"] ?: @""
                                       target:nil
                                       action:nil];
        checkbox.state = [options[@"checkboxChecked"] boolValue]
                             ? NSControlStateValueOn
                             : NSControlStateValueOff;
        alert.accessoryView = checkbox;
      }
      BOOL confirmed = [alert runModal] == NSAlertFirstButtonReturn;
      id result = withCheckbox
                      ? @{ @"confirmed" : @(confirmed),
                           @"dontAskAgain" : @(checkbox.state == NSControlStateValueOn) }
                      : @(confirmed);
#if !__has_feature(objc_arc)
      [alert release];
#endif
      return result;
    }
  }

  if ([scope isEqualToString:@"opener"] && [method isEqualToString:@"openUrl"]) {
    NSString* value = [arguments[@"url"] isKindOfClass:[NSString class]] ? arguments[@"url"] : @"";
    NSURL* url = [NSURL URLWithString:value];
    if (!url || ![[NSWorkspace sharedWorkspace] openURL:url]) {
      if (errorText)
        *errorText = @"Could not open URL";
      return nil;
    }
    return [NSNull null];
  }

  if ([scope isEqualToString:@"editor"] && [method isEqualToString:@"openTarget"]) {
    NSString* path = [arguments[@"filePath"] isKindOfClass:[NSString class]]
                         ? arguments[@"filePath"]
                         : arguments[@"workspacePath"];
    if ([path isKindOfClass:[NSString class]] && [path length]) {
      [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path]];
      return [NSNull null];
    }
  }

  if (errorText)
    *errorText = [NSString stringWithFormat:@"Unsupported native bridge method: %@.%@", scope, method];
  return nil;
}

- (void)handleRequest:(NSString*)request completion:(void (^)(id, NSString*))completion {
  NSError* jsonError = nil;
  NSDictionary* payload = JsonObject(request, &jsonError);
  if (!payload) {
    completion(nil, jsonError.localizedDescription ?: @"Invalid native request");
    return;
  }
  NSString* scope = [payload[@"scope"] isKindOfClass:[NSString class]] ? payload[@"scope"] : @"";
  NSString* method = [payload[@"method"] isKindOfClass:[NSString class]] ? payload[@"method"] : @"";
  NSDictionary* arguments = [payload[@"args"] isKindOfClass:[NSDictionary class]]
                                ? payload[@"args"]
                                : @{};

  if (![scope isEqualToString:@"invoke"]) {
    NSString* errorText = nil;
    id result = [self handleUiScope:scope method:method arguments:arguments error:&errorText];
    completion(result, errorText);
    return;
  }

  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    NSString* errorText = nil;
    id result = [self handleInvoke:method arguments:arguments error:&errorText];
    completion(result, errorText);
  });
}

- (BOOL)serveURL:(NSString*)value job:(mbNetJob)job {
  NSURLComponents* components = [NSURLComponents componentsWithString:value];
  if (![components.scheme isEqualToString:@"http"] ||
      ![components.host isEqualToString:@"127.0.0.1"] ||
      components.port.integerValue != 17913) {
    return NO;
  }

  NSString* relative = components.percentEncodedPath.stringByRemovingPercentEncoding ?: @"/";
  while ([relative hasPrefix:@"/"])
    relative = [relative substringFromIndex:1];
  NSString* candidate = [[_appDistPath stringByAppendingPathComponent:relative]
      stringByStandardizingPath];
  NSString* allowedPrefix = [_appDistPath stringByAppendingString:@"/"];
  BOOL safe = [candidate isEqualToString:_appDistPath] || [candidate hasPrefix:allowedPrefix];
  BOOL isDirectory = NO;
  BOOL exists = safe && [[NSFileManager defaultManager] fileExistsAtPath:candidate
                                                             isDirectory:&isDirectory];
  if (!exists || isDirectory)
    candidate = [_appDistPath stringByAppendingPathComponent:@"index.html"];

  NSData* data = [NSData dataWithContentsOfFile:candidate];
  if (!data) {
    const char message[] = "Not found";
    mbNetSetMIMEType(job, "text/plain");
    mbNetSetData(job, const_cast<char*>(message), sizeof(message) - 1);
    return YES;
  }
  mbNetSetMIMEType(job, MimeType(candidate).UTF8String);
  mbNetSetHTTPHeaderFieldUtf8(job, "Access-Control-Allow-Origin", "*", TRUE);
  mbNetSetData(job, const_cast<void*>(data.bytes), static_cast<int>(data.length));
  return YES;
}

- (void)configureWebView:(mbWebView)webView window:(NSWindow*)window {
  _window = window;
  NSString* storageDirectory =
      [_homePath stringByAppendingPathComponent:@"web-runtime"];
  NSString* localStorage =
      [storageDirectory stringByAppendingPathComponent:@"local-storage"];
  [[NSFileManager defaultManager] createDirectoryAtPath:localStorage
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];
  NSString* cookies = [storageDirectory stringByAppendingPathComponent:@"cookies.dat"];
  mbMacSetStoragePaths(webView, cookies.UTF8String, localStorage.UTF8String);
  mbOnLoadUrlBegin(
      webView,
      [](mbWebView view, void* parameter, const char* url, void* job) -> BOOL {
        OmpDesktopRuntime* runtime = (__bridge OmpDesktopRuntime*)parameter;
        NSString* value = url ? [NSString stringWithUTF8String:url] : @"";
        return [runtime serveURL:value job:job];
      },
      (__bridge void*)self);
  mbOnDidCreateScriptContext(
      webView,
      [](mbWebView view, void* parameter, mbWebFrameHandle frame, void* context,
         int extensionGroup, int worldId) {
        if (worldId != 0 || frame != mbWebFrameGetMainFrame(view))
          return;
        OmpDesktopRuntime* runtime = (__bridge OmpDesktopRuntime*)parameter;
        NSString* shell = NSProcessInfo.processInfo.environment[@"SHELL"] ?: @"/bin/zsh";
        NSString* script = BridgeScript(shell);
        mbRunJs(view, frame, script.UTF8String, FALSE, nullptr, nullptr, nullptr);
      },
      (__bridge void*)self);
  mbOnJsQuery(
      webView,
      [](mbWebView view, void* parameter, mbJsExecState state, int64_t queryId,
         int customMessage, const utf8* request) {
        OmpDesktopRuntime* runtime = (__bridge OmpDesktopRuntime*)parameter;
        if (customMessage != kBridgeMessage) {
          mbResponseQuery(view, queryId, customMessage,
                          "{\"ok\":false,\"error\":\"Unknown native message\"}");
          return;
        }
        NSString* value = request ? [NSString stringWithUTF8String:request] : @"{}";
        [runtime handleRequest:value completion:^(id result, NSString* errorText) {
          NSDictionary* envelope = errorText
                                       ? @{ @"ok" : @NO, @"error" : errorText }
                                       : @{ @"ok" : @YES,
                                            @"value" : result ?: [NSNull null] };
          NSString* response = JsonString(envelope);
          mbResponseQuery(view, queryId, customMessage, response.UTF8String);
#if !__has_feature(objc_arc)
          [response release];
#endif
        }];
      },
      (__bridge void*)self);
}

@end
