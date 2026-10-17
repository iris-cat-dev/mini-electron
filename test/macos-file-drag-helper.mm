#import <AppKit/AppKit.h>

#include <node_api.h>

#include <string>
#include <iterator>
#include <vector>

@interface MiniDragInfo : NSObject <NSDraggingInfo>
- (instancetype)initWithPasteboard:(NSPasteboard*)pasteboard
                             window:(NSWindow*)window
                           location:(NSPoint)location;
@property BOOL animatesToDestination;
@property NSInteger numberOfValidItemsForDrop;
@property NSDraggingFormation draggingFormation;
@property(readonly) NSSpringLoadingHighlight springLoadingHighlight;
@end

@implementation MiniDragInfo {
  NSPasteboard* _pasteboard;
  NSWindow* _window;
  NSPoint _location;
}
@synthesize animatesToDestination;
@synthesize numberOfValidItemsForDrop;
@synthesize draggingFormation;
@synthesize springLoadingHighlight;

- (instancetype)initWithPasteboard:(NSPasteboard*)pasteboard
                             window:(NSWindow*)window
                           location:(NSPoint)location {
  if ((self = [super init])) {
    _pasteboard = pasteboard;
    _window = window;
    _location = location;
  }
  return self;
}
- (NSPoint)draggingLocation { return _location; }
- (NSPasteboard*)draggingPasteboard { return _pasteboard; }
- (NSInteger)draggingSequenceNumber { return 1; }
- (id)draggingSource { return nil; }
- (NSDragOperation)draggingSourceOperationMask { return NSDragOperationCopy; }
- (NSWindow*)draggingDestinationWindow { return _window; }
- (NSArray*)namesOfPromisedFilesDroppedAtDestination:(NSURL*)destination {
  (void)destination;
  return nil;
}
- (NSImage*)draggedImage { return nil; }
- (NSPoint)draggedImageLocation { return NSZeroPoint; }
- (void)slideDraggedImageTo:(NSPoint)point { (void)point; }
- (void)enumerateDraggingItemsWithOptions:(NSDraggingItemEnumerationOptions)options
                                  forView:(NSView*)view
                                  classes:(NSArray*)classes
                            searchOptions:(NSDictionary*)searchOptions
                               usingBlock:(void (^)(NSDraggingItem*, NSInteger, BOOL*))block {
  (void)options;
  (void)view;
  (void)classes;
  (void)searchOptions;
  (void)block;
}
- (void)resetSpringLoading {}
@end

namespace {

NSView* g_drag_view = nil;
MiniDragInfo* g_drag_info = nil;

napi_value Fail(napi_env env, const char* message) {
  napi_throw_error(env, nullptr, message);
  return nullptr;
}

bool StringArgument(napi_env env, napi_value value, std::string* output) {
  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok)
    return false;
  output->resize(length);
  return napi_get_value_string_utf8(env, value, output->data(), length + 1,
                                    &length) == napi_ok;
}

NSWindow* WindowWithTitle(const std::string& title) {
  NSString* expected = [NSString stringWithUTF8String:title.c_str()];
  for (NSWindow* window in NSApp.windows) {
    if ([window.title isEqualToString:expected])
      return window;
  }
  return nil;
}

NSView* DragView(NSWindow* window) {
  for (NSView* view in window.contentView.subviews) {
    if ([view respondsToSelector:@selector(draggingEntered:)] &&
        [view respondsToSelector:@selector(performDragOperation:)]) {
      return view;
    }
  }
  return nil;
}

NSPasteboard* FilePasteboard(const std::vector<std::string>& paths) {
  NSPasteboard* pasteboard = [NSPasteboard pasteboardWithUniqueName];
  [pasteboard clearContents];
  NSMutableArray<NSURL*>* urls = [NSMutableArray arrayWithCapacity:paths.size()];
  for (const std::string& path : paths) {
    NSString* value = [[NSString alloc] initWithBytes:path.data()
                                               length:path.size()
                                             encoding:NSUTF8StringEncoding];
    if (!value)
      return nil;
    [urls addObject:[NSURL fileURLWithPath:value]];
  }
  return [pasteboard writeObjects:urls] ? pasteboard : nil;
}

