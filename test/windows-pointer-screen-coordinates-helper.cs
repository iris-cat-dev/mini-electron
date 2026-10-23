using System;
using System.ComponentModel;
using System.Globalization;
using System.Runtime.InteropServices;

internal static class PointerScreenCoordinatesHelper
{
    [StructLayout(LayoutKind.Sequential)] private struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)] private struct MonitorInfo {
        public int Size;
        public Rect Monitor;
        public Rect Work;
        public uint Flags;
    }

    [DllImport("user32.dll")] private static extern bool SetProcessDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll")] private static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool ClientToScreen(IntPtr window, ref Point point);
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")] private static extern IntPtr MonitorFromWindow(IntPtr window, uint flags);
    [DllImport("user32.dll", CharSet = CharSet.Auto, SetLastError = true)] private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", SetLastError = true)] private static extern bool PostMessage(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam);

    private static string RectJson(Rect rect)
    {
        return "{\"left\":" + rect.Left + ",\"top\":" + rect.Top + ",\"right\":" + rect.Right
            + ",\"bottom\":" + rect.Bottom + ",\"width\":" + (rect.Right - rect.Left)
            + ",\"height\":" + (rect.Bottom - rect.Top) + "}";
    }

    private static IntPtr Pack(int x, int y)
    {
        long value = (long)(ushort)x | ((long)(ushort)y << 16);
        return new IntPtr(value);
    }

    private static int Main(string[] args)
    {
        try {
            // A fresh helper process can opt into physical per-monitor coordinates without
            // changing the runtime process or any user setting.
            SetProcessDpiAwarenessContext(new IntPtr(-4));
            if (args.Length < 3) throw new ArgumentException("Usage: helper <action> <pid> <hwnd> [clientX clientY [delta]]");

            string action = args[0];
            uint expectedProcess = uint.Parse(args[1], CultureInfo.InvariantCulture);
            IntPtr window = new IntPtr(long.Parse(args[2], CultureInfo.InvariantCulture));
            uint actualProcess;
            if (!IsWindow(window) || GetWindowThreadProcessId(window, out actualProcess) == 0)
                throw new InvalidOperationException("Target HWND is not a live window");
            if (actualProcess != expectedProcess)
                throw new InvalidOperationException("Target HWND is not owned by the expected PID");

            Rect windowRect, clientRect;
            Point clientOrigin = new Point();
            if (!GetWindowRect(window, out windowRect) || !GetClientRect(window, out clientRect)
                || !ClientToScreen(window, ref clientOrigin)) throw new Win32Exception();
            uint dpi = GetDpiForWindow(window);
            if (dpi == 0) throw new Win32Exception();
            IntPtr monitor = MonitorFromWindow(window, 2);
            var monitorInfo = new MonitorInfo { Size = Marshal.SizeOf(typeof(MonitorInfo)) };
            if (monitor == IntPtr.Zero || !GetMonitorInfo(monitor, ref monitorInfo)) throw new Win32Exception();

            int clientX = 0, clientY = 0;
            Point screenPoint = clientOrigin;
            IntPtr foregroundBefore = GetForegroundWindow();
            uint message = 0;
            UIntPtr keys = UIntPtr.Zero;
            if (action != "geometry") {
                if (args.Length < 5) throw new ArgumentException("Message action requires client coordinates");
                clientX = int.Parse(args[3], CultureInfo.InvariantCulture);
                clientY = int.Parse(args[4], CultureInfo.InvariantCulture);
                screenPoint = new Point { X = clientX, Y = clientY };
                if (!ClientToScreen(window, ref screenPoint)) throw new Win32Exception();
                if (clientX < 0 || clientY < 0 || clientX >= clientRect.Right - clientRect.Left
                    || clientY >= clientRect.Bottom - clientRect.Top || clientX > short.MaxValue || clientY > short.MaxValue)
                    throw new InvalidOperationException("Refusing a message outside the owned HWND client area");

                IntPtr lParam;
                if (action == "move") { message = 0x0200; lParam = Pack(clientX, clientY); }
                else if (action == "down") { message = 0x0201; keys = new UIntPtr(1); lParam = Pack(clientX, clientY); }
                else if (action == "heldMove") { message = 0x0200; keys = new UIntPtr(1); lParam = Pack(clientX, clientY); }
                else if (action == "up") { message = 0x0202; lParam = Pack(clientX, clientY); }
                else if (action == "wheel") {
                    if (args.Length < 6) throw new ArgumentException("Wheel action requires a delta");
                    int delta = int.Parse(args[5], CultureInfo.InvariantCulture);
                    if (delta < short.MinValue || delta > short.MaxValue || screenPoint.X < short.MinValue
                        || screenPoint.X > short.MaxValue || screenPoint.Y < short.MinValue || screenPoint.Y > short.MaxValue)
                        throw new InvalidOperationException("Wheel data cannot be represented by a native WM_MOUSEWHEEL");
                    message = 0x020a;
                    keys = new UIntPtr((uint)(ushort)delta << 16);
                    lParam = Pack(screenPoint.X, screenPoint.Y);
                } else throw new ArgumentException("Unknown action: " + action);
                if (!PostMessage(window, message, keys, lParam)) throw new Win32Exception();
            }

            IntPtr foregroundAfter = GetForegroundWindow();
            Console.WriteLine("{\"action\":\"" + action + "\",\"hwnd\":\"" + window.ToInt64()
                + "\",\"pid\":" + actualProcess + ",\"dpi\":" + dpi
                + ",\"window\":" + RectJson(windowRect) + ",\"client\":{\"origin\":{\"x\":" + clientOrigin.X
                + ",\"y\":" + clientOrigin.Y + "},\"width\":" + (clientRect.Right - clientRect.Left)
                + ",\"height\":" + (clientRect.Bottom - clientRect.Top) + "},\"point\":{\"clientX\":" + clientX
                + ",\"clientY\":" + clientY + ",\"screenX\":" + screenPoint.X + ",\"screenY\":" + screenPoint.Y
                + "},\"monitor\":{\"bounds\":" + RectJson(monitorInfo.Monitor) + ",\"workArea\":"
                + RectJson(monitorInfo.Work) + "},\"foregroundBefore\":\"" + foregroundBefore.ToInt64()
                + "\",\"foregroundAfter\":\"" + foregroundAfter.ToInt64() + "\"}");
            return 0;
        } catch (Exception error) {
            Console.Error.WriteLine(error);
            return 1;
        }
    }
}
