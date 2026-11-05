using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace winformsshit
{
    public partial class Form1 : Form
    {
        // 14 gb installed + 9 gb temp
        private const long RequiredBytes = 25L * 1024 * 1024 * 1024;

        // stage, seconds, label. textures is in here twice on purpose
        private static readonly (string Stage, double Seconds, string? Label)[] Stages =
        {
            ("keys", 1, null),
            ("discover", 1, null),
            ("textures", 23, "Scanning textures"),
            ("blocks", 100, "Exporting the world"),
            ("tiles", 51, "Building map tiles"),
            ("physics", 13, "Building collision"),
            ("textures", 45, "Converting textures"),
            ("tile materials", 4, "Indexing materials"),
            ("shadow", 27, null),
            ("radio scan", 38, "Scanning radio stations"),
            ("radio encode", 277, "Converting radio audio"),
            ("radio ui", 5, "Radio names and logos"),
            ("swap", 1, null),
        };

        private static readonly string SettingsFile = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "gm_lossantos", "paths.txt");

        private readonly Stopwatch clock = new();
        private LosSantosInstall.Progress? callback;   // gc eats it if this isnt a field
        private bool running;
        private bool closeWhenDone;
        private int stageIndex;
        private int loggedStage;

        public Form1()
        {
            InitializeComponent();

            consoleOutput.ScrollBars = ScrollBars.Vertical;
            progBar.Value = 0;
            progBar.Maximum = 1000;

            gmodPathBtn.Click += (_, _) => Browse(gmodPath, "Select your GarrysMod folder");
            gtaVPathBtn.Click += (_, _) => Browse(gtaVPath, "Select your Grand Theft Auto V folder");
            InstallBtn.Click += InstallBtn_Click;
            FormClosing += Form1_FormClosing;

            LoadPaths();
        }

        private void Browse(TextBox target, string title)
        {
            using var dialog = new FolderBrowserDialog { Description = title, UseDescriptionForTitle = true };
            if (Directory.Exists(target.Text))
                dialog.InitialDirectory = target.Text;
            if (dialog.ShowDialog(this) == DialogResult.OK)
                target.Text = dialog.SelectedPath;
        }

        private void LoadPaths()
        {
            try
            {
                if (File.Exists(SettingsFile))
                {
                    string[] lines = File.ReadAllLines(SettingsFile);
                    if (lines.Length > 0) gmodPath.Text = lines[0];
                    if (lines.Length > 1) gtaVPath.Text = lines[1];
                }
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                // whatever
            }

            if (gmodPath.Text.Length == 0)
                gmodPath.Text = FindSteamApp(4000) ?? "";
            if (gtaVPath.Text.Length == 0)
                gtaVPath.Text = FindSteamApp(271590) ?? FindRockstarGta() ?? "";
        }

        private void SavePaths()
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(SettingsFile)!);
                File.WriteAllLines(SettingsFile, new[] { gmodPath.Text, gtaVPath.Text });
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                Log("Could not remember the folders: " + e.Message);
            }
        }

        private static string? FindSteamApp(int appId)
        {
            try
            {
                if (Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) is not string steam)
                    return null;

                var libraries = new List<string> { steam };
                string vdf = Path.Combine(steam, "steamapps", "libraryfolders.vdf");
                if (File.Exists(vdf))
                    foreach (Match m in Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s+\"(.+?)\""))
                        libraries.Add(m.Groups[1].Value.Replace(@"\\", @"\"));

                foreach (string library in libraries)
                {
                    string manifest = Path.Combine(library, "steamapps", $"appmanifest_{appId}.acf");
                    if (!File.Exists(manifest))
                        continue;
                    Match dir = Regex.Match(File.ReadAllText(manifest), "\"installdir\"\\s+\"(.+?)\"");
                    if (!dir.Success)
                        continue;
                    string path = Path.Combine(library, "steamapps", "common", dir.Groups[1].Value);
                    if (Directory.Exists(path))
                        return Path.GetFullPath(path);
                }
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                // they can just browse
            }
            return null;
        }

        private static string? FindRockstarGta()
        {
            const string key = @"HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Rockstar Games\Grand Theft Auto V";
            return Registry.GetValue(key, "InstallFolder", null) is string path && Directory.Exists(path) ? path : null;
        }

        private static string? ResolveGmod(string path)
        {
            if (File.Exists(Path.Combine(path, "garrysmod", "gameinfo.txt")))
                return Path.GetFullPath(Path.Combine(path, "garrysmod"));
            if (File.Exists(Path.Combine(path, "gameinfo.txt")) &&
                Path.GetFileName(Path.TrimEndingDirectorySeparator(path)).Equals("garrysmod", StringComparison.OrdinalIgnoreCase))
                return Path.GetFullPath(Path.TrimEndingDirectorySeparator(path));
            return null;
        }

        private static bool IsGtaFolder(string path) =>
            File.Exists(Path.Combine(path, "GTA5.exe")) &&
            File.Exists(Path.Combine(path, "x64b.rpf")) &&
            File.Exists(Path.Combine(path, "update", "update.rpf")) &&
            Directory.Exists(Path.Combine(path, "x64", "audio", "sfx"));

        private static bool GmodRunning() =>
            Process.GetProcessesByName("gmod").Length + Process.GetProcessesByName("hl2").Length > 0;

        private static void WarnUser(string text) =>
            MessageBox.Show(text, "gm_lossantos Installer", MessageBoxButtons.OK, MessageBoxIcon.Warning);

        private async void InstallBtn_Click(object? sender, EventArgs e)
        {
            if (running)
            {
                InstallBtn.Enabled = false;
                Log("Cancelling...");
                LosSantosInstall.Cancel();
                return;
            }

            string? gmod = ResolveGmod(gmodPath.Text.Trim());
            string gta = gtaVPath.Text.Trim();
            if (gmod == null)
            {
                WarnUser("That is not a Garry's Mod folder: garrysmod\\gameinfo.txt is missing.");
                return;
            }
            if (!File.Exists(Path.Combine(Path.GetDirectoryName(gmod)!, "bin", "win64", "gmod.exe")))
            {
                WarnUser("gm_lossantos needs 64-bit Garry's Mod. In Steam: right-click Garry's Mod > " +
                         "Properties > Betas, choose x86-64, wait for the update, then install again.");
                return;
            }
            if (!IsGtaFolder(gta))
            {
                WarnUser("That is not a GTA V folder. It needs GTA5.exe, x64b.rpf, update\\update.rpf and " +
                         "x64\\audio\\sfx, so the Enhanced edition (GTA5_Enhanced.exe) will not work.");
                return;
            }
            if (GmodRunning())
            {
                WarnUser("Close Garry's Mod before installing.");
                return;
            }
            long free = new DriveInfo(Path.GetPathRoot(gmod)!).AvailableFreeSpace;
            if (free < RequiredBytes)
            {
                WarnUser($"The install needs {GiB(RequiredBytes)} GB free on the Garry's Mod drive; " +
                         $"it has {GiB(free)} GB.");
                return;
            }

            SavePaths();
            consoleOutput.Clear();
            await RunInstall(gmod, gta, LosSantosInstall.DoAll);
        }

        private async Task RunInstall(string gmod, string gta, int flags)
        {
            var options = new LosSantosOptions
            {
                GtaDir = gta,
                GmodDir = gmod,
                Background = 1,     // so the pc doesnt die
                Flags = flags,
            };

            callback = OnProgress;
            stageIndex = 0;
            loggedStage = -1;
            SetProgress(0);
            if (flags != LosSantosInstall.DoSwap)
                clock.Restart();    // retry is the same install
            SetRunning(true);
            int result;
            string error;
            try
            {
                if (LosSantosInstall.Start(ref options, callback, IntPtr.Zero) == 0)
                {
                    result = LosSantosInstall.ResultFailed;
                }
                else
                {
                    await Task.Run(() => LosSantosInstall.Wait(uint.MaxValue));
                    result = LosSantosInstall.Result();
                }
                error = LosSantosInstall.Error();
            }
            catch (Exception e) when (e is DllNotFoundException or BadImageFormatException or EntryPointNotFoundException)
            {
                result = LosSantosInstall.ResultFailed;
                error = "lossantos_install.dll is missing or damaged. Keep the installer's files together. (" + e.Message + ")";
            }
            SetRunning(false);

            switch (result)
            {
                case LosSantosInstall.ResultOk:
                    SetProgress(1);
                    Log($"Installed in {clock.Elapsed.TotalMinutes:0.0} minutes.");
                    Log("Subscribe to the gm_lossantos base addon on the Workshop, then start the map gm_lossantos.");
                    break;
                case LosSantosInstall.ResultCancelled:
                    SetProgress(0);
                    Log("Cancelled. Nothing was changed.");
                    break;
                default:
                    Log("Failed: " + error);
                    if (GmodRunning() && !closeWhenDone && AskRetrySwap())
                    {
                        await RunInstall(gmod, gta, LosSantosInstall.DoSwap);
                        return;
                    }
                    if (!closeWhenDone)
                        MessageBox.Show(error, "gm_lossantos Installer", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    break;
            }

            if (closeWhenDone)
                Close();
        }

        private bool AskRetrySwap() =>
            MessageBox.Show(
                "Garry's Mod was opened during the install. Close it and press Retry to finish; " +
                "the converted data is ready and does not need to be rebuilt.",
                "gm_lossantos Installer", MessageBoxButtons.RetryCancel, MessageBoxIcon.Warning) == DialogResult.Retry;

        private void OnProgress(IntPtr user, IntPtr stagePtr, int done, int total, IntPtr messagePtr)
        {
            string stage = Marshal.PtrToStringUTF8(stagePtr) ?? "";
            string message = Marshal.PtrToStringUTF8(messagePtr) ?? "";
            if (stage != "done")
                BeginInvoke(() => ShowProgress(stage, done, total, message));
        }

        private void ShowProgress(string stage, int done, int total, string message)
        {
            // only goes forward, thats how the 2 textures stages work. kinda stupid
            for (int i = stageIndex; i < Stages.Length; i++)
            {
                if (Stages[i].Stage == stage)
                {
                    stageIndex = i;
                    break;
                }
            }

            if (message.Length > 0)
                Log(message);
            else if (Stages[stageIndex].Stage == stage && stageIndex != loggedStage && Stages[stageIndex].Label is string label)
                Log(label + "...");
            loggedStage = stageIndex;

            double fraction = total > 0 ? (double)done / total : 0;
            double before = Stages.Take(stageIndex).Sum(s => s.Seconds);
            SetProgress((before + Stages[stageIndex].Seconds * fraction) / Stages.Sum(s => s.Seconds));
        }

        private void SetProgress(double fraction)
        {
            int value = (int)(Math.Clamp(fraction, 0, 1) * progBar.Maximum);
            // threads show up out of order
            if (value == 0 || value > progBar.Value)
                progBar.Value = value;
        }

        private void SetRunning(bool value)
        {
            running = value;
            gmodPath.Enabled = gmodPathBtn.Enabled = gtaVPath.Enabled = gtaVPathBtn.Enabled = !value;
            InstallBtn.Text = value ? "Cancel" : "Install";
            InstallBtn.Enabled = true;
        }

        private void Log(string line) =>
            consoleOutput.AppendText($"[{clock.Elapsed:mm\\:ss}] {line}{Environment.NewLine}");

        private static long GiB(long bytes) => bytes / (1024 * 1024 * 1024);

        private void Form1_FormClosing(object? sender, FormClosingEventArgs e)
        {
            if (!running)
                return;
            e.Cancel = true;
            if (closeWhenDone)
                return;
            DialogResult answer = MessageBox.Show("Cancel the install and close?", "gm_lossantos Installer",
                                                  MessageBoxButtons.YesNo, MessageBoxIcon.Question);
            if (answer != DialogResult.Yes)
                return;
            closeWhenDone = true;
            InstallBtn.Enabled = false;
            Log("Cancelling...");
            LosSantosInstall.Cancel();
        }
    }
}
