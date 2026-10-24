# Development and shared runtime behavior

[Documentation index](README.md) · [Project overview](../README.md)

> Run commands from the repository root unless a section explicitly changes directory.

Platform prerequisites and API coverage: [Windows](WINDOWS.md) · [macOS](MACOS.md). Graphics support: [WebGL and WebGPU](GRAPHICS.md).

## Shared Electron example

Python 3.10 or newer is required on macOS; Windows requires Python 3.12 or newer.
From the repository root, on either supported OS:

```sh
python build.py --electron --interactive
```

`examples/electron/main.js` exercises readiness, window bounds/registry/title,
`loadFile()`, and `webContents.getURL()`. Without `--interactive`, `--electron`
runs a short native-window smoke check and exits. Use `--jobs N` to control build
parallelism and `--out PATH` to change the output directory.

To run another compatible application:

```sh
python build.py --electron --interactive --electron-main /path/to/main.js
```

On Windows the script's containing directory is packaged as `resources/app`,
with `package.json.main` set to the chosen filename. The macOS main-process host
uses Node module loading, accepts an application directory or main script, and
discovers packaged `Contents/Resources/app.asar` or `app`. Its
`ELECTRON_RUN_AS_NODE` route registers ASAR support before entering Node.

### Windows Notification lifecycle (issue #8)

