// Native common-file-dialog probe. Reuses windows-native-input-driver.js transport.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using System.Web.Script.Serialization;

internal static class OwnedDialogHelper {
    private delegate bool EnumWindow(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindow callback, IntPtr data);
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr parent, EnumWindow callback, IntPtr data);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr window, StringBuilder text, int size);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(IntPtr window, StringBuilder text, int size);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] private static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")] private static extern IntPtr GetWindow(IntPtr window, uint command);
    [DllImport("user32.dll")] private static extern IntPtr GetParent(IntPtr window);
    [DllImport("user32.dll")] private static extern int GetDlgCtrlID(IntPtr window);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr wParam, string lParam, uint flags, uint timeout, out IntPtr result);
    private static string Text(IntPtr window, bool cls) {
        var text = new StringBuilder(1024);
        if (cls) GetClassName(window, text, text.Capacity); else GetWindowText(window, text, text.Capacity);
        return text.ToString();
    }
    private static List<object> Dialogs(int pid) {
        var result = new List<object>();
        EnumWindows((window, data) => {
            uint process; GetWindowThreadProcessId(window, out process);
            if (process == pid && IsWindowVisible(window) && Text(window, true) == "#32770") {
                IntPtr owner = GetWindow(window, 4);
                result.Add(new { hwnd = window.ToInt64().ToString(), title = Text(window, false),
                    owner = owner.ToInt64().ToString(), ownerTitle = Text(owner, false), ownerEnabled = IsWindowEnabled(owner) });
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }
    private static object Execute(Dictionary<string, object> command) {
        string action = Convert.ToString(command["action"]);
        if (action == "dialogs") return Dialogs(Convert.ToInt32(command["pid"]));
        if (action == "cancel" || action == "accept") {
            IntPtr window = new IntPtr(Int64.Parse(Convert.ToString(command["hwnd"])));
            uint pid; GetWindowThreadProcessId(window, out pid);
            if (pid != Convert.ToUInt32(command["pid"]) || Text(window, true) != "#32770"
                || Text(window, false) != Convert.ToString(command["title"]))
                throw new InvalidOperationException("Refusing to control a non-fixture dialog");
            if (action == "accept") {
                IntPtr edit = IntPtr.Zero;
                EnumChildWindows(window, (child, data) => {
                    if (Text(child, true) == "Edit") {
                        // Modern save pickers use DirectUI's filename combo edit
                        // (1001); the classic open picker uses cmb13 (1148).
                        bool filename = GetDlgCtrlID(child) == 1001
                            && Text(GetParent(child), true) == "ComboBox";
                        for (IntPtr parent = child; !filename && parent != IntPtr.Zero && parent != window; parent = GetParent(parent))
                            filename = GetDlgCtrlID(parent) == 1148;
                        if (filename) {
                            if (edit != IntPtr.Zero) throw new InvalidOperationException("Native filename edit is ambiguous");
                            edit = child;
                        }
                    }
                    return true;
                }, IntPtr.Zero);
                if (edit == IntPtr.Zero) throw new InvalidOperationException("Native filename edit was not found");
                IntPtr result;
                if (SendMessageTimeout(edit, 0x000C, IntPtr.Zero, Convert.ToString(command["path"]), 2, 3000, out result) == IntPtr.Zero)
                    throw new Win32Exception();
                if (!PostMessage(window, 0x0111, new IntPtr(1), IntPtr.Zero)) throw new Win32Exception();
            } else if (!PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero)) throw new Win32Exception();
            return true;
        }
        throw new InvalidOperationException("Unknown dialog command " + action);
    }
    private static void Main() {
        Console.InputEncoding = new UTF8Encoding(false); Console.OutputEncoding = new UTF8Encoding(false);
        Console.WriteLine("NATIVE_READY"); Console.Out.Flush();
        string line;
        while ((line = Console.ReadLine()) != null) {
            Dictionary<string, object> command = null;
            try {
                command = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(line);
                bool quit = Convert.ToString(command["action"]) == "quit";
                object result = quit ? (object)true : Execute(command);
                Console.WriteLine("NATIVE_RESULT " + new JavaScriptSerializer().Serialize(new { id = command["id"], result = result }));
                Console.Out.Flush();
                if (quit) return;
            } catch (Exception error) {
                if (command == null) Console.Error.WriteLine(error);
                else Console.WriteLine("NATIVE_RESULT " + new JavaScriptSerializer().Serialize(new { id = command["id"], error = error.ToString() }));
                Console.Out.Flush();
            }
        }
    }
}
