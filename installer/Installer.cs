// GUI installer for the plugins in this repo. One exe: the plugin DLLs are embedded as resources named
// "plugin.<Name>.dll" (build.ps1 -Installer). C# 5 / .NET Framework 4 so the csc.exe that ships with Windows builds it
// and every Windows 10/11 PC runs it without installing anything. Runs elevated (app.manifest).
// Does what install.ps1 / uninstall.ps1 do: stop System Informer, edit settings.json, copy or delete the DLLs,
// start System Informer again.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: AssemblyTitle("System Informer plugins installer")]
[assembly: AssemblyProduct("systeminformer-plugins")]

static class Info
{
    // Set by build.ps1 -Installer from the -Version argument and the cloned System Informer tag.
    public const string Version = "@VERSION@";
    public const string SystemInformerVersion = "@SIVERSION@";
}

class PluginEntry
{
    public string Name;
    public string Description;
    public byte[] Data;
}

class InstallerForm : Form
{
    static readonly Dictionary<string, string> Descriptions = new Dictionary<string, string>
    {
        { "GpuMonitor", "A page per GPU (any GPU, also NVIDIA TCC), per-app GPU load and memory, tray icons. Replaces System Informer's own GPU page." },
        { "DiskMonitor", "One Disks page: a tile per disk, speed, response time, temperature, per-app I/O and SMART health, tray icons." },
        { "MemBreakdown", "Where is my memory going: every physical page by use (like RAMMap), top processes, pool tags, cached files, tray icon." },
        { "NvTccGpu", "Older, NVIDIA TCC/MCDM GPUs only (Tesla P40, A100...). GpuMonitor does all of this: install one or the other." },
    };
    // Plugins that must not be installed together.
    static readonly string[][] Exclusive = { new[] { "GpuMonitor", "NvTccGpu" } };

    readonly List<PluginEntry> plugins = new List<PluginEntry>();
    readonly TextBox folderBox = new TextBox();
    readonly Label versionLabel = new Label();
    readonly ListView list = new ListView();
    readonly CheckBox gpuPageBox = new CheckBox();
    readonly TextBox logBox = new TextBox();
    readonly Button applyButton = new Button();
    readonly Button removeAllButton = new Button();
    readonly Button closeButton = new Button();
    bool versionOk;
    bool busy;
    bool settingChecks;
    bool ready;   // set once the window exists (see OnItemChecked)

    protected override void OnShown(EventArgs e)
    {
        base.OnShown(e);
        ready = true;
    }

