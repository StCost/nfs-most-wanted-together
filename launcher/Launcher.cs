// NFS MW Together launcher: edits scripts\MWTogether.ini (comments kept) and starts speed.exe.
// Lives next to speed.exe. Build: launcher\build.bat (csc from .NET Framework 4, no extra installs).
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

static class Ini {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern int GetPrivateProfileString(string s, string k, string d, StringBuilder r, int n, string f);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern bool WritePrivateProfileString(string s, string k, string v, string f);
    public static string Path;
    public static string Get(string s, string k, string d) { var sb = new StringBuilder(512); GetPrivateProfileString(s, k, d, sb, 512, Path); return sb.ToString(); }
    public static bool GetB(string s, string k, bool d) { return Get(s, k, d ? "1" : "0").Trim() != "0"; }
    public static void Set(string s, string k, string v) { WritePrivateProfileString(s, k, v, Path); }
    public static void SetB(string s, string k, bool v) { Set(s, k, v ? "1" : "0"); }
}

class LauncherForm : Form {
    readonly string gameDir;
    TextBox name = new TextBox(), hostIp = new TextBox(), clientIp = new TextBox(), port = new TextBox();
    RadioButton host = new RadioButton { Text = "Host (others join me)" }, join = new RadioButton { Text = "Join a host" };
    CheckBox skipIntro = new CheckBox { Text = "Skip intro screens" }, direct = new CheckBox { Text = "Go straight into career free roam" },
             noSafeHouse = new CheckBox { Text = "No \"leave the safe house?\" question" }, pursuitRes = new CheckBox { Text = "Close pursuit results by itself (free roam)" },
             introInput = new CheckBox { Text = "Menus react during their animations" }, overlay = new CheckBox { Text = "Show status overlay (F8)" };
    ComboBox menuSpeed = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList };
    Label status = new Label { AutoSize = false, ForeColor = Color.DimGray };

    public LauncherForm(string dir) {
        gameDir = dir;
        Text = "NFS MW Together";
        FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false;
        Font = new Font("Segoe UI", 9f);
        ClientSize = new Size(440, 470);
        StartPosition = FormStartPosition.CenterScreen;
        try { Icon = Icon.ExtractAssociatedIcon(Path.Combine(dir, "speed.exe")); } catch { }

        int y = 14;
        Controls.Add(new Label { Text = "Your name", Location = new Point(16, y + 3), AutoSize = true });
        name.SetBounds(130, y, 290, 24); Controls.Add(name); y += 34;
        host.SetBounds(16, y, 200, 24); join.SetBounds(220, y, 200, 24); Controls.Add(host); Controls.Add(join); y += 30;
        Controls.Add(new Label { Text = "Host's IP", Location = new Point(16, y + 3), AutoSize = true });
        hostIp.SetBounds(130, y, 290, 24); Controls.Add(hostIp); y += 30;
        Controls.Add(new Label { Text = "Friend's IP (optional)", Location = new Point(16, y + 3), AutoSize = true });
        clientIp.SetBounds(150, y, 270, 24); Controls.Add(clientIp); y += 30;
        Controls.Add(new Label { Text = "UDP port", Location = new Point(16, y + 3), AutoSize = true });
        port.SetBounds(130, y, 80, 24); Controls.Add(port); y += 30;
        var ips = new Label { Text = "My IPs: " + MyIps(), Location = new Point(16, y), Size = new Size(410, 34), ForeColor = Color.DimGray };
        Controls.Add(ips); y += 40;

        foreach (var c in new Control[] { skipIntro, direct, noSafeHouse, pursuitRes, introInput, overlay }) { c.SetBounds(16, y, 410, 22); Controls.Add(c); y += 24; }
        Controls.Add(new Label { Text = "Menu animation speed", Location = new Point(16, y + 3), AutoSize = true });
        menuSpeed.Items.AddRange(new object[] { "1x (normal)", "2x", "3x" });
        menuSpeed.SetBounds(170, y, 120, 24); Controls.Add(menuSpeed); y += 36;

        var play = new Button { Text = "Play", Font = new Font("Segoe UI", 11f, FontStyle.Bold) };
        play.SetBounds(16, y, 200, 40); play.Click += (s, e) => Play(); Controls.Add(play);
        var log = new Button { Text = "Open log" };
        log.SetBounds(230, y, 90, 40); log.Click += (s, e) => OpenFile(Path.Combine(gameDir, "scripts", "MWTogether.log")); Controls.Add(log);
        var ini = new Button { Text = "All settings" };
        ini.SetBounds(330, y, 90, 40); ini.Click += (s, e) => OpenFile(Ini.Path); Controls.Add(ini);
        y += 48;
        status.SetBounds(16, y, 410, 20); Controls.Add(status);
        AcceptButton = play;

        host.CheckedChanged += (s, e) => { hostIp.Enabled = join.Checked; clientIp.Enabled = host.Checked; };
        LoadSettings();
    }

    static string MyIps() {
        try {
            var list = NetworkInterface.GetAllNetworkInterfaces()
                .Where(n => n.OperationalStatus == OperationalStatus.Up && n.NetworkInterfaceType != NetworkInterfaceType.Loopback)
                .SelectMany(n => n.GetIPProperties().UnicastAddresses)
                .Where(a => a.Address.AddressFamily == AddressFamily.InterNetwork)
                .Select(a => a.Address.ToString()).Distinct().ToArray();
            return list.Length > 0 ? string.Join(", ", list) : "?";
        } catch { return "?"; }
    }

    void OpenFile(string p) { try { if (File.Exists(p)) Process.Start("notepad.exe", "\"" + p + "\""); else status.Text = "Not found yet: " + p; } catch (Exception ex) { status.Text = ex.Message; } }

    void LoadSettings() {
        name.Text = Ini.Get("Player", "Name", Environment.UserName);
        bool isHost = !Ini.Get("Network", "Mode", "host").Trim().Equals("client", StringComparison.OrdinalIgnoreCase);
        host.Checked = isHost; join.Checked = !isHost;
        hostIp.Text = Ini.Get("Network", "HostIP", "");
        clientIp.Text = Ini.Get("Network", "ClientIP", "");
        port.Text = Ini.Get("Network", "Port", "47555");
        skipIntro.Checked = Ini.GetB("Start", "SkipIntro", true);
        direct.Checked = Ini.GetB("Start", "DirectFreeRoam", true);
        noSafeHouse.Checked = Ini.GetB("Start", "SkipLeaveSafeHouseDialog", true);
        pursuitRes.Checked = Ini.GetB("UI", "SkipPursuitResults", false);
        introInput.Checked = Ini.GetB("UI", "IntroInput", true);
        overlay.Checked = Ini.GetB("Debug", "Overlay", false);
        int ms; if (!int.TryParse(Ini.Get("UI", "MenuSpeed", "2"), out ms)) ms = 2;
        menuSpeed.SelectedIndex = Math.Max(0, Math.Min(2, ms - 1));
        hostIp.Enabled = join.Checked; clientIp.Enabled = host.Checked;
    }

    bool Save() {
        int p;
        if (!int.TryParse(port.Text.Trim(), out p) || p < 1 || p > 65535) { status.Text = "The port must be a number from 1 to 65535."; return false; }
        if (join.Checked && hostIp.Text.Trim().Length == 0) { status.Text = "Enter the host's IP address to join."; return false; }
        if (name.Text.Trim().Length == 0) { status.Text = "Enter your name."; return false; }
        Ini.Set("Player", "Name", name.Text.Trim().Length > 15 ? name.Text.Trim().Substring(0, 15) : name.Text.Trim());
        Ini.Set("Network", "Mode", host.Checked ? "host" : "client");
        Ini.Set("Network", "HostIP", hostIp.Text.Trim());
        Ini.Set("Network", "ClientIP", clientIp.Text.Trim());
        Ini.Set("Network", "Port", p.ToString());
        Ini.SetB("Start", "SkipIntro", skipIntro.Checked);
        Ini.SetB("Start", "DirectFreeRoam", direct.Checked);
        Ini.SetB("Start", "SkipLeaveSafeHouseDialog", noSafeHouse.Checked);
        Ini.SetB("UI", "SkipPursuitResults", pursuitRes.Checked);
        Ini.SetB("UI", "IntroInput", introInput.Checked);
        Ini.Set("UI", "MenuSpeed", (menuSpeed.SelectedIndex + 1).ToString());
        Ini.SetB("Debug", "Overlay", overlay.Checked);
        return true;
    }

    void Play() {
        if (!Save()) return;
        string exe = Path.Combine(gameDir, "speed.exe");
        if (Process.GetProcessesByName("speed").Length > 0) { status.Text = "The game is already running."; return; }
        try {
            Process.Start(new ProcessStartInfo(exe) { WorkingDirectory = gameDir, UseShellExecute = true });
            Close();
        } catch (Exception ex) { status.Text = "Couldn't start the game: " + ex.Message; }
    }
}

static class Program {
    [STAThread]
    static void Main() {
        string dir = AppDomain.CurrentDomain.BaseDirectory;
        Application.EnableVisualStyles();
        if (!File.Exists(Path.Combine(dir, "speed.exe"))) {
            MessageBox.Show("Put this launcher next to speed.exe (the NFS Most Wanted folder).", "NFS MW Together");
            return;
        }
        string scripts = Path.Combine(dir, "scripts");
        Ini.Path = Path.Combine(scripts, "MWTogether.ini");
        if (!File.Exists(Ini.Path)) {
            string old = Path.Combine(scripts, "CoopMod.ini");
            if (File.Exists(old)) File.Copy(old, Ini.Path);
            else if (!File.Exists(Path.Combine(scripts, "MWTogether.asi"))) {
                MessageBox.Show("NFS MW Together isn't installed: scripts\\MWTogether.asi is missing.", "NFS MW Together");
                return;
            }
        }
        Application.Run(new LauncherForm(dir));
    }
}
