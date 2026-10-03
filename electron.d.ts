/// <reference types="node" />

import type { EventEmitter } from "node:events";
import type { IncomingHttpHeaders } from "node:http";
import type { Readable, Writable } from "node:stream";

export as namespace Electron;

export type Accelerator = string;
export interface Event {
  preventDefault(): void;
  readonly defaultPrevented: boolean;
}
export interface Rectangle { x: number; y: number; width: number; height: number }
export interface Point { x: number; y: number }
export interface Size { width: number; height: number }
export interface Display { id: number; bounds: Rectangle; workArea: Rectangle; scaleFactor: number }
export interface AuthInfo { isProxy: boolean; scheme: string; host: string; port: number; realm: string }
export interface KeyboardInputEvent {
  type: "keyDown" | "keyUp" | "char";
  keyCode: string;
  modifiers?: Array<"alt" | "altGr" | "capsLock" | "control" | "meta" | "numLock" | "scrollLock" | "shift" | "super">;
}
export interface Input {
  alt: boolean;
  code: string;
  control: boolean;
  isAutoRepeat: boolean;
  isComposing: boolean;
  key: string;
  location: number;
  meta: boolean;
  modifiers: string[];
  shift: boolean;
  type: "keyDown" | "keyUp" | "char";
}
export interface TitleBarOverlayOptions { color?: string; symbolColor?: string; height?: number }
export interface WebPreferences {
  preload?: string;
  partition?: string;
  contextIsolation?: boolean;
  sandbox?: boolean;
  webSecurity?: boolean;
  webviewTag?: boolean;
  nodeIntegration?: boolean;
  nodeIntegrationInSubFrames?: boolean;
  nodeIntegrationInWorker?: boolean;
  allowRunningInsecureContent?: boolean;
}
export interface BrowserWindowConstructorOptions {
  width?: number;
  height?: number;
  x?: number;
  y?: number;
  title?: string;
  show?: boolean;
  parent?: BrowserWindow;
  icon?: NativeImage | string;
  frame?: boolean;
  backgroundColor?: string;
  autoHideMenuBar?: boolean;
  titleBarStyle?: "default" | "hidden" | "hiddenInset" | "customButtonsOnHover";
  titleBarOverlay?: boolean | TitleBarOverlayOptions;
  trafficLightPosition?: Point;
  webPreferences?: WebPreferences;
}
export interface ContextMenuParams {
  x: number;
  y: number;
  linkURL: string;
  linkText: string;
  pageURL: string;
  frameURL: string;
  srcURL: string;
  mediaType: "none" | "image" | "audio" | "video" | "canvas" | "file" | "plugin";
  hasImageContents: boolean;
  isEditable: boolean;
  selectionText: string;
  titleText: string;
  altText: string;
  suggestedFilename: string;
  misspelledWord: string;
  dictionarySuggestions: string[];
  frameCharset: string;
  inputFieldType: string;
  spellcheckEnabled: boolean;
  menuSourceType: string;
  mediaFlags: Record<string, boolean>;
  editFlags: {
    canUndo: boolean; canRedo: boolean; canCut: boolean; canCopy: boolean; canPaste: boolean;
    canDelete: boolean; canSelectAll: boolean; canEditRichly: boolean;
  };
}
export interface OpenDialogOptions {
  title?: string;
  defaultPath?: string;
  buttonLabel?: string;
  filters?: Array<{ name: string; extensions: string[] }>;
  properties?: Array<"openFile" | "openDirectory" | "multiSelections" | "showHiddenFiles" | "createDirectory" | "promptToCreate" | "noResolveAliases" | "treatPackageAsDirectory" | "dontAddToRecent">;
  message?: string;
  securityScopedBookmarks?: boolean;
}
export interface MessageBoxOptions {
  type?: "none" | "info" | "error" | "question" | "warning";
  buttons?: string[];
  defaultId?: number;
  title?: string;
  message: string;
  detail?: string;
  checkboxLabel?: string;
  checkboxChecked?: boolean;
  cancelId?: number;
  noLink?: boolean;
}
export interface MenuItemConstructorOptions {
  id?: string;
  label?: string;
  sublabel?: string;
  role?: string;
  type?: "normal" | "separator" | "submenu" | "checkbox" | "radio";
  accelerator?: Accelerator;
  enabled?: boolean;
  visible?: boolean;
  checked?: boolean;
  submenu?: MenuItemConstructorOptions[] | Menu;
  click?: (menuItem: MenuItem, browserWindow: BaseWindow | undefined, event: KeyboardEvent) => void;
}
export interface KeyboardEvent { triggeredByAccelerator: boolean }
export interface IpcMainEvent extends Event { sender: WebContents; returnValue: unknown; reply(channel: string, ...args: unknown[]): void }
export interface IpcMainInvokeEvent extends Event { sender: WebContents; frameId: number; processId: number }
export interface IpcRendererEvent extends Event { sender: IpcRenderer; senderId: number }