Source builds fix the VERSION_4 callback/temporary-tray leak reported in
[issue #8](https://github.com/iris-cat-dev/mini-electron/issues/8).
Notification IDs come from `HIWORD(lParam)` and events from `LOWORD(lParam)`;
`wParam` is not an ID in this protocol. IDs stay within the 16-bit range and
allocation skips active IDs on wraparound.

The balloon host registers a valid `NIF_ICON` icon with `NIS_HIDDEN`, negotiates
VERSION_4, then submits the balloon. A rejected version or balloon submission
deletes the temporary icon and emits `failed`, rather than assuming negotiation
succeeded. Custom balloon icons use `NIIF_LARGE_ICON` and the system icon
dimensions, including DPI; any resized handle is released after submission.
Timeout/hide delete the temporary icon and emit `close`; click deletes it and
emits `click` followed by `close`. Explicit `close()` also deletes the icon.
Notification cleanup does not alter the application's independent `Tray`.
See the [Microsoft notification contract](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-notifyicondataw).

Run against a freshly rebuilt executable, not the installed 0.1.18 runtime:

```sh
./out/windows-x64/mini-electron.exe --app=test/windows-notification-smoke.js
./out/windows-x64/mini-electron.exe --single-process --app=test/windows-notification-smoke.js
# Optional: requires actual Shell delivery and natural expiration.
./out/windows-x64/mini-electron.exe --app=test/windows-notification-smoke.js --natural-timeout
```

The regression compiles a C# helper with the system .NET Framework compiler.
It verifies the host's PID before posting synthetic VERSION_4 callbacks and
uses zero-flag `NIM_MODIFY` to probe registration without changing icon fields.
`Shell_NotifyIconGetRect` alone cannot distinguish a hidden icon from a deleted
one. It checks multiple notifications, undefined/misleading `wParam`, unknown
and duplicate callbacks, re-show/reentrant close listeners, and resident Tray
preservation.

Local Windows 10 x64 build 19045 verification passed both process modes using a
targeted rebuild of `notification.cc` linked with retained runtime objects and
libraries. Active temporary icons had no tray position; terminal callbacks left
all tested notification IDs unregistered while the resident Tray stayed
registered. This is native Shell API evidence, not an expanded-overflow pixel
inspection. An optional natural-expiration run did not receive a real Shell
timeout within 120 seconds; no notification banner was observed. Synthetic
timeout/click verification is not proof of natural delivery or a physical user
click. Windows 11, the original packaged OMP Desktop, a full-tree rebuild, and
a published Windows runtime were not verified for this change.


### safeStorage

Source builds replace the fixed-key XOR backend reported in
[issue #6](https://github.com/iris-cat-dev/mini-electron/issues/6).

- **Windows:** uses `CryptProtectData` / `CryptUnprotectData` with current-user
  DPAPI protection and no UI prompts. `isEncryptionAvailable()` returns false
  before app readiness, and only returns true after a successful OS-backed
  encrypt/decrypt probe. Each operation still throws if DPAPI subsequently fails.
  This protects against other users, not other applications running as that user.
- **Other platforms:** no secure backend is implemented; availability is false
  and both encryption and decryption throw. There is no plaintext or fixed-key
  fallback. `setUsePlainTextEncryption()` does not enable one.
- Ciphertext is a native authenticated DPAPI blob, without a `v10`/`v11` wrapper
  or application-managed key file. It is not Electron's profile-key ciphertext
  format. Legacy fixed-key XOR output is rejected rather than silently migrated;
  applications must obtain and securely store replacement values.
- Strings use UTF-8 with explicit lengths, including Unicode and embedded NULs.
  Encrypting an empty string produces a DPAPI blob. Decrypting an empty Buffer
  returns `''` when encryption is available; non-Buffer arguments and malformed
  or authentication-failing ciphertext throw.

After rebuilding on Windows, run the application-mode regression (requires
Windows PowerShell for independent .NET DPAPI interoperability checks):

```sh
out/windows-x64/mini-electron.exe --app=test/safe-storage-smoke.js
```

Windows native verification on 2026-10-23 passed the actual application-mode
regression against the current DPAPI implementation, including independent
`.NET ProtectedData` exchange in both directions, ASCII/Unicode/empty/embedded-NUL
values, malformed and legacy-XOR ciphertext rejection, and persisted decryption
after a process restart with another application profile. Only synthetic values
were used. Cross-user decryption and Windows 11 were not exercised.
This was a targeted Windows x64 Release compile/link with retained native
libraries, not a new published Windows runtime; existing release assets are not
updated by a source checkout.

### Preload module scope

The shared Windows/macOS preload runner gives each session and window preload
its own function scope. Electron APIs are obtained with `require('electron')`;
the wrapper does not inject an `electron` parameter. This fixes
[issue #5](https://github.com/iris-cat-dev/mini-electron/issues/5), where
`electron-log@5.4.4`'s top-level `let electron` collided with that parameter.
Syntax and runtime failures still emit `webContents`' `preload-error` with the
source filename and do not prevent later preloads from running.

After rebuilding, run the native regression on either platform:

```sh
out/mac-arm64/mini-electron test/preload-scope-smoke.js
./out/windows-x64/runtime-dist/electron.exe test/preload-scope-smoke.js
```

The regression covers `let`/`const`/`var` Electron imports, independent session,
window, and page bindings, IPC, navigation/reload, and intentional preload
failures with context isolation enabled and disabled. A targeted macOS native
build also ran unmodified `electron-log@5.4.4` with
`log.initialize({ spyRendererConsole: true })`: two windows and three document
loads each delivered all six renderer logs to the main logger without a
`preload-error`. This was a targeted compile/link against retained native build
artifacts, not a successful full-tree rebuild. The full-build attempt stopped in
Blink's `storage_controller.cc` on an incomplete `InspectorDOMStorageAgent` type.
The existing IPC frame smoke passed; an additional single-process smoke on the
targeted executable failed because renderer and host PIDs differed. These checks
do not establish single-process compatibility or Windows packaged-app acceptance.

Windows native verification on 2026-10-23 also found and repaired the separate
DevTools-free console path: Windows now installs a stateless native V8 console
delegate for `log`/`info`/`warn`/`error`/`debug`, forwards the actual severity and
source location, and publishes `app`'s `web-contents-created` event so logging
libraries can subscribe before page output. It does not enable Inspector or CDP.
Creation listeners that destroy contents or throw abort owner construction safely
and preserve the original exception. Root-display resize is now ACK-driven and
asynchronous, avoiding a Blink wait for a future frame during hidden-window load.

The targeted native executable passed:
- `test/preload-scope-smoke.js` in default and `--single-process` modes.
- `test/console-message-smoke.js` in both modes: 72 exactly-once messages per run,
  standard levels, document URLs/lines, main/iframe, hidden-window resize,
  concurrent windows, navigation/reload, and native argument formatting.
- `test/web-contents-created-smoke.js` in both modes: destroy/throw reentrancy,
  owner cleanup, and subsequent real BrowserWindow/BrowserView operation.
- `test/electron-log-smoke.js` with unmodified npm `electron-log@5.4.4` and the exact
  `log.initialize({ spyRendererConsole: true })` initialization in both modes:
  four windows and 12 document loads per run, context isolation on/off, all 24
  explicit IPC and console-spy logs received, no unexpected preload errors.
- `test/ipc-frame-smoke.js` and `test/safe-storage-smoke.js`.

The electron-log consumer regression requires that package through normal Node
module resolution. From the repository root in a Bash-compatible shell, using a
**rebuilt** Windows executable:

```sh
npm install --no-save --prefix out/verification/issue5-deps electron-log@5.4.4
NODE_PATH=./out/verification/issue5-deps/node_modules ./out/windows-x64/runtime-dist/electron.exe --app=test/electron-log-smoke.js
NODE_PATH=./out/verification/issue5-deps/node_modules ./out/windows-x64/runtime-dist/electron.exe --single-process --app=test/electron-log-smoke.js
```

Verification limits: 13 current native translation units were rebuilt and linked
with retained V8/Blink/Node libraries; this is not a full-tree rebuild or a
published Windows release. The full local build stopped at pinned ANGLE checkout
preparation. A separate `windows-resize-presentation-smoke.js` single-process
stress still reports transient white backing samples during native sizing; that
failure was observed before and after the asynchronous resize change and is not
claimed fixed. The original packaged OMP Desktop was not run. Local commands,
exit codes, package integrity and binary hashes are under
`out/verification/issue5-current-main/`.

After rebuilding, run the native regressions above, then validate the real OMP
Desktop package with its normal logging initialization and unmodified preload.
Keep preload execution and `preload-error` reporting enabled. Old Windows
`0.1.17`/`0.1.18` download artifacts do not acquire these source fixes.

## Trusted single-process UI mode

Source builds support an opt-in mode for packaged or otherwise trusted UI.
The application host and all WebContents run in one OS process, with one shared
Blink rendering thread and separate page JavaScript contexts. The default remains
one sandboxed renderer process per WebContents.

```sh
out/windows-x64/mini-electron.exe --single-process --app=/path/to/main.js
```

An application can select the same mode before creating its first BrowserWindow,
BrowserView, webview, or other WebContents:

```js
const { app, BrowserWindow } = require('electron');
app.commandLine.appendSwitch('single-process');

app.whenReady().then(() => {
  const window = new BrowserWindow({ width: 480, height: 320 });
  window.loadFile('index.html');
});
```

The choice is process-wide and fixed when the first WebContents is constructed.
It is not a per-window `webPreferences` setting; `sandbox: false` alone does not
select it, and `sandbox: true` cannot isolate a window in single-process mode.
`webContents.getProcessId()` returns the host PID and preload `process.sandboxed`
is `false`. Context isolation and the existing preload `electron` API remain
available; this mode does not enable unrestricted Node `require()` inside pages
or preload scripts.

Commands, events and renderer replies retain the normal asynchronous client
interface but move through in-process task delivery rather than serialized
renderer command pipes. Windows GPU/capture and guest mappings take owned local
references synchronously. Closing one view does not shut down the shared engine;
the engine, compositor and Blink isolates stop before Node's V8 platform is
disposed at application exit.

There is no renderer OS sandbox or crash isolation in this mode. A native page
crash terminates the application, and a long-running script can block every page
on the shared Blink thread. Explicit `child_process`, utility-process and Node
Worker operations keep their normal semantics. This reduces renderer-process
startup and fixed per-process overhead; it does not remove Blink/V8 from the
binary, and no memory-saving percentage is promised.

Windows runtime regressions:

```sh
out/windows-x64/stage/mini-electron.exe --single-process --app=test/single-process-smoke.js
MINI_SINGLE_PROCESS_API=1 out/windows-x64/stage/mini-electron.exe --app=test/single-process-smoke.js
MINI_SINGLE_PROCESS_EXPECT_DEFAULT=1 out/windows-x64/stage/mini-electron.exe --app=test/single-process-smoke.js
```

The smoke checks real process IDs, preload isolation, JavaScript dialogs,
bidirectional IPC, same-origin storage in distinct session partitions, rendered
pixels, cancellation of pending work, surviving-window behavior, and reopening
after all windows close. It passed with CLI/API selection, GPU and software
rendering, and the unchanged default process model on Windows.

Additional single-process checks passed `execute-javascript-smoke.js`,
`ipc-frame-smoke.js`, `windows-companion-smoke.js` (real desktop pixels, alpha,
focus and taskbar transitions), and `webgl-smoke.js` (including Dedicated Worker
OffscreenCanvas). The full GPU compositor/VSync smoke did not pass: this session
selects a timer clock in both process modes, rather than hardware DXGI/DWM VSync.
The previously recorded `websocket-smoke.js` text/binary failure is fixed in the
current Windows source build: the bundled curl decoder now preserves FIN and
the original message type across continuation and interleaved control frames.
The actual text/binary, ping/pong and close smoke passes in both process modes.
The macOS source path is integrated but has not been built or run for this change.

## WebSocket receive backpressure

Received payload credit stays outstanding through native assembly, browser tasks,
IPC, renderer tasks and Blink delivery. It returns only after native payload
destruction and consumption, or explicit cancellation. An unread WebSocketStream
holds credit until its reader resumes; paused documents do not shift unbounded
payloads into another task or MessageEvent queue.

Each WebContents has an 8 MiB ordinary receive window and a 68 MiB hard aggregate
credit ceiling. One fragmented message may borrow the oversize reserve so the
existing 60 MiB individual-message limit can complete. Every message costs at
least 4 KiB of credit, including empty messages. These are payload-accounting
limits, not a promise that process commit equals the credited byte count.
Separate windows have independent budgets even when they share a Session.
Document replacement, iframe detachment, cancellation and disposal retire the
owning document's reservations without releasing another renderer's credit.

Run the actual native-runtime smokes in the default process mode, and the two
lifecycle smokes again with `--single-process`:

```sh
out/windows-x64/stage/mini-electron.exe --app=test/websocket-smoke.js
out/windows-x64/stage/mini-electron.exe --app=test/websocket-backpressure-smoke.js
out/windows-x64/stage/mini-electron.exe --app=test/websocket-receive-lifecycle-smoke.js
out/windows-x64/stage/mini-electron.exe --app=test/websocket-frame-retirement-smoke.js
```

Windows verification covers blocked-producer progress/resumption, exact-60-MiB
fragmentation with an empty final continuation, empty messages, aggregate sockets,
unread stream cancellation, repeated navigation and iframe removal. Shared macOS
sources changed, but this repair has not been built or exercised on macOS.