bool ReadPaths(napi_env env, napi_value value,
               std::vector<std::string>* paths) {
  bool array = false;
  uint32_t length = 0;
  if (napi_is_array(env, value, &array) != napi_ok || !array ||
      napi_get_array_length(env, value, &length) != napi_ok || !length)
    return false;
  paths->reserve(length);
  for (uint32_t index = 0; index < length; ++index) {
    napi_value item;
    std::string path;
    if (napi_get_element(env, value, index, &item) != napi_ok ||
        !StringArgument(env, item, &path) || path.empty())
      return false;
    paths->push_back(std::move(path));
  }
  return true;
}

napi_value Begin(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value args[4];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  std::string title;
  std::vector<std::string> paths;
  double x = 0, y = 0;
  if (argc != 4 || !StringArgument(env, args[0], &title) ||
      !ReadPaths(env, args[1], &paths) ||
      napi_get_value_double(env, args[2], &x) != napi_ok ||
      napi_get_value_double(env, args[3], &y) != napi_ok)
    return Fail(env, "begin requires a window title, file paths and view coordinates");
  NSWindow* window = WindowWithTitle(title);
  NSView* view = window ? DragView(window) : nil;
  NSPasteboard* pasteboard = FilePasteboard(paths);
  if (!view || !pasteboard)
    return Fail(env, "unable to prepare native file drag");
  g_drag_view = view;
  g_drag_info = [[MiniDragInfo alloc] initWithPasteboard:pasteboard
                                                  window:window
                                                location:[view convertPoint:NSMakePoint(x, y) toView:nil]];
  NSDragOperation operation = [(id)g_drag_view draggingEntered:g_drag_info];
  napi_value result;
  napi_create_uint32(env, static_cast<uint32_t>(operation), &result);
  return result;
}

napi_value Update(napi_env env, napi_callback_info info) {
  (void)info;
  if (!g_drag_view || !g_drag_info)
    return Fail(env, "no active native drag");
  NSDragOperation operation = [(id)g_drag_view draggingUpdated:g_drag_info];
  napi_value result;
  napi_create_uint32(env, static_cast<uint32_t>(operation), &result);
  return result;
}

napi_value Drop(napi_env env, napi_callback_info info) {
  (void)info;
  if (!g_drag_view || !g_drag_info)
    return Fail(env, "no active native drag");
  const BOOL prepared = [(id)g_drag_view prepareForDragOperation:g_drag_info];
  const BOOL dropped = prepared && [(id)g_drag_view performDragOperation:g_drag_info];
  g_drag_view = nil;
  g_drag_info = nil;
  napi_value result;
  napi_get_boolean(env, dropped, &result);
  return result;
}

napi_value Leave(napi_env env, napi_callback_info info) {
  (void)info;
  if (!g_drag_view || !g_drag_info)
    return Fail(env, "no active native drag");
  [(id)g_drag_view draggingExited:g_drag_info];
  g_drag_view = nil;
  g_drag_info = nil;
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}

napi_value NonFile(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg;
  napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr);
  std::string title;
  if (argc != 1 || !StringArgument(env, arg, &title))
    return Fail(env, "nonFile requires a window title");
  NSWindow* window = WindowWithTitle(title);
  NSView* view = window ? DragView(window) : nil;
  if (!view)
    return Fail(env, "renderer drag destination was not found");
  NSPasteboard* pasteboard = [NSPasteboard pasteboardWithUniqueName];
  [pasteboard clearContents];
  [pasteboard setString:@"not a file" forType:NSPasteboardTypeString];
  MiniDragInfo* drag = [[MiniDragInfo alloc] initWithPasteboard:pasteboard
                                                        window:window
                                                      location:NSMakePoint(80, 80)];
  NSDragOperation operation = [(id)view draggingEntered:drag];
  napi_value result;
  napi_create_uint32(env, static_cast<uint32_t>(operation), &result);
  return result;
}

napi_value Init(napi_env env, napi_value exports) {
  napi_property_descriptor methods[] = {
      {"begin", nullptr, Begin, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"update", nullptr, Update, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"drop", nullptr, Drop, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"leave", nullptr, Leave, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"nonFile", nullptr, NonFile, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_define_properties(env, exports, std::size(methods), methods);
  return exports;
}

}  // namespace

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