export class NativeImage {
  isEmpty(): boolean;
  getSize(): Size;
  toPNG(): Buffer;
  toJPEG(quality: number): Buffer;
  toDataURL(): string;
  resize(options: { width?: number; height?: number; quality?: "good" | "better" | "best" }): NativeImage;
  setTemplateImage(option: boolean): void;
}
export const nativeImage: {
  createEmpty(): NativeImage;
  createFromPath(path: string): NativeImage;
  createFromBuffer(buffer: Buffer, options?: { width?: number; height?: number; scaleFactor?: number }): NativeImage;
  createFromDataURL(dataURL: string): NativeImage;
};

export interface Debugger extends EventEmitter {
  attach(protocolVersion?: string): void;
  detach(): void;
  isAttached(): boolean;
  sendCommand(method: string, commandParams?: Record<string, unknown>, sessionId?: string): Promise<unknown>;
}
export interface Session extends EventEmitter {
  readonly webRequest: WebRequest;
  readonly availableSpellCheckerLanguages: string[];
  getStoragePath(): string | null;
  clearStorageData(options?: { origin?: string; storages?: string[]; quotas?: string[] }): Promise<void>;
  clearCache(): Promise<void>;
  clearAuthCache(): Promise<void>;
  addWordToSpellCheckerDictionary(word: string): boolean;
  setProxy(config: { mode?: string; proxyRules?: string; proxyBypassRules?: string; pacScript?: string }): Promise<void>;
}
export interface WebRequest {
  onBeforeRequest(listener: ((details: { id: number; url: string; method: string }, callback: (response: { cancel?: boolean; redirectURL?: string }) => void) => void) | null): void;
}
export interface WindowOpenHandlerDetails { url: string; frameName: string; features: string; disposition: string; referrer: { url: string; policy: string }; postBody?: { data: Array<{ bytes?: Buffer; file?: string }> } }
export type WindowOpenHandlerResponse = { action: "deny" } | { action: "allow"; overrideBrowserWindowOptions?: BrowserWindowConstructorOptions };
export interface WebContents extends EventEmitter {
  readonly id: number;
  readonly session: Session;
  readonly debugger: Debugger;
  send(channel: string, ...args: unknown[]): void;
  loadURL(url: string, options?: { httpReferrer?: string; userAgent?: string; extraHeaders?: string; postData?: unknown[] }): Promise<void>;
  loadFile(filePath: string, options?: { query?: Record<string, string>; search?: string; hash?: string }): Promise<void>;
  getURL(): string;
  getTitle(): string;
  isDestroyed(): boolean;
  isLoading(): boolean;
  getType(): "backgroundPage" | "window" | "browserView" | "remote" | "webview" | "offscreen";
  getZoomLevel(): number;
  setZoomLevel(level: number): void;
  canGoBack(): boolean;
  canGoForward(): boolean;
  goBack(): void;
  goForward(): void;
  reload(): void;
  reloadIgnoringCache(): void;
  focus(): void;
  executeJavaScript(code: string, userGesture?: boolean): Promise<unknown>;
  capturePage(rect?: Rectangle, options?: { stayHidden?: boolean; stayAwake?: boolean }): Promise<NativeImage>;
  invalidate(): void;
  sendInputEvent(inputEvent: KeyboardInputEvent): void;
  setWindowOpenHandler(handler: (details: WindowOpenHandlerDetails) => WindowOpenHandlerResponse): void;
  openDevTools(options?: { mode?: "right" | "bottom" | "undocked" | "detach"; activate?: boolean; title?: string }): void;
  closeDevTools(): void;
  isDevToolsOpened(): boolean;
  inspectElement(x: number, y: number): void;
  replaceMisspelling(suggestion: string): void;
  copyImageAt(x: number, y: number): void;
  downloadURL(url: string): void;
}
export const webContents: {
  fromId(id: number): WebContents | undefined;
  getAllWebContents(): WebContents[];
};

