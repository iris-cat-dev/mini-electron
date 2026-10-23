using System;
using System.Runtime.InteropServices;

internal static class NotificationHelper {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct NotifyIconData {
        public uint Size;
        public IntPtr Window;
        public uint Id, Flags, CallbackMessage;
        public IntPtr Icon;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Tip;
        public uint State, StateMask;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string Info;
        public uint Version;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string InfoTitle;
        public uint InfoFlags;
        public Guid Guid;
        public IntPtr BalloonIcon;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct IconIdentifier {
        public uint Size;
        public IntPtr Window;
        public uint Id;
        public Guid Guid;
    }
    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string name, string title);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool PostMessage(IntPtr window, uint message, UIntPtr wparam, IntPtr lparam);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern bool Shell_NotifyIconW(uint operation, ref NotifyIconData data);
    [DllImport("shell32.dll")] private static extern int Shell_NotifyIconGetRect(ref IconIdentifier id, out Rect rect);

    private static IntPtr FindOwnedWindow(uint process, bool tray) {
        IntPtr window = IntPtr.Zero;
        while ((window = FindWindowEx(tray ? IntPtr.Zero : new IntPtr(-3), window,
                tray ? "HideParentWindowClass" : "MiniElectronNativeNotificationWindow", null)) != IntPtr.Zero) {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner == process) return window;
        }
        throw new InvalidOperationException("No notification host belonging to the test PID");
    }

    private static int Main(string[] args) {
        try {
            uint process = uint.Parse(args[1]);
            IntPtr window = FindOwnedWindow(process, args[0] == "tray");
            if (args[0] == "post") {
                uint id = uint.Parse(args[2]);
                uint notification = uint.Parse(args[3]);
                if (id == 0 || id > 65535 || notification > 65535) throw new ArgumentOutOfRangeException("id/event");
                // wParam is deliberately independent of the VERSION_4 icon ID.
                if (!PostMessage(window, 0x83a1, new UIntPtr(uint.Parse(args[4])),
                        new IntPtr(unchecked((int)((id << 16) | notification)))))
                    throw new System.ComponentModel.Win32Exception();
                Console.WriteLine("POSTED");
                return 0;
            }
            if (args[0] != "inspect" && args[0] != "tray") throw new ArgumentException("Unknown operation");
            string[] ids = args[2].Split(',');
            Console.Write("[");
            for (int index = 0; index < ids.Length; index++) {
                uint id = uint.Parse(ids[index]);
                var data = new NotifyIconData {
                    Size = (uint)Marshal.SizeOf(typeof(NotifyIconData)), Window = window, Id = id
                };
                // A zero-flag NIM_MODIFY changes no fields. Unlike GetRect it also
                // detects icons intentionally hidden with NIS_HIDDEN.
                bool registered = Shell_NotifyIconW(1, ref data);
                var identifier = new IconIdentifier {
                    Size = (uint)Marshal.SizeOf(typeof(IconIdentifier)), Window = window, Id = id
                };
                Rect rect;
                bool positioned = Shell_NotifyIconGetRect(ref identifier, out rect) == 0;
                if (index != 0) Console.Write(",");
                Console.Write("{\"id\":" + id + ",\"registered\":" + (registered ? "true" : "false")
                    + ",\"positioned\":" + (positioned ? "true" : "false") + "}");
            }
            Console.WriteLine("]");
            return 0;
        } catch (Exception error) {
            Console.Error.WriteLine(error);
            return 1;
        }
    }
}
