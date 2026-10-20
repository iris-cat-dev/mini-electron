// Native input driver for Windows smoke tests. Requires an interactive desktop.
using System;
using System.Collections;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;

internal static class NativeInputHelper
{
    [StructLayout(LayoutKind.Sequential)] private struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct GuiThreadInfo {
        public uint Size, Flags;
        public IntPtr Active, Focus, Capture, MenuOwner, MoveSize, Caret;
        public Rect CaretRect;
    }
    [StructLayout(LayoutKind.Sequential)] private struct KeyboardInput {
        public ushort VirtualKey, Scan; public uint Flags, Time; public UIntPtr Extra;
    }
    [StructLayout(LayoutKind.Sequential)] private struct MouseInput {
        public int X, Y; public uint Data, Flags, Time; public UIntPtr Extra;
    }
    [StructLayout(LayoutKind.Explicit)] private struct InputUnion {
        [FieldOffset(0)] public KeyboardInput Keyboard;
        [FieldOffset(0)] public MouseInput Mouse;
    }
    [StructLayout(LayoutKind.Sequential)] private struct Input { public uint Type; public InputUnion Data; }
    [DllImport("user32.dll")] private static extern bool SetProcessDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr window, StringBuilder text, int size);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(IntPtr window, StringBuilder text, int size);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] private static extern IntPtr GetWindowLongPtr(IntPtr window, int index);
    [DllImport("user32.dll")] private static extern IntPtr GetWindow(IntPtr window, uint command);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll", EntryPoint = "GetClassLongPtrW")] private static extern IntPtr GetClassLongPtr(IntPtr window, int index);
    [DllImport("user32.dll", SetLastError = true)] private static extern IntPtr SendMessageTimeout(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam, uint flags, uint timeout, out UIntPtr result);
    [DllImport("user32.dll")] private static extern bool ClientToScreen(IntPtr window, ref Point point);
    [DllImport("user32.dll")] private static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")] private static extern IntPtr WindowFromPoint(Point point);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] private static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] private static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")] private static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] private static extern bool IsZoomed(IntPtr window);
    [DllImport("user32.dll")] private static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] private static extern IntPtr GetKeyboardLayout(uint thread);
    [DllImport("user32.dll")] private static extern uint GetDoubleClickTime();
    [DllImport("user32.dll")] private static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] private static extern short GetAsyncKeyState(int key);
    [DllImport("user32.dll")] private static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool GetGUIThreadInfo(uint thread, ref GuiThreadInfo info);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] private static extern uint MapVirtualKey(uint key, uint mode);
    [DllImport("user32.dll", SetLastError = true)] private static extern uint SendInput(uint count, Input[] input, int size);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool SystemParametersInfo(uint action, uint parameter, out uint value, uint update);
    [DllImport("user32.dll")] private static extern IntPtr GetDC(IntPtr window);
    [DllImport("user32.dll")] private static extern int ReleaseDC(IntPtr window, IntPtr dc);
    [DllImport("gdi32.dll")] private static extern uint GetPixel(IntPtr dc, int x, int y);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool OpenClipboard(IntPtr owner);
    [DllImport("user32.dll")] private static extern bool CloseClipboard();
    [DllImport("user32.dll")] private static extern bool EmptyClipboard();
    [DllImport("user32.dll")] private static extern uint EnumClipboardFormats(uint format);
    [DllImport("user32.dll")] private static extern IntPtr GetClipboardData(uint format);
    [DllImport("user32.dll", SetLastError = true)] private static extern IntPtr SetClipboardData(uint format, IntPtr value);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern uint RegisterClipboardFormat(string name);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr GlobalAlloc(uint flags, UIntPtr size);
    [DllImport("kernel32.dll")] private static extern IntPtr GlobalLock(IntPtr value);
    [DllImport("kernel32.dll")] private static extern bool GlobalUnlock(IntPtr value);
    [DllImport("kernel32.dll")] private static extern UIntPtr GlobalSize(IntPtr value);
    [DllImport("kernel32.dll")] private static extern IntPtr GlobalFree(IntPtr value);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GetProcessHandleCount(IntPtr process, out uint count);

    private sealed class ClipboardItem { public uint Format; public byte[] Bytes; }
    private static readonly JavaScriptSerializer Json = new JavaScriptSerializer();
    private static Form form;
    private static Button source;
    private static Panel control;
    private static Dictionary<string, object> drag;
    private static List<ClipboardItem> savedClipboard;
    private static int accepted;
    private static bool heldLeftButton;
    private static IntPtr heldPointerWindow;

    private sealed class PresentationPoint {
        public int X, Y, Red, Green, Blue, Tolerance;
        public bool FollowBottom;
        public int MatchingFrames, WhiteFrames;
        public double FirstMatchMs = -1, LastMismatchMs = -1;
        public readonly int[] FirstRgb = new int[3], LastRgb = new int[3];
    }
    private sealed class RestorePresentationSampler {
        public IntPtr Window;
        public int DurationMs, StableMs;
        public List<PresentationPoint> Points;
        public Thread Worker;
        public readonly ManualResetEvent Ready = new ManualResetEvent(false);
        public Exception Error;
        public volatile bool Stop;
        public string FailurePath;
        public int Frames, PresentedFrames, MissingFrames, AllWhiteFrames, AnyWhiteFrames;
        public int MissingAfterStableFrames, WhiteAfterStableFrames;
        public double ActualDurationMs, FirstPresentedMs = -1, FirstStablePresentedMs = -1;
        public double LastMissingMs = -1, MissingDurationMs, LongestMissingMs;
        public double WhiteDurationMs, LongestWhiteMs;
    }
    private static RestorePresentationSampler restorePresentation;

    private static int Number(Dictionary<string, object> command, string key) { return Convert.ToInt32(command[key]); }
    private static bool Flag(Dictionary<string, object> command, string key) { return command.ContainsKey(key) && Convert.ToBoolean(command[key]); }
    private static void Reply(Dictionary<string, object> command, object value, Exception error) {
        Console.WriteLine("NATIVE_RESULT " + Json.Serialize(new { id = command["id"], result = value, error = error == null ? null : error.ToString() }));
        Console.Out.Flush();
    }
    private static void Send(Input input) {
        if (SendInput(1, new[] { input }, Marshal.SizeOf(typeof(Input))) != 1) throw new Win32Exception();
    }
    private static void Key(ushort key, bool up) {
        uint scan = MapVirtualKey(key, 4);
        // MapVirtualKey omits E0 for the dedicated navigation cluster; without
        // it these are physical numpad keys, not Home/End/arrows/Delete.
        bool extended = (scan & 0xff00) == 0xe000 || (key >= 0x21 && key <= 0x28)
            || key == 0x2d || key == 0x2e || key == 0x90;
        Send(new Input { Type = 1, Data = new InputUnion { Keyboard = new KeyboardInput {
            VirtualKey = key, Scan = (ushort)(scan & 255), Flags = (up ? 2u : 0u) | (extended ? 1u : 0u)
        } } });
    }
    private static void Mouse(bool up) {
        Send(new Input { Type = 0, Data = new InputUnion { Mouse = new MouseInput { Flags = up ? 4u : 2u } } });
    }
    private static void Click(int x, int y) {
        if (!SetCursorPos(x, y)) throw new Win32Exception();
        Mouse(false); Thread.Sleep(50); Mouse(true);
    }
    private static void ValidateClientPoint(IntPtr window, int x, int y) {
        Rect client; Point origin = new Point();
        if (GetForegroundWindow() != window || !GetClientRect(window, out client)
            || !ClientToScreen(window, ref origin)) {
            IntPtr foreground = GetForegroundWindow();
            var caption = new StringBuilder(256); var className = new StringBuilder(256);
            GetWindowText(foreground, caption, caption.Capacity);
            GetClassName(foreground, className, className.Capacity);
            throw new InvalidOperationException("Refusing pointer input for an unexpected foreground window: class="
                + className + ", title=" + caption);
        }
        if (x < origin.X || x >= origin.X + client.Right - client.Left
            || y < origin.Y || y >= origin.Y + client.Bottom - client.Top)
            throw new InvalidOperationException("Pointer target is outside the native test window client");
    }
    private static void SendPointerMove(IntPtr window, int x, int y) {
        ValidateClientPoint(window, x, y);
        int left = GetSystemMetrics(76), top = GetSystemMetrics(77);
        int width = GetSystemMetrics(78), height = GetSystemMetrics(79);
        if (width <= 1 || height <= 1)
            throw new InvalidOperationException("Windows virtual desktop has invalid dimensions");
        if (x < left || x >= left + width || y < top || y >= top + height)
            throw new InvalidOperationException("Pointer target is outside the Windows virtual desktop");
        long normalizedX = (long)(x - left) * 65535 / (width - 1);
        long normalizedY = (long)(y - top) * 65535 / (height - 1);
        Send(new Input { Type = 0, Data = new InputUnion { Mouse = new MouseInput {
            X = (int)normalizedX, Y = (int)normalizedY, Flags = 0x0001u | 0x4000u | 0x8000u
        } } });
    }
    private static void MovePointer(IntPtr window, int x, int y) {
        SendPointerMove(window, x, y);
        Thread.Sleep(50);
    }
    private static void ReleaseHeldPointer() {
        if (!heldLeftButton) return;
        try { Mouse(true); }
        finally { heldLeftButton = false; heldPointerWindow = IntPtr.Zero; }
    }
    private static object ResizeCycles(Dictionary<string, object> command) {
        if (!command.ContainsKey("samples")) return ResizeCyclesCore(command);
        IntPtr window = Window(command);
        Focus(window);
        Point origin = new Point();
        if (!ClientToScreen(window, ref origin)) throw new Win32Exception();
        double scale = GetDpiForWindow(window) / 96.0;
        var points = new List<PresentationPoint>();
        foreach (Dictionary<string, object> input in (IEnumerable)command["samples"]) {
            var rgb = new List<int>();
            foreach (object channel in (IEnumerable)input["rgb"]) rgb.Add(Convert.ToInt32(channel));
            if (rgb.Count != 3) throw new InvalidOperationException("Invalid resize sample color");
            bool bottom = Flag(input, "bottom");
            points.Add(new PresentationPoint {
                X = origin.X + (int)Math.Round(Number(input, "x") * scale),
                Y = (bottom ? 0 : origin.Y) + (int)Math.Round(Number(input, "y") * scale),
                Red = rgb[0], Green = rgb[1], Blue = rgb[2], Tolerance = 5, FollowBottom = bottom
            });
        }
        var sampler = new RestorePresentationSampler {
            Window = window, DurationMs = 20000, StableMs = 10, Points = points,
            FailurePath = command.ContainsKey("failurePath") ? Convert.ToString(command["failurePath"]) : null
        };
        sampler.Worker = new Thread(() => SampleRestorePresentation(sampler)) { IsBackground = true };
        sampler.Worker.Start();
        object resizing;
        try {
            if (!sampler.Ready.WaitOne(2000)) throw new InvalidOperationException("Resize presentation sampler did not start");
            resizing = ResizeCyclesCore(command);
        } finally {
            sampler.Stop = true;
            if (!sampler.Worker.Join(3000)) throw new InvalidOperationException("Resize presentation sampler did not stop");
        }
        if (sampler.Error != null) throw sampler.Error;
        return new { resizing, presentation = new {
            source = "desktop.GetDC/GetPixel", frames = sampler.Frames, missingFrames = sampler.MissingFrames,
            whiteFrames = sampler.AnyWhiteFrames, missingMs = sampler.MissingDurationMs,
            longestMissingMs = sampler.LongestMissingMs, points
        } };
    }
    private static object ResizeCyclesCore(Dictionary<string, object> command) {
        IntPtr window = Window(command);
        Focus(window);
        int cycles = Number(command, "cycles");
        if (cycles < 1 || cycles > 200 || heldLeftButton || (GetAsyncKeyState(1) & 0x8000) != 0)
            throw new InvalidOperationException("Invalid native resize cycles or held pointer");
        var sizes = new List<object>();
        int distance = command.ContainsKey("distance") ? Number(command, "distance") : 96;
        int steps = command.ContainsKey("steps") ? Number(command, "steps") : 8;
        int interval = command.ContainsKey("interval") ? Number(command, "interval") : 8;
        if (distance < 16 || distance > 1000 || steps < 2 || steps > 40 || interval < 1 || interval > 20)
            throw new InvalidOperationException("Invalid native resize motion");
        Rect initial;
        if (!GetWindowRect(window, out initial)) throw new Win32Exception();
        uint processId;
        uint targetThread = GetWindowThreadProcessId(window, out processId);
        int entered = 0;
        for (int cycle = 0; cycle < cycles; cycle++) {
            Rect before;
            if (GetForegroundWindow() != window || !GetWindowRect(window, out before))
                throw new InvalidOperationException("Native resize target lost foreground");
            int edge = (cycle / 2) % 3;
            int x = before.Right - 2, y = before.Bottom - 2;
            if (edge == 0) y = (before.Top + before.Bottom) / 2;
            if (edge == 1) x = (before.Left + before.Right) / 2;
            UIntPtr hit;
            long position = (ushort)x | ((long)(ushort)y << 16);
            if (SendMessageTimeout(window, 0x0084, UIntPtr.Zero, new IntPtr(position), 2, 1000, out hit) == IntPtr.Zero
                || hit.ToUInt64() < 10 || hit.ToUInt64() > 17)
                throw new InvalidOperationException("Native resize did not hit a window sizing edge");
            if (!SetCursorPos(x, y) || WindowFromPoint(new Point { X = x, Y = y }) != window)
                throw new InvalidOperationException("Native resize edge is obscured");
            Mouse(false);
            heldLeftButton = true;
            heldPointerWindow = window;
            try {
                var started = System.Diagnostics.Stopwatch.StartNew();
                for (;;) {
                    var info = new GuiThreadInfo { Size = (uint)Marshal.SizeOf(typeof(GuiThreadInfo)) };
                    if (!GetGUIThreadInfo(targetThread, ref info)) throw new Win32Exception();
                    if (info.MoveSize == window) break;
                    if (started.ElapsedMilliseconds > 2000) {
                        Point cursor; GetCursorPos(out cursor);
                        throw new InvalidOperationException("Native mouse did not enter the Win32 sizing loop: cycle=" + cycle
                            + ", hit=" + hit + ", target=" + window + ", foreground=" + GetForegroundWindow()
                            + ", moveSize=" + info.MoveSize + ", capture=" + info.Capture
                            + ", expectedPointer=" + x + "," + y + ", actualPointer=" + cursor.X + "," + cursor.Y);
                    }
                    Thread.Sleep(2);
                }
                entered++;
                int targetX = initial.Right - 2;
                int targetY = initial.Bottom - 2;
                if (cycle % 2 == 0) {
                    targetX -= Math.Min(distance, initial.Right - initial.Left - 300);
                    targetY -= Math.Min(distance * 2 / 3, initial.Bottom - initial.Top - 250);
                }
                for (int step = 1; step <= steps; step++) {
                    if (GetForegroundWindow() != window)
                        throw new InvalidOperationException("Native resize target lost foreground during drag");
                    int moveX = edge == 1 ? x : x + (targetX - x) * step / steps;
                    int moveY = edge == 0 ? y : y + (targetY - y) * step / steps;
                    if (!SetCursorPos(moveX, moveY)) throw new Win32Exception();
                    Thread.Sleep(interval);
                }
            } finally { ReleaseHeldPointer(); }
            var released = System.Diagnostics.Stopwatch.StartNew();
            for (;;) {
                var info = new GuiThreadInfo { Size = (uint)Marshal.SizeOf(typeof(GuiThreadInfo)) };
                if (!GetGUIThreadInfo(targetThread, ref info)) throw new Win32Exception();
                if (info.MoveSize != window) break;
                if (released.ElapsedMilliseconds > 2000)
                    throw new InvalidOperationException("Native sizing loop did not exit after mouse release");
                Thread.Sleep(5);
            }
            Rect after;
            if (!GetWindowRect(window, out after)) throw new Win32Exception();
            if (before.Right - before.Left == after.Right - after.Left
                && before.Bottom - before.Top == after.Bottom - after.Top)
                throw new InvalidOperationException("Native sizing loop did not change window geometry");
            sizes.Add(new { width = after.Right - after.Left, height = after.Bottom - after.Top });
        }
        return new { entered, sizes };
    }
    private static IntPtr Window(Dictionary<string, object> command) {
        IntPtr window = command.ContainsKey("hwnd") ? new IntPtr(Convert.ToInt64(command["hwnd"])) : FindWindow(null, Convert.ToString(command["title"]));
        if (window == IntPtr.Zero || !IsWindow(window)) throw new InvalidOperationException("Native test window not found");
        return window;
    }
    private static object NativeRect(Rectangle rect) {
        return new { x = rect.X, y = rect.Y, width = rect.Width, height = rect.Height };
    }
    private static bool HasWindowIcon(IntPtr window) {
        UIntPtr result;
        foreach (UIntPtr kind in new[] { new UIntPtr(0), new UIntPtr(1), new UIntPtr(2) })
            if (SendMessageTimeout(window, 0x007f, kind, IntPtr.Zero, 2, 1000, out result) != IntPtr.Zero
                && result != UIntPtr.Zero) return true;
        return GetClassLongPtr(window, -14) != IntPtr.Zero || GetClassLongPtr(window, -34) != IntPtr.Zero;
    }
    private static void Focus(IntPtr window) {
        if (GetForegroundWindow() == window) return;
        bool accepted = SetForegroundWindow(window);
        // Activation crosses input queues asynchronously. Synchronize with the
        // target before checking; a fixed sleep can sample the old foreground.
        UIntPtr result;
        if (accepted && SendMessageTimeout(window, 0, UIntPtr.Zero, IntPtr.Zero, 2, 2000, out result) == IntPtr.Zero)
            throw new InvalidOperationException("Native test window did not process activation");
        IntPtr foreground = GetForegroundWindow();
        if (foreground != window) {
            uint owner, helperOwner; GetWindowThreadProcessId(foreground, out owner);
            GetWindowThreadProcessId(form.Handle, out helperOwner);
            throw new InvalidOperationException("Cannot focus native test window " + window.ToInt64()
                + "; accepted=" + accepted + ", foreground=" + foreground.ToInt64()
                + ", foreground PID=" + owner + ", helper PID=" + helperOwner);
        }
    }

    private static RestorePresentationSampler BeginRestorePresentation(Dictionary<string, object> command) {
        if (restorePresentation != null)
            throw new InvalidOperationException("A restore presentation sample is already active");
        IntPtr window = Window(command);
        if (!IsIconic(window))
            throw new InvalidOperationException("Restore presentation sampling requires a minimized window");
        int duration = Number(command, "duration");
        int stable = command.ContainsKey("stable") ? Number(command, "stable") : 40;
        int tolerance = command.ContainsKey("tolerance") ? Number(command, "tolerance") : 6;
        if (duration < 250 || duration > 5000 || stable < 10 || stable > 500
            || tolerance < 0 || tolerance > 64)
            throw new InvalidOperationException("Invalid restore presentation timing or tolerance");

        int virtualLeft = GetSystemMetrics(76), virtualTop = GetSystemMetrics(77);
        int virtualWidth = GetSystemMetrics(78), virtualHeight = GetSystemMetrics(79);
        var points = new List<PresentationPoint>();
        foreach (Dictionary<string, object> input in (IEnumerable)command["samples"]) {
            var expected = new List<int>();
            foreach (object channel in (IEnumerable)input["rgb"]) expected.Add(Convert.ToInt32(channel));
            int x = Number(input, "x"), y = Number(input, "y");
            if (expected.Count != 3 || expected.Exists(channel => channel < 0 || channel > 255)
                || x < virtualLeft || x >= virtualLeft + virtualWidth
                || y < virtualTop || y >= virtualTop + virtualHeight)
                throw new InvalidOperationException("Invalid restore presentation sample point");
            points.Add(new PresentationPoint { X = x, Y = y, Red = expected[0],
                Green = expected[1], Blue = expected[2], Tolerance = tolerance });
        }
        if (points.Count == 0 || points.Count > 16)
            throw new InvalidOperationException("Invalid restore presentation sample count");

        var sampler = new RestorePresentationSampler {
            Window = window, DurationMs = duration, StableMs = stable, Points = points
        };
        sampler.Worker = new Thread(() => SampleRestorePresentation(sampler)) { IsBackground = true };
        restorePresentation = sampler;
        sampler.Worker.Start();
        if (!sampler.Ready.WaitOne(2000)) {
            restorePresentation = null;
            throw new InvalidOperationException("Desktop presentation sampler did not start");
        }
        if (sampler.Error != null) {
            restorePresentation = null;
            throw sampler.Error;
        }
        return sampler;
    }
    private static void SampleRestorePresentation(RestorePresentationSampler sampler) {
        IntPtr dc = IntPtr.Zero;
        try {
            dc = GetDC(IntPtr.Zero);
            if (dc == IntPtr.Zero) throw new Win32Exception("Cannot acquire the desktop DC");
            var clock = System.Diagnostics.Stopwatch.StartNew();
            double previousMs = 0, matchingSinceMs = -1, missingSinceMs = -1, whiteSinceMs = -1;
            bool previousMissing = false, previousWhite = false;
            do {
                double nowMs = clock.Elapsed.TotalMilliseconds;
                if (sampler.Frames > 0) {
                    double elapsed = nowMs - previousMs;
                    if (previousMissing) sampler.MissingDurationMs += elapsed;
                    if (previousWhite) sampler.WhiteDurationMs += elapsed;
                }
                bool allMatching = true, allWhite = true, anyWhite = false;
                foreach (PresentationPoint point in sampler.Points) {
                    int y = point.Y;
                    if (point.FollowBottom) {
                        Rect client; Point origin = new Point();
                        if (!GetClientRect(sampler.Window, out client) || !ClientToScreen(sampler.Window, ref origin))
                            throw new Win32Exception();
                        y = origin.Y + client.Bottom - point.Y;
                    }
                    uint color = GetPixel(dc, point.X, y);
                    if (color == 0xffffffff) throw new Win32Exception("Cannot read desktop presentation pixel");
                    int red = (int)(color & 255), green = (int)((color >> 8) & 255), blue = (int)((color >> 16) & 255);
                    point.LastRgb[0] = red; point.LastRgb[1] = green; point.LastRgb[2] = blue;
                    if (sampler.Frames == 0) {
                        point.FirstRgb[0] = red; point.FirstRgb[1] = green; point.FirstRgb[2] = blue;
                    }
                    bool matching = Math.Abs(red - point.Red) <= point.Tolerance
                        && Math.Abs(green - point.Green) <= point.Tolerance
                        && Math.Abs(blue - point.Blue) <= point.Tolerance;
                    bool white = red >= 245 && green >= 245 && blue >= 245;
                    if (matching) {
                        point.MatchingFrames++;
                        if (point.FirstMatchMs < 0) point.FirstMatchMs = nowMs;
                    } else {
                        point.LastMismatchMs = nowMs;
                        allMatching = false;
                    }
                    if (white) point.WhiteFrames++;
                    else allWhite = false;
                    anyWhite |= white;
                }
                if (allMatching) {
                    sampler.PresentedFrames++;
                    if (sampler.FirstPresentedMs < 0) sampler.FirstPresentedMs = nowMs;
                    if (matchingSinceMs < 0) matchingSinceMs = nowMs;
                    if (sampler.FirstStablePresentedMs < 0
                        && nowMs - matchingSinceMs >= sampler.StableMs)
                        sampler.FirstStablePresentedMs = matchingSinceMs;
                    if (missingSinceMs >= 0) {
                        sampler.LongestMissingMs = Math.Max(sampler.LongestMissingMs, nowMs - missingSinceMs);
                        missingSinceMs = -1;
                    }
                } else {
                    sampler.MissingFrames++;
                    sampler.LastMissingMs = nowMs;
                    matchingSinceMs = -1;
                    if (missingSinceMs < 0) missingSinceMs = nowMs;
                    if (sampler.FirstStablePresentedMs >= 0) sampler.MissingAfterStableFrames++;
                }
                if (!allMatching && sampler.FailurePath != null) {
                    Rect bounds;
                    if (!GetWindowRect(sampler.Window, out bounds)) throw new Win32Exception();
                    using (var bitmap = new Bitmap(bounds.Right - bounds.Left, bounds.Bottom - bounds.Top))
                    using (var graphics = Graphics.FromImage(bitmap)) {
                        graphics.CopyFromScreen(bounds.Left, bounds.Top, 0, 0, bitmap.Size);
                        bitmap.Save(sampler.FailurePath, ImageFormat.Png);
                    }
                    sampler.FailurePath = null;
                }
                if (allWhite) {
                    sampler.AllWhiteFrames++;
                    if (whiteSinceMs < 0) whiteSinceMs = nowMs;
                    if (sampler.FirstStablePresentedMs >= 0) sampler.WhiteAfterStableFrames++;
                } else if (whiteSinceMs >= 0) {
                    sampler.LongestWhiteMs = Math.Max(sampler.LongestWhiteMs, nowMs - whiteSinceMs);
                    whiteSinceMs = -1;
                }
                if (anyWhite) sampler.AnyWhiteFrames++;
                sampler.Frames++;
                if (sampler.Frames == 1) sampler.Ready.Set();
                previousMs = nowMs;
                previousMissing = !allMatching;
                previousWhite = allWhite;
                if (nowMs >= sampler.DurationMs || sampler.Stop) break;
                Thread.Sleep(1);
            } while (true);
            sampler.ActualDurationMs = clock.Elapsed.TotalMilliseconds;
            if (missingSinceMs >= 0)
                sampler.LongestMissingMs = Math.Max(sampler.LongestMissingMs, sampler.ActualDurationMs - missingSinceMs);
            if (whiteSinceMs >= 0)
                sampler.LongestWhiteMs = Math.Max(sampler.LongestWhiteMs, sampler.ActualDurationMs - whiteSinceMs);
        } catch (Exception error) {
            sampler.Error = error;
        } finally {
            sampler.Ready.Set();
            if (dc != IntPtr.Zero) ReleaseDC(IntPtr.Zero, dc);
        }
    }
    private static object FinishRestorePresentation() {
        RestorePresentationSampler sampler = restorePresentation;
        if (sampler == null)
            throw new InvalidOperationException("No restore presentation sample is active");
        try {
            if (!sampler.Worker.Join(sampler.DurationMs + 3000))
                throw new InvalidOperationException("Desktop presentation sampler did not finish");
            if (sampler.Error != null) throw sampler.Error;
            Rect client; Point origin = new Point();
            if (!GetClientRect(sampler.Window, out client) || !ClientToScreen(sampler.Window, ref origin))
                throw new Win32Exception();
            var points = new List<object>();
            foreach (PresentationPoint point in sampler.Points) points.Add(new {
                x = point.X, y = point.Y, rgb = new[] { point.Red, point.Green, point.Blue },
                matchingFrames = point.MatchingFrames, whiteFrames = point.WhiteFrames,
                firstMatchMs = point.FirstMatchMs, lastMismatchMs = point.LastMismatchMs,
                firstRgb = point.FirstRgb, lastRgb = point.LastRgb
            });
            return new {
                source = "desktop.GetDC/GetPixel",
                frames = sampler.Frames, presentedFrames = sampler.PresentedFrames,
                missingFrames = sampler.MissingFrames, allWhiteFrames = sampler.AllWhiteFrames,
                anyWhiteFrames = sampler.AnyWhiteFrames,
                firstPresentedMs = sampler.FirstPresentedMs,
                firstStablePresentedMs = sampler.FirstStablePresentedMs,
                lastMissingMs = sampler.LastMissingMs,
                missingDurationMs = sampler.MissingDurationMs,
                longestMissingMs = sampler.LongestMissingMs,
                whiteDurationMs = sampler.WhiteDurationMs,
                longestWhiteMs = sampler.LongestWhiteMs,
                missingAfterStableFrames = sampler.MissingAfterStableFrames,
                whiteAfterStableFrames = sampler.WhiteAfterStableFrames,
                durationMs = sampler.ActualDurationMs,
                minimized = IsIconic(sampler.Window), maximized = IsZoomed(sampler.Window),
                foreground = GetForegroundWindow() == sampler.Window,
                clientX = origin.X, clientY = origin.Y,
                clientWidth = client.Right - client.Left, clientHeight = client.Bottom - client.Top,
                points = points
            };
        } finally {
            restorePresentation = null;
        }
    }
    private static object SamplePointerPresentation(Dictionary<string, object> command) {
        IntPtr window = Window(command);
        var target = (Dictionary<string, object>)command["point"];
        int x = Number(target, "x"), y = Number(target, "y");
        ValidateClientPoint(window, x, y);
        int duration = Number(command, "durationMs"), tolerance = Number(command, "tolerance");
        if (duration < 50 || duration > 3000 || tolerance < 0 || tolerance > 20)
            throw new InvalidOperationException("Invalid pointer presentation duration or tolerance");
        var points = new List<Point>();
        var expected = new List<int[]>();
        foreach (Dictionary<string, object> point in (IEnumerable)command["samples"]) {
            int px = Number(point, "x"), py = Number(point, "y");
            ValidateClientPoint(window, px, py);
            var rgb = new List<int>();
            foreach (object channel in (IEnumerable)point["rgb"]) rgb.Add(Convert.ToInt32(channel));
            if (rgb.Count != 3 || rgb.Exists(channel => channel < 0 || channel > 255))
                throw new InvalidOperationException("Invalid presentation sample color");
            points.Add(new Point { X = px, Y = py });
            expected.Add(rgb.ToArray());
        }
        if (points.Count == 0 || points.Count > 16)
            throw new InvalidOperationException("Invalid pointer presentation sample count");
        var colors = new int[points.Count][];
        for (int index = 0; index < colors.Length; ++index) colors[index] = new int[3];
        int frames = 0, mismatchAfterMatch = 0, whiteFrames = 0;
        double firstMatchMs = -1;
        IntPtr dc = GetDC(IntPtr.Zero);
        if (dc == IntPtr.Zero) throw new Win32Exception("Cannot acquire desktop presentation DC");
        var clock = System.Diagnostics.Stopwatch.StartNew();
        try {
            SendPointerMove(window, x, y);
            do {
                IntPtr foreground = GetForegroundWindow();
                if (foreground != window) {
                    uint owner; GetWindowThreadProcessId(foreground, out owner);
                    var caption = new StringBuilder(256); var className = new StringBuilder(256);
                    GetWindowText(foreground, caption, caption.Capacity);
                    GetClassName(foreground, className, className.Capacity);
                    throw new InvalidOperationException("Pointer presentation target lost foreground: target="
                        + window.ToInt64() + ", foreground=" + foreground.ToInt64() + ", foreground PID=" + owner
                        + ", class=" + className + ", title=" + caption);
                }
                bool matching = true, white = false;
                for (int index = 0; index < points.Count; ++index) {
                    Point point = points[index];
                    uint rgb = GetPixel(dc, point.X, point.Y);
                    if (rgb == 0xffffffff) throw new Win32Exception("Cannot read desktop presentation pixel");
                    int[] color = colors[index], wanted = expected[index];
                    color[0] = (int)(rgb & 255);
                    color[1] = (int)((rgb >> 8) & 255);
                    color[2] = (int)((rgb >> 16) & 255);
                    for (int channel = 0; channel < 3; ++channel)
                        if (Math.Abs(color[channel] - wanted[channel]) > tolerance) matching = false;
                    if (color[0] >= 245 && color[1] >= 245 && color[2] >= 245) white = true;
                }
                if (matching && firstMatchMs < 0) firstMatchMs = clock.Elapsed.TotalMilliseconds;
                else if (!matching && firstMatchMs >= 0) mismatchAfterMatch++;
                if (white) whiteFrames++;
                frames++;
                Thread.Sleep(1);
            } while (clock.ElapsedMilliseconds < duration);
        } finally { ReleaseDC(IntPtr.Zero, dc); }
        return new { source = "desktop.GetDC/GetPixel", frames, firstMatchMs,
            mismatchAfterMatch, whiteFrames, durationMs = clock.Elapsed.TotalMilliseconds, colors };
    }
    private static object SamplePointerMotion(Dictionary<string, object> command) {
        IntPtr window = Window(command);
        var moves = new List<Point>();
        var samples = new List<Point>();
        foreach (Dictionary<string, object> point in (IEnumerable)command["moves"])
            moves.Add(new Point { X = Number(point, "x"), Y = Number(point, "y") });
        foreach (Dictionary<string, object> point in (IEnumerable)command["samples"])
            samples.Add(new Point { X = Number(point, "x"), Y = Number(point, "y") });
        int cycles = Number(command, "cycles");
        if (moves.Count < 2 || moves.Count > 16 || samples.Count == 0 || samples.Count > 16 || cycles < 1 || cycles > 10)
            throw new InvalidOperationException("Invalid pointer motion sample");
        foreach (Point point in moves) ValidateClientPoint(window, point.X, point.Y);
        foreach (Point point in samples) ValidateClientPoint(window, point.X, point.Y);
        IntPtr dc = GetDC(IntPtr.Zero);
        if (dc == IntPtr.Zero) throw new Win32Exception();
        Exception movementError = null;
        var worker = new Thread(() => {
            try {
                for (int cycle = 0; cycle < cycles; ++cycle)
                    foreach (Point point in moves) {
                        Focus(window);
                        MovePointer(window, point.X, point.Y);
                    }
            } catch (Exception error) { movementError = error; }
        });
        int frames = 0;
        var missing = new int[samples.Count];
        uint lastMissingColor = 0;
        try {
            worker.Start();
            do {
                for (int index = 0; index < samples.Count; ++index) {
                    Point point = samples[index];
                    uint rgb = GetPixel(dc, point.X, point.Y);
                    if (rgb == 0xffffffff) throw new Win32Exception("Cannot read native caption pixel");
                    if ((rgb & 255) < 240 || ((rgb >> 8) & 255) < 240 || ((rgb >> 16) & 255) < 240) {
                        missing[index]++;
                        lastMissingColor = rgb;
                    }
                }
                frames++;
                Thread.Sleep(1);
            } while (worker.IsAlive);
        } finally {
            if (worker.IsAlive) worker.Join();
            ReleaseDC(IntPtr.Zero, dc);
        }
        if (movementError != null) throw movementError;
        return new { frames, missing, lastMissingColor, source = "desktop.GetDC/GetPixel" };
    }
    private static void Open() {
        for (int i = 0; i != 30; ++i) { if (OpenClipboard(form.Handle)) return; Thread.Sleep(10); }
        throw new Win32Exception("Cannot open the Windows clipboard");
    }
    private static void SaveClipboard() {
        var items = new List<ClipboardItem>();
        Open();
        try {
            uint format = 0;
            while ((format = EnumClipboardFormats(format)) != 0) {
                if (format == 2 || format == 3 || format == 9 || format == 14 || (format >= 0x80 && format <= 0x8e))
                    throw new InvalidOperationException("Clipboard contains a non-HGLOBAL format; refusing to overwrite it");
                IntPtr handle = GetClipboardData(format);
                ulong size = GlobalSize(handle).ToUInt64();
                if (size == 0 || size > 64 * 1024 * 1024) throw new InvalidOperationException("Cannot losslessly preserve clipboard format " + format);
                IntPtr data = GlobalLock(handle);
                if (data == IntPtr.Zero) throw new Win32Exception();
                try { var bytes = new byte[(int)size]; Marshal.Copy(data, bytes, 0, bytes.Length); items.Add(new ClipboardItem { Format = format, Bytes = bytes }); }
                finally { GlobalUnlock(handle); }
            }
        } finally { CloseClipboard(); }
        savedClipboard = items;
    }
    private static void WriteClipboard(List<ClipboardItem> items) {
        Open();
        try {
            if (!EmptyClipboard()) throw new Win32Exception();
            foreach (ClipboardItem item in items) {
                IntPtr memory = GlobalAlloc(0x42, new UIntPtr((uint)item.Bytes.Length));
                if (memory == IntPtr.Zero) throw new Win32Exception();
                bool transferred = false;
                try {
                    IntPtr data = GlobalLock(memory);
                    if (data == IntPtr.Zero) throw new Win32Exception();
                    try { Marshal.Copy(item.Bytes, 0, data, item.Bytes.Length); } finally { GlobalUnlock(memory); }
                    if (SetClipboardData(item.Format, memory) == IntPtr.Zero) throw new Win32Exception();
                    transferred = true;
                } finally { if (!transferred) GlobalFree(memory); }
            }
        } finally { CloseClipboard(); }
    }
    private static void RestoreClipboard() {
        if (savedClipboard == null) return;
        WriteClipboard(savedClipboard);
        savedClipboard = null;
    }
    private static byte[] ImageBytes(ImageFormat format) {
        using (var bitmap = new Bitmap(16, 16)) using (var graphics = Graphics.FromImage(bitmap)) using (var stream = new MemoryStream()) {
            graphics.Clear(Color.FromArgb(203, 44, 55)); bitmap.Save(stream, format); return stream.ToArray();
        }
    }
    private static object Execute(Dictionary<string, object> command) {
        string action = Convert.ToString(command["action"]);
        if (action == "clipboardSave") { SaveClipboard(); return true; }
        if (action == "clipboardRestore") { RestoreClipboard(); return true; }
        if (action == "clipboardWrite") {
            var items = new List<ClipboardItem>();
            foreach (Dictionary<string, object> item in (IEnumerable)command["formats"]) {
                string name = Convert.ToString(item["name"]);
                uint format = name == "UnicodeText" ? 13u : name == "DIB" ? 8u : name == "DIBV5" ? 17u : RegisterClipboardFormat(name);
                items.Add(new ClipboardItem { Format = format, Bytes = Convert.FromBase64String(Convert.ToString(item["data"])) });
            }
            WriteClipboard(items); return true;
        }
        if (action == "clipboardRead") {
            Open();
            try {
                IntPtr memory = GetClipboardData(13), text = memory == IntPtr.Zero ? IntPtr.Zero : GlobalLock(memory);
                try {
                    var formats = new List<uint>(); uint format = 0;
                    while ((format = EnumClipboardFormats(format)) != 0) formats.Add(format);
                    return new { text = text == IntPtr.Zero ? null : Marshal.PtrToStringUni(text), formats = formats, html = RegisterClipboardFormat("HTML Format") };
                } finally { if (text != IntPtr.Zero) GlobalUnlock(memory); }
            } finally { CloseClipboard(); }
        }
        if (action == "images") {
            byte[] bmp = ImageBytes(ImageFormat.Bmp), dib = new byte[bmp.Length - 14];
            Buffer.BlockCopy(bmp, 14, dib, 0, dib.Length);
            return new { png = Convert.ToBase64String(ImageBytes(ImageFormat.Png)), dib = Convert.ToBase64String(dib) };
        }
        if (action == "geometry") {
            IntPtr window = Window(command); Rect rect, client; Point point = new Point();
            if (!GetWindowRect(window, out rect) || !GetClientRect(window, out client)
                || !ClientToScreen(window, ref point)) throw new Win32Exception();
            uint owner; uint thread = GetWindowThreadProcessId(window, out owner);
            return new { left = rect.Left, top = rect.Top, right = rect.Right, bottom = rect.Bottom,
                clientX = point.X, clientY = point.Y, clientWidth = client.Right - client.Left,
                clientHeight = client.Bottom - client.Top, dpi = GetDpiForWindow(window),
                maximized = IsZoomed(window), minimized = IsIconic(window),
                keyboardLayout = GetKeyboardLayout(thread).ToInt64().ToString("X") };
        }
        if (action == "nativeWindowState") {
            IntPtr window = Window(command);
            var title = new StringBuilder(512); var className = new StringBuilder(512);
            GetWindowText(window, title, title.Capacity);
            GetClassName(window, className, className.Capacity);
            return new {
                window = window.ToInt64(),
                foreground = GetForegroundWindow().ToInt64(),
                style = GetWindowLongPtr(window, -16).ToInt64(),
                exStyle = GetWindowLongPtr(window, -20).ToInt64(),
                visible = IsWindowVisible(window),
                minimized = IsIconic(window),
                owner = GetWindow(window, 4).ToInt64(),
                className = className.ToString(),
                title = title.ToString(),
                hasIcon = HasWindowIcon(window)
            };
        }
        if (action == "nativeScreenState") {
            IntPtr window = Window(command);
            Screen primary = Screen.PrimaryScreen, target = Screen.FromHandle(window);
            Point cursor;
            if (!GetCursorPos(out cursor)) throw new Win32Exception();
            return new {
                primary = new { bounds = NativeRect(primary.Bounds), workArea = NativeRect(primary.WorkingArea) },
                target = new { bounds = NativeRect(target.Bounds), workArea = NativeRect(target.WorkingArea) },
                windowDpi = GetDpiForWindow(window),
                cursor = new { x = cursor.X, y = cursor.Y }
            };
        }
        if (action == "guiState") {
            IntPtr window = Window(command); uint process;
            uint thread = GetWindowThreadProcessId(window, out process);
            var info = new GuiThreadInfo { Size = (uint)Marshal.SizeOf(typeof(GuiThreadInfo)) };
            if (!GetGUIThreadInfo(thread, ref info)) throw new Win32Exception();
            Point caret = new Point { X = info.CaretRect.Left, Y = info.CaretRect.Bottom };
            if (info.Caret != IntPtr.Zero && !ClientToScreen(info.Caret, ref caret)) throw new Win32Exception();
            return new { window = window.ToInt64(), focus = info.Focus.ToInt64(), caret = info.Caret.ToInt64(),
                left = info.CaretRect.Left, top = info.CaretRect.Top, right = info.CaretRect.Right,
                bottom = info.CaretRect.Bottom, screenX = caret.X, screenY = caret.Y };
        }
        if (action == "restorePresentationBegin") { BeginRestorePresentation(command); return true; }
        if (action == "restorePresentationEnd") return FinishRestorePresentation();
        if (action == "restorePresentation") {
            RestorePresentationSampler sampler = BeginRestorePresentation(command);
            ShowWindow(sampler.Window, 9);
            return FinishRestorePresentation();
        }
        if (action == "sample") {
            IntPtr window = Window(command); Rect rect;
            IntPtr expected = Flag(command, "inactive") ? form.Handle : window;
            if (GetForegroundWindow() != expected || !GetWindowRect(window, out rect))
                throw new InvalidOperationException("Selection sample has unexpected foreground window");
            int x = Number(command, "x"), y = Number(command, "y");
            int width = Number(command, "width"), height = Number(command, "height");
            if (width <= 0 || height <= 0 || width > 1000 || height > 300
                || x < rect.Left || y < rect.Top || x + width > rect.Right || y + height > rect.Bottom)
                throw new InvalidOperationException("Invalid selection sample bounds");
            var counts = new Dictionary<int, int>();
            using (var bitmap = new Bitmap(width, height)) using (var graphics = Graphics.FromImage(bitmap)) {
                graphics.CopyFromScreen(x, y, 0, 0, bitmap.Size);
                for (int row = 0; row < height; ++row) for (int column = 0; column < width; ++column) {
                    int rgb = bitmap.GetPixel(column, row).ToArgb() & 0xffffff, count;
                    counts.TryGetValue(rgb, out count); counts[rgb] = count + 1;
                }
            }
            var sorted = new List<KeyValuePair<int, int>>(counts);
            sorted.Sort((left, right) => right.Value.CompareTo(left.Value));
            var colors = new List<object>();
            for (int i = 0; i < Math.Min(12, sorted.Count); ++i) {
                int rgb = sorted[i].Key;
                colors.Add(new { rgb = new[] { (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255 }, count = sorted[i].Value });
            }
            return colors;
        }
        if (action == "capture") {
            IntPtr window = Window(command); Rect rect;
            if (GetForegroundWindow() != (Flag(command, "inactive") ? form.Handle : window) || !GetWindowRect(window, out rect))
                throw new InvalidOperationException("Cannot capture an unexpected foreground window");
            using (var bitmap = new Bitmap(rect.Right - rect.Left, rect.Bottom - rect.Top))
            using (var graphics = Graphics.FromImage(bitmap)) {
                graphics.CopyFromScreen(rect.Left, rect.Top, 0, 0, bitmap.Size);
                bitmap.Save(Convert.ToString(command["path"]), ImageFormat.Png);
            }
            return true;
        }
        if (action == "focus") { Focus(Window(command)); return true; }
        if (action == "focusSource") { Focus(form.Handle); return true; }
        if (action == "move") {
            if (!SetWindowPos(Window(command), IntPtr.Zero, Number(command, "x"), Number(command, "y"), 0, 0, 0x15))
                throw new Win32Exception();
            return true;
        }
        if (action == "processHandleCount") {
            int processId = Number(command, "processId");
            if (processId <= 0) throw new InvalidOperationException("Invalid process ID");
            using (var process = System.Diagnostics.Process.GetProcessById(processId)) {
                uint count;
                if (!GetProcessHandleCount(process.Handle, out count)) throw new Win32Exception();
                return count;
            }
        }
        if (action == "resizeCycles") return ResizeCycles(command);
        if (action == "samplePointerMotion") return SamplePointerMotion(command);
        if (action == "samplePointerPresentation") return SamplePointerPresentation(command);
        if (action == "pointerMove") {
            IntPtr window = Window(command);
            if (heldLeftButton && heldPointerWindow != window)
                throw new InvalidOperationException("Refusing to move a held pointer over another window");
            MovePointer(window, Number(command, "x"), Number(command, "y"));
            return true;
        }
        if (action == "pointerProbe") {
            IntPtr window = Window(command);
            Point cursor;
            if (!GetCursorPos(out cursor)) throw new Win32Exception();
            Point expected = new Point { X = Number(command, "x"), Y = Number(command, "y") };
            uint owner; uint thread = GetWindowThreadProcessId(window, out owner);
            var info = new GuiThreadInfo { Size = (uint)Marshal.SizeOf(typeof(GuiThreadInfo)) };
            if (!GetGUIThreadInfo(thread, ref info)) throw new Win32Exception();
            IntPtr foreground = GetForegroundWindow();
            uint foregroundOwner, helperOwner;
            GetWindowThreadProcessId(foreground, out foregroundOwner);
            GetWindowThreadProcessId(form.Handle, out helperOwner);
            string foregroundProcessName = "";
            if (foregroundOwner != owner) {
                try { using (var process = System.Diagnostics.Process.GetProcessById((int)foregroundOwner)) foregroundProcessName = process.ProcessName; }
                catch (ArgumentException) { foregroundProcessName = "exited"; }
                catch (Win32Exception) { foregroundProcessName = "unavailable"; }
            }
            return new {
                expected = new { x = expected.X, y = expected.Y },
                cursor = new { x = cursor.X, y = cursor.Y },
                target = window.ToInt64(),
                foreground = foreground.ToInt64(),
                targetProcess = owner, foregroundProcess = foregroundOwner, helperProcess = helperOwner,
                foregroundProcessName,
                windowAtExpected = WindowFromPoint(expected).ToInt64(),
                windowAtCursor = WindowFromPoint(cursor).ToInt64(),
                active = info.Active.ToInt64(),
                focus = info.Focus.ToInt64(),
                capture = info.Capture.ToInt64()
            };
        }
        if (action == "pointerDown") {
            IntPtr window = Window(command);
            Focus(window);
            if (heldLeftButton || (GetAsyncKeyState(1) & 0x8000) != 0)
                throw new InvalidOperationException("Refusing to press an already-held left pointer button");
            MovePointer(window, Number(command, "x"), Number(command, "y"));
            Mouse(false);
            heldLeftButton = true;
            heldPointerWindow = window;
            return true;
        }
        if (action == "pointerUp") {
            IntPtr window = Window(command);
            if (!heldLeftButton || heldPointerWindow != window)
                throw new InvalidOperationException("No held pointer belongs to the native test window");
            try { MovePointer(window, Number(command, "x"), Number(command, "y")); }
            finally { ReleaseHeldPointer(); }
            return true;
        }
        if (action == "wheelSettings") {
            uint lines, chars;
            if (!SystemParametersInfo(0x0068, 0, out lines, 0)
                || !SystemParametersInfo(0x006C, 0, out chars, 0))
                throw new Win32Exception();
            return new { lines = lines, chars = chars, pageScroll = UInt32.MaxValue };
        }
        if (action == "wheel") {
            IntPtr window = Window(command);
            Focus(window);
            int x = Number(command, "x"), y = Number(command, "y");
            ValidateClientPoint(window, x, y);
            if (!SetCursorPos(x, y)) throw new Win32Exception();
            Send(new Input { Type = 0, Data = new InputUnion { Mouse = new MouseInput {
                Data = unchecked((uint)Number(command, "delta")),
                Flags = Flag(command, "horizontal") ? 0x1000u : 0x0800u
            } } });
            return true;
        }
        if (action == "key") {
            Focus(Window(command)); var keys = new List<ushort>();
            foreach (object key in (IEnumerable)command["keys"]) keys.Add(Convert.ToUInt16(key));
            try { foreach (ushort key in keys) { Key(key, false); Thread.Sleep(25); } }
            finally { for (int i = keys.Count - 1; i >= 0; --i) { Key(keys[i], true); Thread.Sleep(25); } }
            return true;
        }
        if (action == "unicode") {
            Focus(Window(command));
            foreach (char unit in Convert.ToString(command["text"])) {
                Send(new Input { Type = 1, Data = new InputUnion { Keyboard = new KeyboardInput { Scan = unit, Flags = 4 } } });
                Send(new Input { Type = 1, Data = new InputUnion { Keyboard = new KeyboardInput { Scan = unit, Flags = 6 } } });
            }
            return true;
        }
        if (action == "click" || action == "doubleClick") {
            Focus(Window(command)); Click(Number(command, "x"), Number(command, "y"));
            if (action == "doubleClick") {
                Thread.Sleep(Math.Max(20, (int)(GetDoubleClickTime() * 0.6) - 50));
                Click(Number(command, "x"), Number(command, "y"));
            }
            return true;
        }
        if (action == "quit") {
            if (restorePresentation != null) {
                RestorePresentationSampler sampler = restorePresentation;
                if (!sampler.Worker.Join(sampler.DurationMs + 3000))
                    throw new InvalidOperationException("Desktop presentation sampler did not finish during shutdown");
                restorePresentation = null;
            }
            ReleaseHeldPointer(); RestoreClipboard(); form.Close(); return true;
        }
        throw new InvalidOperationException("Unknown native command " + action);
    }
    private static void BeginDrag(Dictionary<string, object> command) {
        if (drag != null) throw new InvalidOperationException("A native drag is already active");
        Point origin = new Point { X = source.Width / 2, Y = source.Height / 2 };
        ClientToScreen(source.Handle, ref origin);
        Point destination = new Point { X = Number(command, "x"), Y = Number(command, "y") };
        if (Flag(command, "control")) {
            destination = new Point { X = control.Width / 2, Y = control.Height / 2 };
            ClientToScreen(control.Handle, ref destination);
        }
        Focus(form.Handle);
        if (WindowFromPoint(origin) != source.Handle)
            throw new InvalidOperationException("Native OLE source is obscured; refusing pointer input");
        drag = command; accepted = 0;
        new Thread(() => {
            try {
                SetCursorPos(origin.X, origin.Y); Thread.Sleep(80); Mouse(false); Thread.Sleep(100);
                for (int step = 1; step <= 36; ++step) {
                    SetCursorPos(origin.X + (destination.X - origin.X) * step / 36, origin.Y + (destination.Y - origin.Y) * step / 36);
                    Thread.Sleep(15);
                }
                Thread.Sleep(700);
                if (Flag(command, "cancel")) { Key(27, false); Key(27, true); }
                Mouse(true);
            } catch (Exception error) { Console.Error.WriteLine(error); Mouse(true); }
        }) { IsBackground = true }.Start();
    }
    [STAThread] private static void Main() {
        SetProcessDpiAwarenessContext(new IntPtr(-4));
        Console.InputEncoding = new UTF8Encoding(false); Console.OutputEncoding = new UTF8Encoding(false);
        Application.EnableVisualStyles();
        form = new Form { Text = "mini-electron native input source", AutoScaleMode = AutoScaleMode.None,
            StartPosition = FormStartPosition.Manual, Location = new System.Drawing.Point(20, Math.Max(20, Screen.PrimaryScreen.WorkingArea.Bottom - 210)),
            ClientSize = new Size(700, 160), TopMost = true, FormBorderStyle = FormBorderStyle.FixedToolWindow };
        source = new Button { Text = "Real OLE file source", Bounds = new Rectangle(15, 15, 290, 70) };
        control = new Panel { Bounds = new Rectangle(350, 15, 320, 120), AllowDrop = true, BackColor = Color.LightGreen };
        control.Controls.Add(new Label { Text = "Native positive-control drop target", Dock = DockStyle.Bottom });
        control.DragEnter += (sender, e) => { e.Effect = e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Copy : DragDropEffects.None; };
        control.DragOver += (sender, e) => { e.Effect = e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Copy : DragDropEffects.None; };
        control.DragDrop += (sender, e) => { accepted = ((string[])e.Data.GetData(DataFormats.FileDrop)).Length; e.Effect = DragDropEffects.Copy; };
        source.MouseDown += (sender, e) => {
            if (e.Button != MouseButtons.Left || drag == null) return;
            var command = drag;
            try {
                var data = new DataObject();
                if (Flag(command, "nonFile")) data.SetText("not a native file drag");
                else {
                    var paths = new List<string>(); foreach (object file in (IEnumerable)command["files"]) paths.Add(Convert.ToString(file));
                    data.SetData(DataFormats.FileDrop, true, paths.ToArray());
                }
                DragDropEffects effect = source.DoDragDrop(data, DragDropEffects.Copy);
                Reply(command, new { effect = effect.ToString(), controlAccepted = accepted }, null);
            } catch (Exception error) { Reply(command, null, error); }
            finally { drag = null; }
        };
        form.Controls.Add(source); form.Controls.Add(control);
        form.Shown += (sender, e) => {
            // Keep Node's console hidden, but undo its startup hide flag for the OLE source.
            ShowWindow(form.Handle, 5);
            Console.WriteLine("NATIVE_READY"); Console.Out.Flush();
            new Thread(() => {
                string line;
                while ((line = Console.ReadLine()) != null) {
                    try {
                        var command = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(line);
                        form.BeginInvoke(new Action(() => {
                            try { if (Convert.ToString(command["action"]) == "drag") BeginDrag(command); else Reply(command, Execute(command), null); }
                            catch (Exception error) { Reply(command, null, error); }
                        }));
                    } catch (Exception error) { Console.Error.WriteLine(error); }
                }
                if (!form.IsDisposed) form.BeginInvoke(new Action(() => form.Close()));
            }) { IsBackground = true }.Start();
        };
        form.FormClosing += (sender, e) => { ReleaseHeldPointer(); RestoreClipboard(); };
        Application.Run(form);
    }
}
