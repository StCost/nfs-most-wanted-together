// NFS MW Together launcher: edits scripts\MWTogether.ini (comments kept) and starts speed.exe.
// Lives next to speed.exe. Build (csc from .NET Framework 4, nothing to install):
//   csc /target:winexe /out:MWTogetherLauncher.exe /r:System.Windows.Forms.dll /r:System.Drawing.dll
//       /resource:docs\banner.png,banner.png launcher\Launcher.cs
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
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

// Most Wanted look (taken from the game's main menu): lowercase heavy serif headings, bold white
// labels, dark panels with diagonal hatch stripes, amber corner brackets on the selected item, amber
// chevrons.
class GameButton : Button {
    public bool Selected, Plain, Chevrons;
    bool hover;
    public GameButton() { SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true); }
    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnPaint(PaintEventArgs e) {
        var g = e.Graphics; var r = ClientRectangle;
        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
        g.Clear(Parent != null ? Parent.BackColor : Theme.Back);
        if (!Plain) {
            Theme.Stripes(g, r, hover ? Color.FromArgb(86, 80, 60) : Selected ? Color.FromArgb(64, 62, 56) : Color.FromArgb(40, 40, 38));
            if (hover && !Selected) using (var hp = new Pen(Color.FromArgb(150, Theme.Accent), 1)) g.DrawRectangle(hp, 0, 0, r.Width - 1, r.Height - 1);
            if (Selected) {   // amber corner brackets, like the game's selection marker
                using (var p = new Pen(Theme.Accent, 3)) {
                    int L = Math.Min(14, r.Height / 2), x0 = 1, y0 = 1, x1 = r.Width - 2, y1 = r.Height - 2;
                    g.DrawLines(p, new[] { new Point(x0, y0 + L), new Point(x0, y0), new Point(x0 + L, y0) });
                    g.DrawLines(p, new[] { new Point(x1 - L, y0), new Point(x1, y0), new Point(x1, y0 + L) });
                    g.DrawLines(p, new[] { new Point(x0, y1 - L), new Point(x0, y1), new Point(x0 + L, y1) });
                    g.DrawLines(p, new[] { new Point(x1 - L, y1), new Point(x1, y1), new Point(x1, y1 - L) });
                }
            }
        }
        Color fg = Plain ? (hover ? Theme.Text : Theme.Dim) : (Selected || hover ? Color.White : Theme.Dim);
        var flags = (Plain ? TextFormatFlags.Left : TextFormatFlags.HorizontalCenter) | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix;
        string txt = Text + (Chevrons ? "" : "");
        var tr = r; if (Chevrons) tr.Width -= 30;
        TextRenderer.DrawText(g, txt, Font, tr, fg, flags);
        if (Chevrons) TextRenderer.DrawText(g, "»", new Font("Arial Black", Font.Size + 2), new Rectangle(r.Right - 44, 0, 36, r.Height), Theme.Accent, TextFormatFlags.VerticalCenter | TextFormatFlags.HorizontalCenter);
    }
}

// Check box in the same style: a striped box, amber when ticked, amber outline under the mouse.
class GameCheck : CheckBox {
    bool hover;
    public GameCheck() { SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true); Cursor = Cursors.Hand; }
    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnPaint(PaintEventArgs e) {
        var g = e.Graphics;
        g.Clear(Parent != null ? Parent.BackColor : Theme.Back);
        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
        int s = 18, y = (Height - s) / 2;
        var box = new Rectangle(1, y, s, s);
        Theme.Stripes(g, box, Color.FromArgb(52, 52, 48));
        using (var p = new Pen(hover ? Theme.Accent : Color.FromArgb(110, 112, 110), hover ? 2 : 1)) g.DrawRectangle(p, box);
        if (Checked) using (var p = new Pen(Theme.Accent, 3)) g.DrawLines(p, new[] { new Point(box.X + 4, y + 9), new Point(box.X + 8, y + 13), new Point(box.X + 15, y + 4) });
        TextRenderer.DrawText(g, Text, Font, new Rectangle(s + 10, 0, Width - s - 10, Height), hover || Checked ? Color.White : Theme.Dim, TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.NoPrefix);
    }
}