export class BaseWindow extends EventEmitter {
  id: number;
  isDestroyed(): boolean;
  close(): void;
  focus(): void;
  show(): void;
  hide(): void;
}
export class BrowserWindow extends BaseWindow {
  constructor(options?: BrowserWindowConstructorOptions);
  static getAllWindows(): BrowserWindow[];
  static getFocusedWindow(): BrowserWindow | null;
  static fromWebContents(webContents: WebContents): BrowserWindow | null;
  readonly webContents: WebContents;
  loadURL(url: string, options?: { extraHeaders?: string }): Promise<void>;
  loadFile(filePath: string): Promise<void>;
  isVisible(): boolean;
  isMinimized(): boolean;
  isMaximized(): boolean;
  isFullScreen(): boolean;
  maximize(): void;
  unmaximize(): void;
  minimize(): void;
  restore(): void;
  setFullScreen(flag: boolean): void;
  getBounds(): Rectangle;
  getNormalBounds(): Rectangle;
  setBounds(bounds: Partial<Rectangle>, animate?: boolean): void;
  setPosition(x: number, y: number, animate?: boolean): void;
  setBackgroundColor(backgroundColor: string): void;
  setTitleBarOverlay(options: TitleBarOverlayOptions): void;
  setTrafficLightPosition(position: Point): void;
  setWindowButtonPosition(position: Point | null): void;
}

export interface Dock {
  bounce(type?: "critical" | "informational"): number;
  cancelBounce(id: number): void;
  setBadge(text: string): void;
  getBadge(): string;
  setIcon(image: NativeImage | string): Promise<void>;
}
export interface CommandLine {
  appendSwitch(switchName: string, value?: string): void;
  appendArgument(value: string): void;
  hasSwitch(switchName: string): boolean;
  getSwitchValue(switchName: string): string;
}
export interface LoginItemSettings { openAtLogin: boolean; openAsHidden: boolean; wasOpenedAtLogin: boolean; wasOpenedAsHidden: boolean; restoreState: boolean; executableWillLaunchAtLogin: boolean }
export interface App extends EventEmitter {
  readonly isPackaged: boolean;
  readonly name: string;
  readonly dock?: Dock;
  readonly commandLine: CommandLine;
  whenReady(): Promise<void>;
  isReady(): boolean;
  quit(): void;
  exit(exitCode?: number): void;
  getPath(name: "home" | "appData" | "userData" | "sessionData" | "temp" | "exe" | "module" | "desktop" | "documents" | "downloads" | "music" | "pictures" | "videos" | "recent" | "logs" | "crashDumps"): string;
  setPath(name: string, path: string): void;
  getAppPath(): string;
  getVersion(): string;
  setName(name: string): void;
  setBadgeCount(count?: number): boolean;
  requestSingleInstanceLock(additionalData?: Record<string, unknown>): boolean;
  releaseSingleInstanceLock(): void;
  setAsDefaultProtocolClient(protocol: string, path?: string, args?: string[]): boolean;
  getLoginItemSettings(options?: { path?: string; args?: string[] }): LoginItemSettings;
  setLoginItemSettings(settings: { openAtLogin?: boolean; openAsHidden?: boolean; path?: string; args?: string[] }): void;
}
export const app: App;

export class MenuItem { readonly id: string; readonly label: string; enabled: boolean; visible: boolean; checked: boolean }
export class Menu {
  static setApplicationMenu(menu: Menu | null): void;
  static getApplicationMenu(): Menu | null;
  static buildFromTemplate(template: MenuItemConstructorOptions[]): Menu;
  readonly items: MenuItem[];
  popup(options?: { window?: BaseWindow; x?: number; y?: number; positioningItem?: number; callback?: () => void }): void;
  closePopup(browserWindow?: BaseWindow): void;
}
export const clipboard: {
  readText(type?: "selection" | "clipboard"): string;
  writeText(text: string, type?: "selection" | "clipboard"): void;
  writeImage(image: NativeImage, type?: "selection" | "clipboard"): void;
  readImage(type?: "selection" | "clipboard"): NativeImage;
  write(data: { text?: string; html?: string; image?: NativeImage; rtf?: string; bookmark?: string }): void;
};
export const shell: {
  openExternal(url: string, options?: { activate?: boolean; workingDirectory?: string }): Promise<void>;
  openPath(path: string): Promise<string>;
  showItemInFolder(fullPath: string): void;
  trashItem(path: string): Promise<void>;
};
export const dialog: {
  showMessageBox(browserWindow: BrowserWindow, options: MessageBoxOptions): Promise<{ response: number; checkboxChecked: boolean }>;
  showMessageBox(options: MessageBoxOptions): Promise<{ response: number; checkboxChecked: boolean }>;
  showOpenDialog(browserWindow: BrowserWindow, options: OpenDialogOptions): Promise<{ canceled: boolean; filePaths: string[]; bookmarks?: string[] }>;
  showOpenDialog(options: OpenDialogOptions): Promise<{ canceled: boolean; filePaths: string[]; bookmarks?: string[] }>;
};

