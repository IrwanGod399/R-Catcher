using System;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace UI
{
    public class LogRecordModel
    {
        public string Time { get; set; } = string.Empty;
        public string PID { get; set; } = string.Empty;
        public string Process { get; set; } = string.Empty;
        public string Name { get; set; } = string.Empty;
        public string Status { get; set; } = string.Empty;
        public string ActionTaken { get; set; } = "Monitored";
    }

    public partial class MainWindow : Window
    {
        private ObservableCollection<LogRecordModel> allLogs = new ObservableCollection<LogRecordModel>();
        private ObservableCollection<LogRecordModel> displayedLogs = new ObservableCollection<LogRecordModel>();

        private IntPtr hPort = IntPtr.Zero;
        private bool isMonitoring = false;
        private CancellationTokenSource cts = null;
        private int blockedCount = 0;

        private const string BAIT_PATH = @"C:\Honey";

        //  Daftar folder tujuan tempat honeyfile ditanam (dipilih user via dialog Browse)
        private ObservableCollection<string> targetFolders = new ObservableCollection<string>();

        //  Definisi honeyfile (mengikuti konsep /g di mspyUser.c):
        //  di tiap folder tujuan dibuat subfolder "1" & "z", masing-masing diisi
        //  5 nama x 5 ekstensi = 25 honeyfile berukuran acak berbeda-beda.
        private static readonly string[] HoneyBaseNames = { "passwords", "laporan_keuangan", "gaji_karyawan", "private_key", "backup_db" };
        private static readonly string[] HoneyExts = { ".log", ".bin", ".docx", ".pdf", ".xlsx" };
        private static readonly string[] HoneySubFolders = { "1", "z" };

        //  Rentang ukuran honeyfile (byte): acak antara 4 KB s/d 256 KB
        private const int HONEY_MIN_BYTES = 4 * 1024;
        private const int HONEY_MAX_BYTES = 256 * 1024;

        public MainWindow()
        {
            InitializeComponent();
            dgLogs.ItemsSource = displayedLogs;
            lstFolders.ItemsSource = targetFolders;

            //  Folder tujuan default. Saat Deploy, di dalamnya digenerate subfolder "1" & "z".
            //  Bisa dihapus lewat tombol Remove atau ditambah lewat Browse (mis. Documents).
            targetFolders.Add(BAIT_PATH);
            UpdateFolderCount();
        }

        private void UpdateFolderCount()
        {
            lblFolderCount.Text = "(" + targetFolders.Count + ")";
        }

        //  Tulis satu honeyfile berisi data berpola (byte = x % 255), sebesar sizeBytes.
        //  Pola ini membuat file tampak punya struktur data (bukan kosong) — sama seperti /g.
        private static void WriteHoneyfile(string path, int sizeBytes)
        {
            //  Kalau file lama sudah ber-atribut Hidden, WriteAllBytes akan gagal
            //  ("Access denied"). Jadi normalkan atributnya dulu sebelum menulis ulang.
            if (File.Exists(path))
                File.SetAttributes(path, FileAttributes.Normal);

            byte[] data = new byte[sizeBytes];
            for (int x = 0; x < sizeBytes; x++)
                data[x] = (byte)(x % 255);
            File.WriteAllBytes(path, data);

            //  Sembunyikan file supaya tidak terlihat/diklik user di Explorer.
            File.SetAttributes(path, FileAttributes.Hidden);
        }

        //  Hapus semua honeyfile (subfolder "1" & "z") di dalam satu folder root.
        //  Folder root (mis. Documents) TIDAK disentuh; subfolder "1"/"z" dihapus bila sudah kosong.
        private static void DeleteHoneyfilesInFolder(string rootDir)
        {
            foreach (var sub in HoneySubFolders)
            {
                string subDir = Path.Combine(rootDir, sub);

                foreach (var name in HoneyBaseNames)
                {
                    foreach (var ext in HoneyExts)
                    {
                        try
                        {
                            string fullPath = Path.Combine(subDir, name + ext);
                            if (File.Exists(fullPath))
                                File.Delete(fullPath);
                        }
                        catch { }
                    }
                }

                try
                {
                    if (Directory.Exists(subDir) &&
                        Directory.GetFileSystemEntries(subDir).Length == 0)
                    {
                        Directory.Delete(subDir, false);
                    }
                }
                catch { }
            }
        }

        //  Sinkronkan daftar target di kernel dengan honeyfile yang MASIH ADA di folder-folder ini.
        //  Karena kernel hanya punya "clear semua" + "add", kita clear lalu daftar ulang yang tersisa.
        private void ResyncKernelTargets(string[] folders)
        {
            SendKernelCommand(DriverBridge.COMMAND_CLEAR_TARGETS, string.Empty);

            foreach (var rootDir in folders)
            {
                foreach (var sub in HoneySubFolders)
                {
                    string subDir = Path.Combine(rootDir, sub);
                    foreach (var name in HoneyBaseNames)
                    {
                        foreach (var ext in HoneyExts)
                        {
                            string fullPath = Path.Combine(subDir, name + ext);
                            if (File.Exists(fullPath))
                            {
                                string ntPath = DriverBridge.ConvertDosPathToNtPath(fullPath);
                                if (!string.IsNullOrEmpty(ntPath))
                                    SendKernelCommand(DriverBridge.COMMAND_ADD_TARGET, ntPath);
                            }
                        }
                    }
                }
            }
        }

        private void btnBrowseFolder_Click(object sender, RoutedEventArgs e)
        {
            using (var dlg = new System.Windows.Forms.FolderBrowserDialog())
            {
                dlg.Description = "Pilih folder tujuan tempat honeyfile akan ditanam";
                dlg.ShowNewFolderButton = true;

                if (dlg.SelectedPath == string.Empty && Directory.Exists(BAIT_PATH))
                    dlg.SelectedPath = BAIT_PATH;

                if (dlg.ShowDialog() == System.Windows.Forms.DialogResult.OK)
                {
                    string chosen = dlg.SelectedPath;

                    //  Cegah duplikat (case-insensitive)
                    bool exists = false;
                    foreach (var f in targetFolders)
                    {
                        if (string.Equals(f, chosen, StringComparison.OrdinalIgnoreCase))
                        {
                            exists = true;
                            break;
                        }
                    }

                    if (exists)
                    {
                        lblStatus.Text = "Folder sudah ada di daftar: " + chosen;
                    }
                    else
                    {
                        targetFolders.Add(chosen);
                        UpdateFolderCount();
                        lblStatus.Text = "Folder ditambahkan: " + chosen;
                    }
                }
            }
        }

        private async void btnRemoveFolder_Click(object sender, RoutedEventArgs e)
        {
            if (!(lstFolders.SelectedItem is string selected))
            {
                lblStatus.Text = "Pilih dulu folder di daftar yang ingin dihapus.";
                return;
            }

            string folderToRemove = selected;

            //  1. Keluarkan dari daftar UI
            targetFolders.Remove(folderToRemove);
            UpdateFolderCount();
            lblStatus.Text = "Menghapus honeyfile di: " + folderToRemove + " ...";

            //  2. Snapshot folder yang tersisa (untuk resync kernel) + status port
            string[] remaining = targetFolders.ToArray();
            bool portOk = EnsurePortConnected();

            //  3. Hapus honeyfile fisik folder itu + sinkronkan target kernel (di background)
            await Task.Run(() =>
            {
                DeleteHoneyfilesInFolder(folderToRemove);
                if (portOk)
                    ResyncKernelTargets(remaining);
            });

            lblStatus.Text = "Folder dihapus & honeyfile-nya dibersihkan: " + folderToRemove;
        }

        private bool EnsurePortConnected()
        {
            if (hPort != IntPtr.Zero && hPort != new IntPtr(-1))
                return true;

            int hResult = DriverBridge.FilterConnectCommunicationPort(
                DriverBridge.MINISPY_PORT_NAME, 0, IntPtr.Zero, 0, IntPtr.Zero, out hPort);

            return hResult == 0;
        }

        private void btnLoad_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                var psi = new ProcessStartInfo("fltmc", "load minispy")
                {
                    UseShellExecute = true,
                    Verb = "runas",
                    CreateNoWindow = true
                };
                Process.Start(psi)?.WaitForExit();

                StringBuilder instName = new StringBuilder(256);
                DriverBridge.FilterAttach("minispy", "C:\\", null, (uint)instName.Capacity, instName);

                if (EnsurePortConnected())
                {
                    lblStatus.Text = "Port Connected & Filter Attached to C:";
                    statusIndicator.Fill = new SolidColorBrush(Color.FromRgb(56, 189, 248));
                }
                else
                {
                    lblStatus.Text = "Driver Loaded, Port Waiting";
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show("Gagal memuat driver: " + ex.Message);
            }
        }

        private async void btnDeploy_Click(object sender, RoutedEventArgs e)
        {
            if (!EnsurePortConnected())
            {
                MessageBox.Show("Hubungkan driver terlebih dahulu via tombol gear.");
                return;
            }

            //  Snapshot daftar folder (ObservableCollection tidak thread-safe untuk Task.Run)
            string[] folders = targetFolders.ToArray();

            if (folders.Length == 0)
            {
                MessageBox.Show("Belum ada folder target. Tambahkan folder dulu lewat tombol Browse.");
                return;
            }

            lblStatus.Text = "Deploying honeyfiles ke " + folders.Length + " folder...";

            int deployedCount = await Task.Run(() =>
            {
                int count = 0;
                var rnd = new Random();

                foreach (var rootDir in folders)
                {
                    try
                    {
                        if (!Directory.Exists(rootDir))
                            Directory.CreateDirectory(rootDir);

                        //  Generate subfolder "1" dan "z" di dalam folder tujuan (konsep /g)
                        foreach (var sub in HoneySubFolders)
                        {
                            string subDir = Path.Combine(rootDir, sub);
                            if (!Directory.Exists(subDir))
                                Directory.CreateDirectory(subDir);

                            //  Sembunyikan subfolder bait ("1"/"z") juga (pertahankan flag Directory)
                            try { File.SetAttributes(subDir, File.GetAttributes(subDir) | FileAttributes.Hidden); }
                            catch { }

                            //  25 honeyfile (5 nama x 5 ekstensi) per subfolder
                            foreach (var name in HoneyBaseNames)
                            {
                                foreach (var ext in HoneyExts)
                                {
                                    string fullPath = Path.Combine(subDir, name + ext);

                                    //  Ukuran acak berbeda-beda tiap file (4 KB - 256 KB)
                                    int sizeBytes = rnd.Next(HONEY_MIN_BYTES, HONEY_MAX_BYTES + 1);
                                    WriteHoneyfile(fullPath, sizeBytes);

                                    string ntPath = DriverBridge.ConvertDosPathToNtPath(fullPath);
                                    if (!string.IsNullOrEmpty(ntPath))
                                    {
                                        SendKernelCommand(DriverBridge.COMMAND_ADD_TARGET, ntPath);
                                        count++;
                                    }
                                }
                            }
                        }
                    }
                    catch (Exception ex)
                    {
                        Dispatcher.Invoke(() => MessageBox.Show("Gagal membuat bait di " + rootDir + ": " + ex.Message));
                    }
                }
                return count;
            });

            lblBaitStatus.Text = "Active (" + folders.Length + " folder)";
            lblBaitStatus.Foreground = new SolidColorBrush(Color.FromRgb(16, 185, 129));
            lblStatus.Text = deployedCount + " honeyfile ditanam & target di-set ke kernel";
        }

        private async void btnCleanBait_Click(object sender, RoutedEventArgs e)
        {
            if (EnsurePortConnected())
            {
                SendKernelCommand(DriverBridge.COMMAND_CLEAR_TARGETS, string.Empty);
            }

            lblStatus.Text = "Cleaning up honeyfiles...";

            //  Snapshot daftar folder untuk dipakai di thread background
            string[] folders = targetFolders.ToArray();

            await Task.Run(() =>
            {
                foreach (var rootDir in folders)
                    DeleteHoneyfilesInFolder(rootDir);
            });

            lblBaitStatus.Text = "Cleared";
            lblBaitStatus.Foreground = new SolidColorBrush(Color.FromRgb(239, 68, 68));
            lblStatus.Text = "Honeyfile dihapus dari " + folders.Length + " folder & target kernel di-clear";
        }

        private void SendKernelCommand(uint commandCode, string targetPath)
        {
            var msg = new DriverBridge.MINISPY_COMMAND_MSG
            {
                Command = commandCode,
                NameBuffer = targetPath ?? string.Empty
            };

            int size = Marshal.SizeOf(msg);
            IntPtr buffer = Marshal.AllocHGlobal(size);

            try
            {
                Marshal.StructureToPtr(msg, buffer, false);
                uint bytesReturned = 0;
                DriverBridge.FilterSendMessage(hPort, buffer, (uint)size, IntPtr.Zero, 0, out bytesReturned);
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }

        private void btnStart_Click(object sender, RoutedEventArgs e)
        {
            if (!EnsurePortConnected())
            {
                MessageBox.Show("Port driver tidak tersedia. Pastikan driver aktif.");
                return;
            }

            btnStart.IsEnabled = false;
            btnStop.IsEnabled = true;
            isMonitoring = true;
            cts = new CancellationTokenSource();

            //  Aktifkan proteksi di kernel (mulai memblokir/terminate)
            SendKernelCommand(DriverBridge.COMMAND_START_MONITORING, string.Empty);

            statusIndicator.Fill = new SolidColorBrush(Color.FromRgb(16, 185, 129));
            lblStatus.Text = "Kernel Port Stream Active | Proteksi ON";

            Task.Run(() => DirectKernelPortListener(cts.Token));
        }

        private void btnStop_Click(object sender, RoutedEventArgs e)
        {
            isMonitoring = false;
            if (cts != null)
            {
                cts.Cancel();
                cts = null;
            }

            //  Matikan proteksi di kernel (berhenti memblokir/terminate).
            //  Daftar honeyfile tetap tersimpan, jadi Start berikutnya cukup mengaktifkan lagi.
            if (EnsurePortConnected())
                SendKernelCommand(DriverBridge.COMMAND_STOP_MONITORING, string.Empty);

            btnStart.IsEnabled = true;
            btnStop.IsEnabled = false;
            statusIndicator.Fill = new SolidColorBrush(Color.FromRgb(239, 68, 68));
            lblStatus.Text = "Monitoring Paused | Proteksi OFF";
        }

        private void btnClear_Click(object sender, RoutedEventArgs e)
        {
            allLogs.Clear();
            displayedLogs.Clear();
            blockedCount = 0;
            lblBlockedCount.Text = "0";
            lblEventCount.Text = "0";
        }

        private void txtFilter_TextChanged(object sender, TextChangedEventArgs e)
        {
            string query = txtFilter.Text.Trim().ToLower();
            displayedLogs.Clear();

            foreach (var item in allLogs)
            {
                if (string.IsNullOrEmpty(query) ||
                    item.Process.ToLower().Contains(query) ||
                    item.Name.ToLower().Contains(query) ||
                    item.PID.Contains(query))
                {
                    displayedLogs.Add(item);
                }
            }
        }

        private void DirectKernelPortListener(CancellationToken token)
        {
            int outBufferSize = 64 * 1024;
            IntPtr outBuffer = Marshal.AllocHGlobal(outBufferSize);

            IntPtr pCmd = Marshal.AllocHGlobal(sizeof(uint));
            Marshal.WriteInt32(pCmd, (int)DriverBridge.GetMiniSpyLog);

            try
            {
                while (isMonitoring && !token.IsCancellationRequested)
                {
                    uint bytesReturned = 0;
                    int hResult = DriverBridge.FilterSendMessage(
                        hPort, pCmd, sizeof(uint), outBuffer, (uint)outBufferSize, out bytesReturned);

                    if (hResult == 0 && bytesReturned > 0)
                    {
                        int offset = 0;

                        while (offset < bytesReturned)
                        {
                            IntPtr recordBase = new IntPtr(outBuffer.ToInt64() + offset);

                            int recordLength = Marshal.ReadInt32(recordBase, 0);
                            if (recordLength <= 0 || (offset + recordLength) > bytesReturned)
                                break;

                            // ===== Parsing record dengan OFFSET TETAP (layout LOG_RECORD, build x64) =====
                            // Header 16 byte (Length/SequenceNumber/RecordType/Reserved) + RECORD_DATA.
                            // Offset dihitung dari AWAL record:
                            const int OFF_PROCESS_NAME = 56;   // CHAR[64]  ProcessName (ANSI)
                            const int OFF_PROCESS_ID   = 120;  // FILE_ID   ProcessId   (8 byte)
                            const int OFF_STATUS       = 144;  // NTSTATUS  Status      (4 byte)
                            const int OFF_NAME         = 216;  // WCHAR[]   Name/path file (null-terminated)

                            // 1. PID — langsung dari field ProcessId (bukan lagi dari string "PID|nama")
                            string pidStr = "-";
                            long procId = Marshal.ReadInt64(recordBase, OFF_PROCESS_ID);
                            if (procId > 0 && procId < 0xFFFFFFFF)
                                pidStr = procId.ToString();

                            // 2. Nama proses — ANSI string di offset tetap, potong di NULL pertama
                            string processName = Marshal.PtrToStringAnsi(
                                new IntPtr(recordBase.ToInt64() + OFF_PROCESS_NAME), 64);
                            int pnNul = processName.IndexOf('\0');
                            if (pnNul >= 0) processName = processName.Substring(0, pnNul);
                            if (string.IsNullOrEmpty(processName)) processName = "<unknown>";

                            // 3. Status NTSTATUS — 4 byte di offset tetap
                            uint status = (uint)Marshal.ReadInt32(recordBase, OFF_STATUS);
                            bool isBlocked = (status == 0xC0000022);

                            // 4. Path file target — WCHAR string mulai offset 216, potong di NULL pertama
                            string targetPath = string.Empty;
                            if (recordLength > OFF_NAME)
                            {
                                targetPath = Marshal.PtrToStringUni(
                                    new IntPtr(recordBase.ToInt64() + OFF_NAME),
                                    (recordLength - OFF_NAME) / 2);
                                int tpNul = targetPath.IndexOf('\0');
                                if (tpNul >= 0) targetPath = targetPath.Substring(0, tpNul);
                            }

                            // Konversi NT Path (\Device\HarddiskVolumeX\...) menjadi DOS Path (C:\...)
                            string dosTargetPath = DriverBridge.ConvertNtPathToDosPath(targetPath);

                            string timeStr = DateTime.Now.ToString("HH:mm:ss:fff");
                            string action = isBlocked ? "BLOCKED & TERMINATED" : "MONITORED";

                            var logItem = new LogRecordModel
                            {
                                Time = timeStr,
                                PID = pidStr,
                                Process = processName,
                                Name = dosTargetPath,
                                Status = string.Format("0x{0:X8}", status),
                                ActionTaken = action
                            };

                            Dispatcher.Invoke(() =>
                            {
                                allLogs.Insert(0, logItem);

                                string q = txtFilter.Text.Trim().ToLower();
                                if (string.IsNullOrEmpty(q) ||
                                    logItem.Process.ToLower().Contains(q) ||
                                    logItem.Name.ToLower().Contains(q) ||
                                    logItem.PID.Contains(q))
                                {
                                    displayedLogs.Insert(0, logItem);
                                }

                                if (isBlocked)
                                {
                                    blockedCount++;
                                    lblBlockedCount.Text = blockedCount.ToString();
                                }

                                lblEventCount.Text = allLogs.Count.ToString();
                            });

                            offset += recordLength;
                        }
                    }

                    Thread.Sleep(100);
                }
            }
            finally
            {
                Marshal.FreeHGlobal(outBuffer);
                Marshal.FreeHGlobal(pCmd);
            }
        }

        protected override void OnClosed(EventArgs e)
        {
            isMonitoring = false;
            if (cts != null) cts.Cancel();

            if (hPort != IntPtr.Zero && hPort != new IntPtr(-1))
            {
                DriverBridge.CloseHandle(hPort);
                hPort = IntPtr.Zero;
            }

            base.OnClosed(e);
        }
    }

    public static class DriverBridge
    {
        public const string MINISPY_PORT_NAME = "\\MiniSpyPort";
        public const uint GetMiniSpyLog = 0;
        public const uint COMMAND_CLEAR_TARGETS = 2;
        public const uint COMMAND_ADD_TARGET = 3;
        public const uint COMMAND_START_MONITORING = 4;
        public const uint COMMAND_STOP_MONITORING = 5;

        [DllImport("fltlib.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        public static extern int FilterConnectCommunicationPort(
            string lpPortName,
            uint dwOptions,
            IntPtr lpContext,
            uint dwSizeOfContext,
            IntPtr lpSecurityAttributes,
            out IntPtr hPort);

        [DllImport("fltlib.dll", SetLastError = true)]
        public static extern int FilterSendMessage(
            IntPtr hPort,
            IntPtr lpInBuffer,
            uint dwInBufferSize,
            IntPtr lpOutBuffer,
            uint dwOutBufferSize,
            out uint lpBytesReturned);

        [DllImport("fltlib.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        public static extern int FilterAttach(
            string lpFilterName,
            string lpVolumeName,
            string lpInstanceName,
            uint dwCreatedInstanceNameLength,
            StringBuilder lpCreatedInstanceName);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool CloseHandle(IntPtr hObject);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern uint QueryDosDevice(string lpDeviceName, StringBuilder lpTargetPath, int ucchMax);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct MINISPY_COMMAND_MSG
        {
            public uint Command;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
            public string NameBuffer;
        }

        public static string ConvertDosPathToNtPath(string dosPath)
        {
            if (string.IsNullOrEmpty(dosPath) || dosPath.Length < 2 || dosPath[1] != ':')
                return string.Empty;

            string drive = dosPath.Substring(0, 2);
            StringBuilder sb = new StringBuilder(512);

            if (QueryDosDevice(drive, sb, 512) == 0)
                return string.Empty;

            return sb.ToString() + dosPath.Substring(2);
        }

        public static string ConvertNtPathToDosPath(string ntPath)
        {
            if (string.IsNullOrEmpty(ntPath))
                return string.Empty;

            string[] drives = Directory.GetLogicalDrives();
            StringBuilder sb = new StringBuilder(512);

            foreach (string driveWithSlash in drives)
            {
                string driveLetter = driveWithSlash.TrimEnd('\\');

                if (QueryDosDevice(driveLetter, sb, 512) != 0)
                {
                    string devicePath = sb.ToString();

                    if (ntPath.StartsWith(devicePath, StringComparison.OrdinalIgnoreCase))
                    {
                        return driveLetter + ntPath.Substring(devicePath.Length);
                    }
                }
            }

            return ntPath;
        }
    }
}