static class Theme {
    // The game's dark hazard-stripe panels: wide diagonal bands on near-black.
    public static void Stripes(Graphics g, Rectangle r, Color band) {
        var old = g.Clip; g.SetClip(r);
        using (var b = new SolidBrush(Color.FromArgb(18, 18, 17))) g.FillRectangle(b, r);
        using (var b = new SolidBrush(band))
            for (int x = r.Left - r.Height; x < r.Right + 12; x += 16)
                g.FillPolygon(b, new[] { new Point(x, r.Bottom), new Point(x + 8, r.Bottom), new Point(x + 8 + r.Height, r.Top), new Point(x + r.Height, r.Top) });
        g.Clip = old;
    }
    public static readonly Color Back = Color.FromArgb(14, 16, 18), Panel = Color.FromArgb(28, 32, 36), Field = Color.FromArgb(40, 45, 50);
    public static readonly Color Text = Color.FromArgb(225, 228, 230), Dim = Color.FromArgb(140, 148, 155), Accent = Color.FromArgb(242, 169, 0);
    public static readonly Font Body = new Font("Segoe UI", 10f), Small = new Font("Segoe UI", 8.5f),
                                Head = new Font("Georgia", 12f, FontStyle.Bold), Big = new Font("Georgia", 22f, FontStyle.Bold),
                                Label = new Font("Arial", 10f, FontStyle.Bold), Button = new Font("Arial", 11f, FontStyle.Bold);
}

class LauncherForm : Form {
    readonly string gameDir;
    const int W = 480;   // content width
    TextBox name = new TextBox(), hostIp = new TextBox(), port = new TextBox();
    GameButton hostBtn = new GameButton(), joinBtn = new GameButton(), play = new GameButton(), settingsBtn = new GameButton();
    bool isHost = true;
    Panel hostIpRow, myIpRow, settings;
    Label myIps = new Label();
    CheckBox skipIntro = Check("Skip intros"), direct = Check("Quick career start"),
             noSafeHouse = Check("Quick leave the safe house"), pursuitRes = Check("Auto-close pursuit results in free roam"),
             introInput = Check("Menus respond during animations"), overlay = Check("Show status overlay (F8)");
    GameButton[] speedBtns = { new GameButton(), new GameButton(), new GameButton() };
    int menuSpeed = 2;
    Label status = new Label { AutoSize = false, ForeColor = Theme.Accent, Font = Theme.Small };

    static CheckBox Check(string t) {
        return new GameCheck { Text = t, AutoSize = false, Size = new Size(W, 28), Font = Theme.Label, Margin = new Padding(0, 1, 0, 1) };
    }
    static Label Caption(string t) {
        return new Label { Text = t.ToLowerInvariant(), AutoSize = true, ForeColor = Color.White, Font = Theme.Head, Margin = new Padding(0, 12, 0, 4) };
    }
    static void StyleField(TextBox tb) { tb.BorderStyle = BorderStyle.FixedSingle; tb.BackColor = Theme.Field; tb.ForeColor = Theme.Text; tb.Font = Theme.Body; }
    static void StyleButton(GameButton b, string text, bool selected) {
        b.Text = text; b.Cursor = Cursors.Hand; b.Font = Theme.Button; b.Selected = selected; b.Invalidate();
    }
    static Panel Row(int h) { return new Panel { Size = new Size(W, h), Margin = new Padding(0), BackColor = Color.Transparent }; }

    public LauncherForm(string dir) {
        gameDir = dir;
        Text = "NFS MW Together";
        FormBorderStyle = FormBorderStyle.FixedSingle; MaximizeBox = false;
        BackColor = Theme.Back; ForeColor = Theme.Text; Font = Theme.Body;
        StartPosition = FormStartPosition.CenterScreen;
        AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink;
        try { Icon = Icon.ExtractAssociatedIcon(Path.Combine(dir, "speed.exe")); } catch { }

        var root = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoSize = true, Padding = new Padding(0, 0, 0, 16), Margin = new Padding(0) };
        Controls.Add(root);

