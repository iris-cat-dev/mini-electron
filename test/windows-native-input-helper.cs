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
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", SetLastError = true)] private static extern IntPtr SendMessageTimeout(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam, uint flags, uint timeout, out UIntPtr result);
    [DllImport("user32.dll")] private static extern bool ClientToScreen(IntPtr window, ref Point point);
    [DllImport("user32.dll")] private static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")] private static extern IntPtr WindowFromPoint(Point point);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] private static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] private static extern bool IsWindow(IntPtr window);
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
            || !ClientToScreen(window, ref origin))
            throw new InvalidOperationException("Refusing pointer input for an unexpected foreground window");
        if (x < origin.X || x >= origin.X + client.Right - client.Left
            || y < origin.Y || y >= origin.Y + client.Bottom - client.Top)
            throw new InvalidOperationException("Pointer target is outside the native test window client");
    }
    private static void MovePointer(IntPtr window, int x, int y) {
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
        Thread.Sleep(50);
    }
    private static void ReleaseHeldPointer() {
        if (!heldLeftButton) return;
        try { Mouse(true); }
        finally { heldLeftButton = false; heldPointerWindow = IntPtr.Zero; }
    }
    private static IntPtr Window(Dictionary<string, object> command) {
        IntPtr window = command.ContainsKey("hwnd") ? new IntPtr(Convert.ToInt64(command["hwnd"])) : FindWindow(null, Convert.ToString(command["title"]));
        if (window == IntPtr.Zero || !IsWindow(window)) throw new InvalidOperationException("Native test window not found");
        return window;
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
        if (action == "wheel") {
            Focus(Window(command));
            if (!SetCursorPos(Number(command, "x"), Number(command, "y"))) throw new Win32Exception();
            Send(new Input { Type = 0, Data = new InputUnion { Mouse = new MouseInput {
                Data = unchecked((uint)Number(command, "delta")), Flags = 0x800
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
        if (action == "quit") { ReleaseHeldPointer(); RestoreClipboard(); form.Close(); return true; }
        throw new InvalidOperationException("Unknown native command " + action);
    }
    private static void BeginDrag(Dictionary<string, object> command) {
        if (drag != null) throw new InvalidOperationException("A native drag is already active");
        drag = command; accepted = 0;
        Point origin = new Point { X = source.Width / 2, Y = source.Height / 2 };
        ClientToScreen(source.Handle, ref origin);
        Point destination = new Point { X = Number(command, "x"), Y = Number(command, "y") };
        if (Flag(command, "control")) {
            destination = new Point { X = control.Width / 2, Y = control.Height / 2 };
            ClientToScreen(control.Handle, ref destination);
        }
        form.Activate();
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
