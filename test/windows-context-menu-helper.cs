using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

internal static class ContextMenuHelper {
    [StructLayout(LayoutKind.Sequential)] private struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct MouseInput { public int X, Y; public uint Data, Flags, Time; public UIntPtr Extra; }
    [StructLayout(LayoutKind.Explicit)] private struct InputUnion { [FieldOffset(0)] public MouseInput Mouse; }
    [StructLayout(LayoutKind.Sequential)] private struct Input { public uint Type; public InputUnion Data; }
    [DllImport("user32.dll")] private static extern bool SetProcessDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] private static extern IntPtr WindowFromPoint(Point point);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string name, string title);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] private static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")] private static extern int GetMenuItemCount(IntPtr menu);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetMenuString(IntPtr menu, uint item, StringBuilder text, int size, uint flags);
    [DllImport("user32.dll")] private static extern bool GetMenuItemRect(IntPtr window, IntPtr menu, uint item, out Rect rect);
    [DllImport("user32.dll")] private static extern uint GetMenuState(IntPtr menu, uint item, uint flags);
    [DllImport("user32.dll")] private static extern short GetAsyncKeyState(int key);
    [DllImport("user32.dll", SetLastError = true)] private static extern uint SendInput(uint count, Input[] inputs, int size);

    private static void Click(int x, int y, bool right) {
        int key = right ? 2 : 1;
        if ((GetAsyncKeyState(key) & 0x8000) != 0) throw new InvalidOperationException("Refusing to click an already-held mouse button");
        if (!SetCursorPos(x, y)) throw new Win32Exception();
        var inputs = new Input[2];
        inputs[0].Data.Mouse.Flags = right ? 0x8u : 0x2u;
        inputs[1].Data.Mouse.Flags = right ? 0x10u : 0x4u;
        if (SendInput(2, inputs, Marshal.SizeOf(typeof(Input))) != 2) throw new Win32Exception();
    }

    [STAThread] private static int Main(string[] args) {
        uint expectedProcess = 0;
        try {
            SetProcessDpiAwarenessContext(new IntPtr(-4));
            expectedProcess = uint.Parse(args[1]);
            if (args[0] == "activate-window") {
                var window = new IntPtr(unchecked((long)ulong.Parse(args[2])));
                uint actualProcess;
                GetWindowThreadProcessId(window, out actualProcess);
                if (actualProcess != expectedProcess) throw new InvalidOperationException("Activation target is not an owned test process");
                Rect rect;
                if (!GetWindowRect(window, out rect)) throw new Win32Exception();
                var point = new Point { X = (rect.Left + rect.Right) / 2, Y = rect.Bottom - Math.Min(40, (rect.Bottom - rect.Top) / 4) };
                GetWindowThreadProcessId(WindowFromPoint(point), out actualProcess);
                if (actualProcess != expectedProcess) throw new InvalidOperationException("Owned test window is obscured; refusing to click another application");
                Click(point.X, point.Y, false);
                Console.WriteLine("ACTIVATED");
                return 0;
            }
            if (args[0] == "right-click") {
                var point = new Point { X = int.Parse(args[2]), Y = int.Parse(args[3]) };
                uint actualProcess;
                GetWindowThreadProcessId(WindowFromPoint(point), out actualProcess);
                if (actualProcess != expectedProcess) throw new InvalidOperationException("Pointer target is not the owned test runtime");
                Click(point.X, point.Y, true);
                Console.WriteLine("RIGHT_CLICKED");
                return 0;
            }
            if (args[0] != "select") throw new ArgumentException("Unknown operation");
            string label = args[2];
            Console.WriteLine("READY");
            Console.Out.Flush();
            var text = new StringBuilder(512);
            var deadline = DateTime.UtcNow.AddSeconds(10);
            while (DateTime.UtcNow < deadline) {
                IntPtr popup = IntPtr.Zero;
                while ((popup = FindWindowEx(IntPtr.Zero, popup, "#32768", null)) != IntPtr.Zero) {
                    uint actualProcess;
                    GetWindowThreadProcessId(popup, out actualProcess);
                    if (actualProcess != expectedProcess) continue;
                    IntPtr menu = SendMessage(popup, 0x01e1, IntPtr.Zero, IntPtr.Zero);
                    int count = GetMenuItemCount(menu);
                    for (uint index = 0; index < count; index++) {
                        text.Clear();
                        GetMenuString(menu, index, text, text.Capacity, 0x400);
                        if (!string.Equals(text.ToString(), label, StringComparison.Ordinal)) continue;
                        if ((GetMenuState(menu, index, 0x400) & 3) != 0) throw new InvalidOperationException("Requested native menu item is disabled");
                        Rect rect;
                        if (!GetMenuItemRect(IntPtr.Zero, menu, index, out rect)) throw new Win32Exception();
                        Click((rect.Left + rect.Right) / 2, (rect.Top + rect.Bottom) / 2, false);
                        Console.WriteLine("SELECTED " + label);
                        return 0;
                    }
                }
                Thread.Sleep(15);
            }
            throw new TimeoutException("Owned native popup item did not appear: " + label);
        } catch (Exception error) {
            Console.Error.WriteLine(error);
            if (expectedProcess != 0) {
                IntPtr popup = IntPtr.Zero;
                while ((popup = FindWindowEx(IntPtr.Zero, popup, "#32768", null)) != IntPtr.Zero) {
                    uint actualProcess;
                    GetWindowThreadProcessId(popup, out actualProcess);
                    if (actualProcess == expectedProcess)
                        PostMessage(popup, 0x0100, new IntPtr(0x1b), IntPtr.Zero);
                }
            }
            return 1;
        }
    }
}
