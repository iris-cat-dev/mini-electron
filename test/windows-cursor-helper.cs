// Real global cursor/input observations. Never changes cursor schemes or other system settings.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;

internal static class CursorHelper
{
    [StructLayout(LayoutKind.Sequential)] private struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct CursorInfo { public uint Size, Flags; public IntPtr Cursor; public Point Position; }
    [StructLayout(LayoutKind.Sequential)] private struct GuiThreadInfo {
        public uint Size, Flags;
        public IntPtr Active, Focus, Capture, MenuOwner, MoveSize, Caret;
        public Rect CaretRect;
    }
    [StructLayout(LayoutKind.Sequential)] private struct IconInfo { public int IsIcon; public uint HotspotX, HotspotY; public IntPtr Mask, Color; }
    [StructLayout(LayoutKind.Sequential)] private struct BitmapInfoHeader {
        public uint Size; public int Width, Height; public ushort Planes, Bits;
        public uint Compression, ImageSize; public int XPels, YPels; public uint Used, Important;
    }
    [StructLayout(LayoutKind.Sequential)] private struct NativeBitmap {
        public int Type, Width, Height, WidthBytes; public ushort Planes, Bits; public IntPtr Pixels;
    }
    [StructLayout(LayoutKind.Sequential)] private struct MouseInput { public int X, Y; public uint Data, Flags, Time; public UIntPtr Extra; }
    [StructLayout(LayoutKind.Explicit)] private struct InputUnion { [FieldOffset(0)] public MouseInput Mouse; }
    [StructLayout(LayoutKind.Sequential)] private struct Input { public uint Type; public InputUnion Data; }
    [DllImport("user32.dll")] private static extern bool SetProcessDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll", SetLastError = true)] private static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    [DllImport("user32.dll")] private static extern bool SwitchDesktop(IntPtr desktop);
    [DllImport("user32.dll")] private static extern bool CloseDesktop(IntPtr desktop);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll", SetLastError = true)] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool GetGUIThreadInfo(uint thread, ref GuiThreadInfo info);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] private static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll", SetLastError = true)] private static extern IntPtr SendMessageTimeout(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam, uint flags, uint timeout, out UIntPtr result);
    [DllImport("user32.dll")] private static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] private static extern bool ClientToScreen(IntPtr window, ref Point point);
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")] private static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")] private static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] private static extern IntPtr WindowFromPoint(Point point);
    [DllImport("user32.dll")] private static extern short GetAsyncKeyState(int key);
    [DllImport("user32.dll")] private static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll", SetLastError = true)] private static extern uint SendInput(uint count, Input[] input, int size);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool GetCursorInfo(ref CursorInfo info);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool GetIconInfo(IntPtr cursor, out IconInfo info);
    [DllImport("user32.dll")] private static extern IntPtr LoadCursorW(IntPtr module, IntPtr name);
    [DllImport("user32.dll")] private static extern IntPtr GetDC(IntPtr window);
    [DllImport("user32.dll")] private static extern int ReleaseDC(IntPtr window, IntPtr dc);
    [DllImport("gdi32.dll")] private static extern int GetObject(IntPtr bitmap, int size, out NativeBitmap data);
    [DllImport("gdi32.dll")] private static extern int GetDIBits(IntPtr dc, IntPtr bitmap, uint start, uint lines, byte[] pixels, ref BitmapInfoHeader info, uint usage);
    [DllImport("gdi32.dll")] private static extern bool DeleteObject(IntPtr obj);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll")] private static extern bool FreeLibrary(IntPtr module);

    private static readonly JavaScriptSerializer Json = new JavaScriptSerializer();
    private static readonly Dictionary<string, string> Expected = new Dictionary<string, string>();
    private static IntPtr module, heldWindow;
    private static uint held;
    private static Point original;
    private static bool moved;
    private static int Number(Dictionary<string, object> command, string key) { return Convert.ToInt32(command[key]); }
    private static void Desktop() {
        IntPtr desktop = OpenInputDesktop(0, false, 0x100);
        if (desktop == IntPtr.Zero) throw new InvalidOperationException("Unlock an interactive Windows input desktop before running this smoke");
        try {
            // This is the current input desktop, so this checks accessibility without switching away.
            if (!SwitchDesktop(desktop)) throw new InvalidOperationException("Windows input desktop is locked or not visible");
        } finally { CloseDesktop(desktop); }
    }
    private static IntPtr Window(string title) {
        IntPtr window = FindWindow(null, title);
        if (window == IntPtr.Zero || !IsWindowVisible(window) || IsIconic(window))
            throw new InvalidOperationException("Visible native test window not found: " + title);
        return window;
    }
    private static IntPtr Window(Dictionary<string, object> command) { return Window(Convert.ToString(command["title"])); }
    private static void Send(uint flags, int x, int y) {
        var input = new Input { Data = new InputUnion { Mouse = new MouseInput { X = x, Y = y, Flags = flags } } };
        if (SendInput(1, new[] { input }, Marshal.SizeOf(typeof(Input))) != 1) throw new Win32Exception();
    }
    private static void Release() {
        if ((held & 1) != 0) Send(4, 0, 0);
        if ((held & 2) != 0) Send(16, 0, 0);
        held = 0; heldWindow = IntPtr.Zero;
    }
    private static void Move(Dictionary<string, object> command, IntPtr window) {
        IntPtr expectedForeground = command.ContainsKey("foregroundTitle") ? Window(Convert.ToString(command["foregroundTitle"])) : window;
        if (GetForegroundWindow() != expectedForeground || (held != 0 && heldWindow != window))
            throw new InvalidOperationException("Refusing input: unexpected foreground window or capture owner");
        int x = Number(command, "x"), y = Number(command, "y");
        Rect rect; Point origin = new Point();
        if (!GetClientRect(window, out rect) || !ClientToScreen(window, ref origin)) throw new Win32Exception();
        if (x < origin.X || y < origin.Y || x >= origin.X + rect.Right || y >= origin.Y + rect.Bottom
            || WindowFromPoint(new Point { X = x, Y = y }) != window)
            throw new InvalidOperationException("Refusing input: intended native client point is outside or obscured");
        int left = GetSystemMetrics(76), top = GetSystemMetrics(77), width = GetSystemMetrics(78), height = GetSystemMetrics(79);
        if (width <= 1 || height <= 1 || x < left || y < top || x >= left + width || y >= top + height)
            throw new InvalidOperationException("Pointer target is outside the Windows virtual desktop");
        if (!moved) { if (!GetCursorPos(out original)) throw new Win32Exception(); moved = true; }
        Send(0xc001, (int)((long)(x - left) * 65535 / (width - 1)), (int)((long)(y - top) * 65535 / (height - 1)));
    }
    private sealed class Image {
        public int Width, Height;
        public uint HotspotX, HotspotY;
        public byte[] Color, Mask;
        public string Fingerprint;
    }
    private static byte[] Pixels(IntPtr bitmap, out int width, out int height) {
        NativeBitmap data;
        if (GetObject(bitmap, Marshal.SizeOf(typeof(NativeBitmap)), out data) == 0) throw new Win32Exception();
        width = data.Width; height = data.Height;
        if (width < 1 || height < 1 || width > 1024 || height > 2048) throw new InvalidOperationException("Invalid native cursor bitmap size");
        byte[] pixels = new byte[width * height * 4];
        var header = new BitmapInfoHeader { Size = (uint)Marshal.SizeOf(typeof(BitmapInfoHeader)), Width = width, Height = -height, Planes = 1, Bits = 32 };
        IntPtr dc = GetDC(IntPtr.Zero);
        if (dc == IntPtr.Zero) throw new Win32Exception();
        try {
            if (GetDIBits(dc, bitmap, 0, (uint)height, pixels, ref header, 0) != height) throw new Win32Exception();
        } finally { ReleaseDC(IntPtr.Zero, dc); }
        return pixels;
    }
    private static Image Inspect(IntPtr cursor) {
        IconInfo info;
        if (!GetIconInfo(cursor, out info)) throw new Win32Exception();
        try {
            var image = new Image { HotspotX = info.HotspotX, HotspotY = info.HotspotY };
            int width, height;
            image.Mask = Pixels(info.Mask, out width, out height);
            image.Width = width; image.Height = info.Color == IntPtr.Zero ? height / 2 : height;
            image.Color = info.Color == IntPtr.Zero ? new byte[0] : Pixels(info.Color, out width, out height);
            using (SHA256 hash = SHA256.Create()) {
                byte[] bytes = new byte[image.Color.Length + image.Mask.Length];
                Buffer.BlockCopy(image.Color, 0, bytes, 0, image.Color.Length);
                Buffer.BlockCopy(image.Mask, 0, bytes, image.Color.Length, image.Mask.Length);
                image.Fingerprint = image.Width + "x" + image.Height + ":" + info.HotspotX + "," + info.HotspotY + ":" + Convert.ToBase64String(hash.ComputeHash(bytes));
            }
            return image;
        } finally {
            if (info.Mask != IntPtr.Zero) DeleteObject(info.Mask);
            if (info.Color != IntPtr.Zero) DeleteObject(info.Color);
        }
    }
    private static void LoadExpected(string executable) {
        if (module != IntPtr.Zero) return;
        module = LoadLibraryEx(executable, IntPtr.Zero, 2);
        if (module == IntPtr.Zero) throw new Win32Exception("Cannot inspect runtime cursor resources");
        var stocks = new Dictionary<string, int> {
            { "default", 32512 }, { "text", 32513 }, { "wait", 32514 }, { "crosshair", 32515 },
            { "nwse-resize", 32642 }, { "nesw-resize", 32643 }, { "ew-resize", 32644 }, { "ns-resize", 32645 },
            { "move", 32646 }, { "not-allowed", 32648 }, { "pointer", 32649 }, { "progress", 32650 }, { "help", 32651 }
        };
        foreach (var entry in stocks) {
            IntPtr cursor = LoadCursorW(IntPtr.Zero, new IntPtr(entry.Value));
            if (cursor == IntPtr.Zero) throw new Win32Exception("Missing stock cursor " + entry.Key);
            Expected.Add(entry.Key, Inspect(cursor).Fingerprint);
        }
        var resources = new Dictionary<string, int> {
            { "alias", 45000 }, { "cell", 45001 }, { "col-resize", 45002 }, { "copy", 45003 }, { "grab", 45004 }, { "grabbing", 45005 },
            { "row-resize", 45017 }, { "vertical-text", 45018 }, { "zoom-in", 45019 }, { "zoom-out", 45020 }
        };
        foreach (var entry in resources) {
            IntPtr cursor = LoadCursorW(module, new IntPtr(entry.Value));
            if (cursor == IntPtr.Zero) throw new Win32Exception("Missing embedded runtime cursor " + entry.Key + " (#" + entry.Value + ")");
            Expected.Add(entry.Key, Inspect(cursor).Fingerprint);
        }
    }
    private static object InputState(IntPtr window) {
        uint process;
        uint thread = GetWindowThreadProcessId(window, out process);
        var gui = new GuiThreadInfo { Size = (uint)Marshal.SizeOf(typeof(GuiThreadInfo)) };
        Point position;
        if (thread == 0 || !GetGUIThreadInfo(thread, ref gui) || !GetCursorPos(out position)) throw new Win32Exception();
        return new { x = position.X, y = position.Y, window = window.ToInt64(), thread, process,
            foreground = GetForegroundWindow().ToInt64(), active = gui.Active.ToInt64(), focus = gui.Focus.ToInt64(),
            capture = gui.Capture.ToInt64(), held, heldWindow = heldWindow.ToInt64(),
            buttons = (GetAsyncKeyState(1) < 0 ? 1 : 0) | (GetAsyncKeyState(2) < 0 ? 2 : 0) | (GetAsyncKeyState(4) < 0 ? 4 : 0) };
    }
    private static object Observe(Dictionary<string, object> command) {
        LoadExpected(Convert.ToString(command["executable"]));
        var state = new CursorInfo { Size = (uint)Marshal.SizeOf(typeof(CursorInfo)) };
        if (!GetCursorInfo(ref state)) throw new Win32Exception();
        IntPtr window = Window(command);
        if (WindowFromPoint(state.Position) != window) throw new InvalidOperationException("Native cursor is no longer over the test window");
        var matches = new List<string>();
        Image image = null;
        if (state.Cursor == IntPtr.Zero || (state.Flags & 1) == 0) matches.Add("none");
        else {
            image = Inspect(state.Cursor);
            foreach (var entry in Expected) if (entry.Value == image.Fingerprint) matches.Add(entry.Key);
            if (matches.Count == 0) matches.Add("custom");
        }
        int center = image != null && image.Color.Length != 0 ? ((image.Height / 2) * image.Width + image.Width / 2) * 4 : -1;
        long position = (ushort)state.Position.X | ((long)(ushort)state.Position.Y << 16);
        UIntPtr hit;
        if (SendMessageTimeout(window, 0x84, UIntPtr.Zero, new IntPtr(position), 2, 1000, out hit) == IntPtr.Zero) throw new Win32Exception();
        return new { source = "GetCursorInfo/GetIconInfo/GetDIBits", matches, handle = state.Cursor.ToInt64(), flags = state.Flags,
            x = state.Position.X, y = state.Position.Y, foreground = GetForegroundWindow().ToInt64(), window = window.ToInt64(),
            hit = hit.ToUInt64(), dpi = GetDpiForWindow(window), width = image == null ? 0 : image.Width, height = image == null ? 0 : image.Height,
            hotspotX = image == null ? 0 : image.HotspotX, hotspotY = image == null ? 0 : image.HotspotY,
            fingerprint = image == null ? null : image.Fingerprint,
            centerBGRA = center < 0 ? null : new[] { (int)image.Color[center], (int)image.Color[center + 1], (int)image.Color[center + 2], (int)image.Color[center + 3] } };
    }
    private static object Execute(Dictionary<string, object> command) {
        string action = Convert.ToString(command["action"]);
        if (action == "quit") return true;
        Desktop();
        IntPtr window = Window(command);
        if (action == "geometry") {
            Point origin = new Point(); Rect client;
            if (!ClientToScreen(window, ref origin) || !GetClientRect(window, out client)) throw new Win32Exception();
            return new { clientX = origin.X, clientY = origin.Y, clientWidth = client.Right, clientHeight = client.Bottom, dpi = GetDpiForWindow(window) };
        }
        if (action == "focus") {
            if (GetForegroundWindow() != window) {
                UIntPtr result;
                if (!SetForegroundWindow(window) || SendMessageTimeout(window, 0, UIntPtr.Zero, IntPtr.Zero, 2, 2000, out result) == IntPtr.Zero)
                    throw new InvalidOperationException("Cannot activate native test window");
            }
            if (GetForegroundWindow() != window) throw new InvalidOperationException("Unexpected foreground window after activation");
            return true;
        }
        if (action == "cursor") return Observe(command);
        if (action == "state") return InputState(window);
        if (action == "wakeup") {
            IntPtr target = FindWindow(null, Convert.ToString(command["targetTitle"]));
            uint process;
            if (target == IntPtr.Zero || GetWindowThreadProcessId(target, out process) == 0
                || process != Convert.ToUInt32(command["pid"]))
                throw new InvalidOperationException("Refusing cursor wakeup for an unowned window");
            UIntPtr result;
            if (SendMessageTimeout(target, 0x20, new UIntPtr(unchecked((ulong)target.ToInt64())), new IntPtr(1), 2, 1000, out result) == IntPtr.Zero)
                throw new Win32Exception();
            return true;
        }
        if (action == "move") { Move(command, window); return true; }
        if (action == "down" || action == "up") {
            uint button = Convert.ToString(command["button"]) == "right" ? 2u : 1u;
            if (GetForegroundWindow() != window) throw new InvalidOperationException("Refusing button input to an inactive window");
            if (action == "down" && ((held & button) != 0 || (held != 0 && heldWindow != window)
                || (held == 0 && (GetAsyncKeyState(1) < 0 || GetAsyncKeyState(2) < 0 || GetAsyncKeyState(4) < 0))))
                throw new InvalidOperationException("Refusing to press a button while an unexpected mouse button is held");
            if (action == "up" && ((held & button) == 0 || heldWindow != window)) throw new InvalidOperationException("Button was not pressed by this helper");
            Move(command, window);
            Send(action == "down" ? (button == 1 ? 2u : 8u) : (button == 1 ? 4u : 16u), 0, 0);
            if (action == "down") { held |= button; heldWindow = window; } else { held &= ~button; if (held == 0) heldWindow = IntPtr.Zero; }
            return true;
        }
        throw new InvalidOperationException("Unknown native cursor command " + action);
    }
    private static void Main() {
        SetProcessDpiAwarenessContext(new IntPtr(-4));
        Console.InputEncoding = new UTF8Encoding(false); Console.OutputEncoding = new UTF8Encoding(false);
        Console.WriteLine("NATIVE_READY"); Console.Out.Flush();
        try {
            string line;
            while ((line = Console.ReadLine()) != null) {
                var command = Json.Deserialize<Dictionary<string, object>>(line);
                object result = null; string error = null;
                try { result = Execute(command); } catch (Exception failure) { error = failure.ToString(); }
                bool quit = Convert.ToString(command["action"]) == "quit";
                Console.WriteLine("NATIVE_RESULT " + Json.Serialize(new { id = command["id"], result, error })); Console.Out.Flush();
                if (quit) break;
            }
        } finally {
            Release();
            if (moved) SetCursorPos(original.X, original.Y);
            if (module != IntPtr.Zero) FreeLibrary(module);
        }
    }
}