        // banner + title
        var banner = new PictureBox { Size = new Size(W + 40, 228), SizeMode = PictureBoxSizeMode.Zoom, Margin = new Padding(0), BackColor = Color.Black };
        try { var s = typeof(LauncherForm).Assembly.GetManifestResourceStream("banner.png"); if (s != null) banner.Image = Image.FromStream(s); } catch { }
        banner.SizeMode = PictureBoxSizeMode.StretchImage;
        root.Controls.Add(banner);
        var body = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoSize = true, Padding = new Padding(20, 6, 20, 0), Margin = new Padding(0) };
        root.Controls.Add(body);
        body.Controls.Add(new Label { Text = "Need for Speed", AutoSize = true, ForeColor = Theme.Text, Font = new Font("Georgia", 13f, FontStyle.Bold | FontStyle.Italic), Margin = new Padding(2, 8, 0, 0) });
        body.Controls.Add(new Label { Text = "Most Wanted", AutoSize = true, ForeColor = Color.White, Font = new Font("Georgia", 27f, FontStyle.Bold), Margin = new Padding(0, -4, 0, 0) });
        body.Controls.Add(new Label { Text = "Together", AutoSize = true, ForeColor = Theme.Accent, Font = new Font("Georgia", 19f, FontStyle.Bold | FontStyle.Italic), Margin = new Padding(2, -6, 0, 2) });

        // name
        body.Controls.Add(Caption("Nickname"));
        StyleField(name); name.Size = new Size(W, 26); name.MaxLength = 15; name.Margin = new Padding(0);
        body.Controls.Add(name);

        // host / join
        var modeRow = Row(50);
        StyleButton(hostBtn, "Host LAN", false); hostBtn.SetBounds(0, 14, W / 2 - 4, 36);
        StyleButton(joinBtn, "Join LAN", false); joinBtn.SetBounds(W / 2 + 4, 14, W / 2 - 4, 36);
        hostBtn.Click += (s, e) => SetMode(true); joinBtn.Click += (s, e) => SetMode(false);
        modeRow.Controls.Add(hostBtn); modeRow.Controls.Add(joinBtn);
        body.Controls.Add(modeRow);

        hostIpRow = Row(58);
        var hl = Caption("Host's IP address"); hl.Location = new Point(0, 6); hostIpRow.Controls.Add(hl);
        StyleField(hostIp); hostIp.SetBounds(0, 30, W, 26); hostIpRow.Controls.Add(hostIp);
        body.Controls.Add(hostIpRow);

        myIpRow = Row(46);
        myIps.SetBounds(0, 10, W, 34); myIps.ForeColor = Theme.Text; myIps.Font = Theme.Label;
        myIps.Text = "LAN IPs:  " + MyIps();
        myIpRow.Controls.Add(myIps);
        body.Controls.Add(myIpRow);

        // play
        StyleButton(play, "PLAY", true); play.Chevrons = true; play.Font = new Font("Arial Black", 17f);
        play.Size = new Size(W, 48); play.Margin = new Padding(0, 14, 0, 6);
        play.Click += (s, e) => Play();
        body.Controls.Add(play);
        status.Size = new Size(W, 18); status.Margin = new Padding(0);
        body.Controls.Add(status);

        // settings (collapsed)
        StyleButton(settingsBtn, "SETTINGS  ▸", false); settingsBtn.Plain = true; settingsBtn.Font = Theme.Head;
        settingsBtn.Size = new Size(W, 30); settingsBtn.Margin = new Padding(0, 6, 0, 0);
        settingsBtn.Click += (s, e) => { settings.Visible = !settings.Visible; settingsBtn.Text = settings.Visible ? "SETTINGS  ▾" : "SETTINGS  ▸"; settingsBtn.Invalidate(); };
        body.Controls.Add(settingsBtn);

        var sp = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoSize = true, BackColor = Theme.Panel, Padding = new Padding(12, 4, 12, 12), Margin = new Padding(0), Visible = false };
        settings = sp;
        foreach (var c in new CheckBox[] { skipIntro, direct, noSafeHouse, pursuitRes, introInput, overlay }) { c.Width = W - 24; sp.Controls.Add(c); }
        var msRow = new Panel { Size = new Size(W - 24, 32), Margin = new Padding(0, 4, 0, 0) };
        msRow.Controls.Add(new Label { Text = "Menu animation speed", AutoSize = true, Location = new Point(0, 6), ForeColor = Theme.Text });
        for (int i = 0; i < 3; ++i) {
            int v = i + 1;
            speedBtns[i].SetBounds(200 + i * 62, 0, 58, 28);
            speedBtns[i].Click += (s, e) => SetSpeed(v);
            msRow.Controls.Add(speedBtns[i]);
        }
        sp.Controls.Add(msRow);
        var portRow = new Panel { Size = new Size(W - 24, 32), Margin = new Padding(0, 2, 0, 0) };
        portRow.Controls.Add(new Label { Text = "UDP port", AutoSize = true, Location = new Point(0, 6), ForeColor = Theme.Text });
        StyleField(port); port.SetBounds(200, 2, 120, 26); portRow.Controls.Add(port);
        sp.Controls.Add(portRow);
        var btnRow = new Panel { Size = new Size(W - 24, 36), Margin = new Padding(0, 8, 0, 0) };
        var log = new GameButton(); StyleButton(log, "Open log", false); log.SetBounds(0, 0, (W - 24) / 2 - 4, 32);
        log.Click += (s, e) => OpenFile(Path.Combine(gameDir, "scripts", "MWTogether.log"));
        var ini = new GameButton(); StyleButton(ini, "All settings", false); ini.SetBounds((W - 24) / 2 + 4, 0, (W - 24) / 2 - 4, 32);
        ini.Click += (s, e) => OpenFile(Ini.Path);
        btnRow.Controls.Add(log); btnRow.Controls.Add(ini);
        sp.Controls.Add(btnRow);
        body.Controls.Add(sp);

        AcceptButton = play;
        LoadSettings();
    }

    void SetSpeed(int v) {
        menuSpeed = v;
        for (int i = 0; i < 3; ++i) StyleButton(speedBtns[i], (i + 1) + "x", i + 1 == v);
    }

    public void ForceMode(bool host) { SetMode(host); }
    public void ShowSettings() { settings.Visible = true; settingsBtn.Text = "SETTINGS  ▾"; }

    void SetMode(bool host) {
        isHost = host;
        StyleButton(hostBtn, "Host LAN", host); StyleButton(joinBtn, "Join LAN", !host);
        hostIpRow.Visible = !host;      // only joining needs the host's address
        myIpRow.Visible = host;         // the host tells their friend their IP
    }

    static string MyIps() {
        try {
            var list = NetworkInterface.GetAllNetworkInterfaces()
                .Where(n => n.OperationalStatus == OperationalStatus.Up && n.NetworkInterfaceType != NetworkInterfaceType.Loopback)
                .SelectMany(n => n.GetIPProperties().UnicastAddresses)
                .Where(a => a.Address.AddressFamily == AddressFamily.InterNetwork && !a.Address.ToString().StartsWith("169.254."))   // not the self-assigned ones nobody can reach
                .Select(a => a.Address.ToString()).Distinct().ToArray();
            return list.Length > 0 ? string.Join(", ", list) : "?";
        } catch { return "?"; }
    }

    void OpenFile(string p) { try { if (File.Exists(p)) Process.Start("notepad.exe", "\"" + p + "\""); else status.Text = "Not found yet: " + p; } catch (Exception ex) { status.Text = ex.Message; } }

    void LoadSettings() {
        name.Text = Ini.Get("Player", "Name", Environment.UserName);
        hostIp.Text = Ini.Get("Network", "HostIP", "");
        port.Text = Ini.Get("Network", "Port", "47555");
        skipIntro.Checked = Ini.GetB("Start", "SkipIntro", true);
        direct.Checked = Ini.GetB("Start", "DirectFreeRoam", true);
        noSafeHouse.Checked = Ini.GetB("Start", "SkipLeaveSafeHouseDialog", true);
        pursuitRes.Checked = Ini.GetB("UI", "SkipPursuitResults", false);
        introInput.Checked = Ini.GetB("UI", "IntroInput", true);
        overlay.Checked = Ini.GetB("Debug", "Overlay", false);
        int ms; if (!int.TryParse(Ini.Get("UI", "MenuSpeed", "2"), out ms)) ms = 2;
        SetSpeed(Math.Max(1, Math.Min(3, ms)));
        SetMode(!Ini.Get("Network", "Mode", "host").Trim().Equals("client", StringComparison.OrdinalIgnoreCase));
    }

    bool Save() {
        int p;
        if (!int.TryParse(port.Text.Trim(), out p) || p < 1 || p > 65535) { status.Text = "The port must be a number from 1 to 65535."; return false; }
        if (!isHost && hostIp.Text.Trim().Length == 0) { status.Text = "Enter the host's IP address to join."; return false; }
        if (name.Text.Trim().Length == 0) { status.Text = "Enter your name."; return false; }
        Ini.Set("Player", "Name", name.Text.Trim());
        Ini.Set("Network", "Mode", isHost ? "host" : "client");
        if (!isHost) Ini.Set("Network", "HostIP", hostIp.Text.Trim());
        Ini.Set("Network", "Port", p.ToString());
        Ini.SetB("Start", "SkipIntro", skipIntro.Checked);
        Ini.SetB("Start", "DirectFreeRoam", direct.Checked);
        Ini.SetB("Start", "SkipLeaveSafeHouseDialog", noSafeHouse.Checked);
        Ini.SetB("UI", "SkipPursuitResults", pursuitRes.Checked);
        Ini.SetB("UI", "IntroInput", introInput.Checked);
        Ini.Set("UI", "MenuSpeed", menuSpeed.ToString());
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
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
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
        var args = Environment.GetCommandLineArgs();
        if (args.Length >= 3 && args[1] == "--screenshot") {   // render to a PNG and exit (docs / checks)
            var f = new LauncherForm(dir);
            string opts = string.Join(" ", args, 3, args.Length - 3);   // e.g. "settings join"
            if (opts.Contains("join")) f.ForceMode(false); else if (opts.Contains("host")) f.ForceMode(true);
            if (opts.Contains("settings")) f.ShowSettings();
            f.StartPosition = FormStartPosition.Manual; f.Location = new Point(-4000, -4000); f.ShowInTaskbar = false;
            f.Show(); for (int i = 0; i < 20; ++i) { Application.DoEvents(); System.Threading.Thread.Sleep(15); }
            var bmp = new Bitmap(f.Width, f.Height);
            f.DrawToBitmap(bmp, new Rectangle(0, 0, f.Width, f.Height));
            bmp.Save(args[2], System.Drawing.Imaging.ImageFormat.Png);
            return;
        }
        Application.Run(new LauncherForm(dir));
    }
}