export interface NotificationConstructorOptions { title: string; subtitle?: string; body?: string; silent?: boolean; icon?: string | NativeImage; urgency?: "normal" | "critical" | "low"; timeoutType?: "default" | "never" }
export class Notification extends EventEmitter {
  constructor(options: NotificationConstructorOptions);
  static isSupported(): boolean;
  show(): void;
  close(): void;
}
export interface Tray extends EventEmitter {
  setImage(image: NativeImage | string): void;
  setToolTip(toolTip: string): void;
  setContextMenu(menu: Menu | null): void;
  destroy(): void;
  isDestroyed(): boolean;
}
export const Tray: { new(image: NativeImage | string): Tray };

export interface IpcMain extends EventEmitter {
  handle<Args extends unknown[], Result>(channel: string, listener: (event: IpcMainInvokeEvent, ...args: Args) => Result): void;
  handleOnce<Args extends unknown[], Result>(channel: string, listener: (event: IpcMainInvokeEvent, ...args: Args) => Result): void;
  removeHandler(channel: string): void;
}
export const ipcMain: IpcMain;
export interface IpcRenderer extends EventEmitter {
  send(channel: string, ...args: unknown[]): void;
  sendSync(channel: string, ...args: unknown[]): unknown;
  invoke(channel: string, ...args: unknown[]): Promise<unknown>;
}
export const ipcRenderer: IpcRenderer;
export interface ContextBridge { exposeInMainWorld(apiKey: string, api: object): void; executeInMainWorld<T>(executionScript: { func: (...args: unknown[]) => T; args?: unknown[] }): Promise<T> }
export const contextBridge: ContextBridge;
export const webUtils: { getPathForFile(file: File): string };

export interface ProtocolRequest { url: string; method: string; referrer: string; headers: Record<string, string>; uploadData?: Array<{ bytes?: Buffer; file?: string }> }
export const protocol: {
  registerSchemesAsPrivileged(customSchemes: Array<{ scheme: string; privileges?: { standard?: boolean; secure?: boolean; supportFetchAPI?: boolean; bypassCSP?: boolean; allowServiceWorkers?: boolean; stream?: boolean; codeCache?: boolean } }>): void;
  handle(scheme: string, handler: (request: Request) => Response | Promise<Response>): Promise<void>;
  unhandle(scheme: string): Promise<void>;
};
export const screen: { getPrimaryDisplay(): Display; getAllDisplays(): Display[]; getDisplayNearestPoint(point: Point): Display; getCursorScreenPoint(): Point };
export const session: { readonly defaultSession: Session; fromPartition(partition: string, options?: { cache?: boolean }): Session };

export interface ClientRequest extends Writable {
  setHeader(name: string, value: string | string[]): void;
  getHeader(name: string): string | string[] | undefined;
  removeHeader(name: string): void;
  abort(): void;
  followRedirect(): void;
}
export interface IncomingMessage extends Readable { statusCode: number; statusMessage: string; headers: IncomingHttpHeaders; httpVersion: string }
export const net: {
  fetch(input: string | Request, init?: RequestInit): Promise<Response>;
  request(options: string | { method?: string; url: string; session?: Session; redirect?: "follow" | "error" | "manual" }): ClientRequest;
  isOnline(): boolean;
};
export interface AutoUpdater extends EventEmitter {
  setFeedURL(options: string | { url: string; headers?: Record<string, string>; serverType?: "json" | "default" }): void;
  getFeedURL(): string;
  checkForUpdates(): void;
  quitAndInstall(): void;
}
// Windows provides the NSIS lifecycle event source; native updater methods are macOS-only.
export const autoUpdater: EventEmitter | AutoUpdater;
export const powerMonitor: EventEmitter & { getSystemIdleTime(): number; getSystemIdleState(threshold: number): "active" | "idle" | "locked" | "unknown" };
export const nativeTheme: EventEmitter & {
  readonly shouldUseDarkColors: boolean;
  readonly shouldUseHighContrastColors: boolean;
  readonly shouldUseInvertedColorScheme: boolean;
  themeSource: "system" | "light" | "dark";
};