    static string SettingsPath
    {
        get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), @"SystemInformer\settings.json"); }
    }

    string PluginsDir { get { return Path.Combine(folderBox.Text.Trim(), "plugins"); } }

    public InstallerForm()
    {
        AutoScaleDimensions = new SizeF(96f, 96f);
        AutoScaleMode = AutoScaleMode.Dpi;
        Text = "System Informer plugins " + Info.Version;
        Font = new Font("Segoe UI", 9f);
        ClientSize = new Size(720, 560);
        MinimumSize = new Size(600, 480);
        StartPosition = FormStartPosition.CenterScreen;

        Assembly asm = Assembly.GetExecutingAssembly();
        foreach (string res in asm.GetManifestResourceNames())
        {
            if (!res.StartsWith("plugin.", StringComparison.Ordinal) || !res.EndsWith(".dll", StringComparison.Ordinal)) continue;
            PluginEntry p = new PluginEntry();
            p.Name = res.Substring(7, res.Length - 11);
            string d;
            p.Description = Descriptions.TryGetValue(p.Name, out d) ? d : "";
            using (Stream s = asm.GetManifestResourceStream(res))
            using (MemoryStream m = new MemoryStream())
            {
                s.CopyTo(m);
                p.Data = m.ToArray();
            }
            plugins.Add(p);
        }
        plugins.Sort((a, b) => Order(a.Name).CompareTo(Order(b.Name)));

        Label folderLabel = new Label { Text = "System Informer folder:", AutoSize = true, Location = new Point(12, 16) };
        folderBox.SetBounds(160, 12, 440, 23);
        folderBox.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        folderBox.Text = FindSystemInformer();
        folderBox.TextChanged += delegate { Refresh2(); };
        Button browse = new Button { Text = "Browse...", Anchor = AnchorStyles.Top | AnchorStyles.Right };
        browse.SetBounds(608, 11, 100, 25);
        browse.Click += delegate
        {
            using (FolderBrowserDialog dlg = new FolderBrowserDialog())
            {
                dlg.Description = "Folder that contains SystemInformer.exe";
                if (dlg.ShowDialog(this) == DialogResult.OK) folderBox.Text = dlg.SelectedPath;
            }
        };
        versionLabel.SetBounds(12, 42, 696, 36);
        versionLabel.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;

        Label hint = new Label
        {
            Text = "Ticked plugins are installed or updated, unticked ones are removed. Then click Apply.",
            AutoSize = true, Location = new Point(12, 82)
        };
        list.SetBounds(12, 102, 696, 170);
        list.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        list.View = View.Details;
        list.CheckBoxes = true;
        list.FullRowSelect = true;
        list.HeaderStyle = ColumnHeaderStyle.Nonclickable;
        list.ShowItemToolTips = true;
        list.Columns.Add("Plugin", 120);
        list.Columns.Add("Now", 120);
        list.Columns.Add("What it does", 450);
        list.ItemChecked += OnItemChecked;

        gpuPageBox.Text = "Turn off System Informer's own GPU page and GPU columns (GpuMonitor replaces them)";
        gpuPageBox.SetBounds(12, 280, 696, 22);
        gpuPageBox.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;

        Label warn = new Label
        {
            Text = "These plugins are not signed. While one is loaded, System Informer shows a one-time \"unsigned images\" " +
                "warning and its driver runs with reduced protection (no details of protected system processes). " +
                "Removing all plugins restores it. System Informer is closed and started again while installing.",
            ForeColor = Color.FromArgb(150, 80, 0)
        };
        warn.SetBounds(12, 306, 696, 50);
        warn.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;

        logBox.Multiline = true;
        logBox.ReadOnly = true;
        logBox.ScrollBars = ScrollBars.Vertical;
        logBox.SetBounds(12, 360, 696, 150);
        logBox.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;

        applyButton.Text = "Apply";
        applyButton.SetBounds(392, 522, 100, 28);
        removeAllButton.Text = "Remove all";
        removeAllButton.SetBounds(500, 522, 100, 28);
        closeButton.Text = "Close";
        closeButton.SetBounds(608, 522, 100, 28);
        foreach (Button b in new[] { applyButton, removeAllButton, closeButton })
            b.Anchor = AnchorStyles.Bottom | AnchorStyles.Right;
        applyButton.Click += delegate { Run(false); };
        removeAllButton.Click += delegate { Run(true); };
        closeButton.Click += delegate { Close(); };
        AcceptButton = applyButton;
        CancelButton = closeButton;

        Controls.AddRange(new Control[] { folderLabel, folderBox, browse, versionLabel, hint, list, gpuPageBox, warn, logBox,
            applyButton, removeAllButton, closeButton });

        string gpu = ReadSetting(SettingsPath, "ExtendedTools.EnableGpuMonitor");
        gpuPageBox.Checked = gpu == null ? true : gpu == "0";
        Refresh2();
        if (plugins.Count == 0) Log("This installer contains no plugins (build error).");
    }

    static int Order(string name)
    {
        string[] order = { "GpuMonitor", "DiskMonitor", "MemBreakdown", "NvTccGpu" };
        int i = Array.IndexOf(order, name);
        return i < 0 ? order.Length : i;
    }

    static string FindSystemInformer()
    {
        foreach (RegistryView view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
        {
            try
            {
                using (RegistryKey hk = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view))
                using (RegistryKey k = hk.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\SystemInformer"))
                {
                    if (k != null)
                    {
                        string loc = k.GetValue("InstallLocation") as string;
                        if (!string.IsNullOrEmpty(loc) && File.Exists(Path.Combine(loc, "SystemInformer.exe")))
                            return loc.TrimEnd('\\');
                    }
                }
            }
            catch (Exception) { }
        }
        return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "SystemInformer");
    }

    // Re-reads the System Informer version and what is installed, and resets the ticks to the installed state.
    void Refresh2()
    {
        string exe = Path.Combine(folderBox.Text.Trim(), "SystemInformer.exe");
        versionOk = false;
        if (!File.Exists(exe))
        {
            versionLabel.ForeColor = Color.Firebrick;
            versionLabel.Text = "SystemInformer.exe is not in this folder. Install System Informer " + Info.SystemInformerVersion +
                " (system-informer.com), or pick its folder.";
        }
        else
        {
            string v = FileVersionInfo.GetVersionInfo(exe).FileVersion ?? "";
            v = v.Replace(", ", ".").Trim();
            if (v == Info.SystemInformerVersion)
            {
                versionOk = true;
                versionLabel.ForeColor = Color.DarkGreen;
                versionLabel.Text = "System Informer " + v + " found. These plugins are built for this version.";
            }
            else
            {
                versionLabel.ForeColor = Color.Firebrick;
                versionLabel.Text = "System Informer " + v + " found, but these plugins are built for " + Info.SystemInformerVersion +
                    " and would crash it. Install that version, or get the plugins release for yours. You can still remove plugins.";
            }
        }

        settingChecks = true;
        list.BeginUpdate();
        list.Items.Clear();
        foreach (PluginEntry p in plugins)
        {
            string status = Status(p);
            ListViewItem item = new ListViewItem(new[] { p.Name, status, p.Description });
            item.Tag = p;
            item.ToolTipText = p.Description;
            item.Checked = status != "Not installed";
            list.Items.Add(item);
        }
        list.EndUpdate();
        settingChecks = false;
        UpdateButtons();
    }

    string Status(PluginEntry p)
    {
        string target = Path.Combine(PluginsDir, p.Name + ".dll");
        if (!File.Exists(target)) return "Not installed";
        try
        {
            return Hash(File.ReadAllBytes(target)) == Hash(p.Data) ? "Installed" : "Other version";
        }
        catch (Exception) { return "Installed"; }
    }

    static string Hash(byte[] data)
    {
        using (SHA256 sha = SHA256.Create()) return Convert.ToBase64String(sha.ComputeHash(data));
    }

    void OnItemChecked(object sender, ItemCheckedEventArgs e)
    {
        // The ListView raises ItemChecked while it creates its window (rows half built): ignore until shown.
        if (settingChecks || !ready || e.Item == null || !(e.Item.Tag is PluginEntry)) return;
        if (e.Item.Checked)
        {
            string name = ((PluginEntry)e.Item.Tag).Name;
            settingChecks = true;
            foreach (string[] group in Exclusive)
            {
                if (Array.IndexOf(group, name) < 0) continue;
                foreach (ListViewItem other in list.Items)
                {
                    PluginEntry op = other == null ? null : other.Tag as PluginEntry;
                    if (op == null) continue;
                    string n = op.Name;
                    if (n != name && Array.IndexOf(group, n) >= 0) other.Checked = false;
                }
            }
            settingChecks = false;
        }
        UpdateButtons();
    }

    void UpdateButtons()
    {
        applyButton.Enabled = !busy && File.Exists(Path.Combine(folderBox.Text.Trim(), "SystemInformer.exe"));
        removeAllButton.Enabled = applyButton.Enabled;
        closeButton.Enabled = !busy;
        list.Enabled = !busy;
        gpuPageBox.Enabled = !busy;
    }

    void Log(string text)
    {
        if (InvokeRequired) { BeginInvoke(new Action<string>(Log), text); return; }
        logBox.AppendText(DateTime.Now.ToString("HH:mm:ss") + "  " + text + "\r\n");
    }

    void Run(bool removeAll)
    {
        List<PluginEntry> install = new List<PluginEntry>();
        List<PluginEntry> remove = new List<PluginEntry>();
        foreach (ListViewItem item in list.Items)
        {
            PluginEntry p = (PluginEntry)item.Tag;
            if (item.Checked && !removeAll) install.Add(p); else remove.Add(p);
        }
        if (install.Count > 0 && !versionOk)
        {
            MessageBox.Show(this, versionLabel.Text, Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        if (removeAll && MessageBox.Show(this, "Remove all of these plugins from System Informer?", Text,
            MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;

        string folder = folderBox.Text.Trim();
        bool hasGpuMonitor = install.Exists(p => p.Name == "GpuMonitor");
        string gpuPage = hasGpuMonitor ? (gpuPageBox.Checked ? "0" : "1") : "1";
        busy = true;
        UpdateButtons();
        Thread t = new Thread(delegate ()
        {
            bool ok = Work(folder, install, remove, gpuPage);
            BeginInvoke(new Action(delegate
            {
                busy = false;
                Refresh2();
                if (ok) Log(install.Count > 0
                    ? "Done. In System Informer: View > System Information for the new pages, View > Tray icons for the icons."
                    : "Done.");
            }));
        });
        t.IsBackground = true;
        t.Start();
    }

    bool Work(string folder, List<PluginEntry> install, List<PluginEntry> remove, string gpuPage)
    {
        bool ok = true;
        string pluginsDir = Path.Combine(folder, "plugins");
        try
        {
            // 1. Stop System Informer. With HideOnClose it only hides when closed, and it writes settings.json on exit.
            Process[] running = Process.GetProcessesByName("SystemInformer");
            foreach (Process p in running)
            {
                try { p.Kill(); p.WaitForExit(15000); } catch (Exception ex) { Log("Could not stop System Informer (" + p.Id + "): " + ex.Message); }
            }
            Log(running.Length > 0 ? "Stopped System Informer." : "System Informer was not running.");

            // 2. Settings: third-party plugins load only with EnableDefaultSafePlugins = 0. Keep it 0 while any
            //    unsigned plugin stays (ours, or someone else's: System Informer's own have a .sig beside them).
            EditSettings(install.Count > 0 || OtherUnsignedPlugins(pluginsDir, remove), gpuPage);

            // 3. Copy. The old DLL can stay locked for a few seconds after System Informer exits.
            Directory.CreateDirectory(pluginsDir);
            foreach (PluginEntry p in install)
            {
                string target = Path.Combine(pluginsDir, p.Name + ".dll");
                Retry(delegate { File.WriteAllBytes(target, p.Data); }, p.Name + ".dll is locked");
                Log("Installed " + p.Name + ".");
            }
            // 4. Remove.
            foreach (PluginEntry p in remove)
            {
                string target = Path.Combine(pluginsDir, p.Name + ".dll");
                if (!File.Exists(target)) continue;
                Retry(delegate { File.Delete(target); }, p.Name + ".dll is locked");
                Log("Removed " + p.Name + ".");
            }
        }
        catch (Exception ex)
        {
            Log("ERROR: " + ex.Message);
            ok = false;
        }
        finally
        {
            // 5. Always start System Informer again, even after an error.
            try
            {
                Process.Start(new ProcessStartInfo(Path.Combine(folder, "SystemInformer.exe")) { UseShellExecute = true, WorkingDirectory = folder });
                Log("Started System Informer.");
            }
            catch (Exception ex) { Log("Could not start System Informer: " + ex.Message); }
        }
        return ok;
    }

    static bool OtherUnsignedPlugins(string pluginsDir, List<PluginEntry> remove)
    {
        if (!Directory.Exists(pluginsDir)) return false;
        foreach (string dll in Directory.GetFiles(pluginsDir, "*.dll"))
        {
            string name = Path.GetFileNameWithoutExtension(dll);
            if (remove.Exists(p => string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase))) continue;
            if (!File.Exists(Path.ChangeExtension(dll, ".sig"))) return true;
        }
        return false;
    }

    static void Retry(Action action, string message)
    {
        for (int i = 1; ; i++)
        {
            try { action(); return; }
            catch (IOException) { if (i >= 20) throw new IOException(message + " after 20 s."); }
            catch (UnauthorizedAccessException) { if (i >= 20) throw new IOException(message + " after 20 s."); }
            Thread.Sleep(1000);
        }
    }

    void EditSettings(bool anyInstalled, string gpuPage)
    {
        string path = SettingsPath;
        string safe = anyInstalled ? "0" : "1";
        if (!File.Exists(path))
        {
            // System Informer has never saved its settings: start it with just this one; it adds the defaults.
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            File.WriteAllText(path, "{\n\"EnableDefaultSafePlugins\":\"" + safe + "\",\n\"ExtendedTools.EnableGpuMonitor\":\"" + gpuPage + "\"\n}\n",
                new UTF8Encoding(false));
            Log("Created settings.json.");
            return;
        }
        string text = File.ReadAllText(path);
        string backup = path + ".bak-plugins";
        if (!File.Exists(backup)) File.Copy(path, backup);
        string updated = SetSetting(text, "EnableDefaultSafePlugins", safe);
        updated = SetSetting(updated, "ExtendedTools.EnableGpuMonitor", gpuPage);
        if (updated != text)
        {
            File.WriteAllText(path, updated, new UTF8Encoding(false));
            Log("Settings: third-party plugins " + (safe == "0" ? "allowed" : "off again (System Informer's default)") +
                ", System Informer's GPU page " + (gpuPage == "0" ? "off" : "on") + ". Backup: settings.json.bak-plugins");
        }
    }

    static string ReadSetting(string path, string name)
    {
        try
        {
            Match m = Regex.Match(File.ReadAllText(path), "\"" + Regex.Escape(name) + "\":\"([^\"]*)\"");
            return m.Success ? m.Groups[1].Value : null;
        }
        catch (Exception) { return null; }
    }

    static string SetSetting(string text, string name, string value)
    {
        string pattern = "\"" + Regex.Escape(name) + "\":\"[^\"]*\"";
        string entry = "\"" + name + "\":\"" + value + "\"";
        if (Regex.IsMatch(text, pattern)) return Regex.Replace(text, pattern, entry);
        int brace = text.IndexOf('{');
        return brace < 0 ? text : text.Substring(0, brace + 1) + "\n" + entry + "," + text.Substring(brace + 1);
    }

    [STAThread]
    static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new InstallerForm());
    }
}
