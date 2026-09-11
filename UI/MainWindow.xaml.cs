using System;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
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

        public MainWindow()
        {
            InitializeComponent();
            dgLogs.ItemsSource = displayedLogs;
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

            lblStatus.Text = "Deploying honeyfiles...";

            await Task.Run(() =>
            {
                try
                {
                    string[] folders = { "1", "z" };
                    string[] files = { "passwords", "laporan_keuangan", "backup_db" };
                    string[] exts = { ".docx", ".xlsx", ".pdf" };

                    byte[] dummyData = new byte[1024];

                    if (!Directory.Exists(BAIT_PATH))
                        Directory.CreateDirectory(BAIT_PATH);

                    foreach (var f in folders)
                    {
                        string targetDir = Path.Combine(BAIT_PATH, f);
                        if (!Directory.Exists(targetDir))
                            Directory.CreateDirectory(targetDir);

                        foreach (var name in files)
                        {
                            foreach (var ext in exts)
                            {
                                string fullPath = Path.Combine(targetDir, name + ext);
                                File.WriteAllBytes(fullPath, dummyData);

                                string ntPath = DriverBridge.ConvertDosPathToNtPath(fullPath);
                                if (!string.IsNullOrEmpty(ntPath))
                                {
                                    SendKernelCommand(DriverBridge.COMMAND_ADD_TARGET, ntPath);
                                }
                            }
                        }
                    }
                }
                catch (Exception ex)
                {
                    Dispatcher.Invoke(() => MessageBox.Show("Gagal membuat bait: " + ex.Message));
                }
            });

            lblBaitStatus.Text = "Active (C:\\Honey)";
            lblBaitStatus.Foreground = new SolidColorBrush(Color.FromRgb(16, 185, 129));
            lblStatus.Text = "Honeyfiles deployed & targets set";
        }

        private async void btnCleanBait_Click(object sender, RoutedEventArgs e)
        {
            if (EnsurePortConnected())
            {
                SendKernelCommand(DriverBridge.COMMAND_CLEAR_TARGETS, string.Empty);
            }

            lblStatus.Text = "Cleaning up honeyfiles...";

            await Task.Run(() =>
            {
                try
                {
                    if (Directory.Exists(BAIT_PATH))
                    {
                        Directory.Delete(BAIT_PATH, true);
                    }
                }
                catch { }
            });

            lblBaitStatus.Text = "Cleared";
            lblBaitStatus.Foreground = new SolidColorBrush(Color.FromRgb(239, 68, 68));
            lblStatus.Text = "All bait files deleted & targets cleared";
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

            statusIndicator.Fill = new SolidColorBrush(Color.FromRgb(16, 185, 129));
            lblStatus.Text = "Kernel Port Stream Active";

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

            btnStart.IsEnabled = true;
            btnStop.IsEnabled = false;
            statusIndicator.Fill = new SolidColorBrush(Color.FromRgb(239, 68, 68));
            lblStatus.Text = "Monitoring Paused";
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

                            // 1. Ekstrak Target File Path (Unicode)
                            string targetPath = string.Empty;
                            int nameOffset = -1;

                            for (int n = 120; n < recordLength - 4; n += 2)
                            {
                                char c = (char)Marshal.ReadInt16(recordBase, n);
                                if (c == '\\' || c == 'C' || c == 'D')
                                {
                                    IntPtr testNamePtr = new IntPtr(recordBase.ToInt64() + n);
                                    string candidate = Marshal.PtrToStringUni(testNamePtr, (recordLength - n) / 2);
                                    if (!string.IsNullOrEmpty(candidate))
                                    {
                                        int nullPos = candidate.IndexOf('\0');
                                        if (nullPos > 0) candidate = candidate.Substring(0, nullPos);

                                        if (candidate.Contains("\\") || candidate.IndexOf("Honey", StringComparison.OrdinalIgnoreCase) >= 0)
                                        {
                                            targetPath = candidate;
                                            nameOffset = n;
                                            break;
                                        }
                                    }
                                }
                            }

                            if (nameOffset == -1) nameOffset = 144;

                            // 2. Ekstrak Status NTSTATUS
                            uint status = 0;
                            bool isBlocked = false;

                            for (int s = 32; s < nameOffset; s += 4)
                            {
                                uint val = (uint)Marshal.ReadInt32(recordBase, s);
                                if (val == 0xC0000022)
                                {
                                    status = 0xC0000022;
                                    isBlocked = true;
                                    break;
                                }
                            }

                            if (status == 0)
                            {
                                status = (uint)Marshal.ReadInt32(recordBase, 40);
                                if (status == 0) status = (uint)Marshal.ReadInt32(recordBase, 56);
                            }

                            // 3. Ekstrak ProcessName & PID (Format kernel: "PID|ProcessName")
                            string rawProcessInfo = string.Empty;

                            for (int p = 48; p < nameOffset - 4; p++)
                            {
                                byte b = Marshal.ReadByte(recordBase, p);
                                if ((b >= '0' && b <= '9') || (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z'))
                                {
                                    IntPtr strPtr = new IntPtr(recordBase.ToInt64() + p);
                                    string candidate = Marshal.PtrToStringAnsi(strPtr);
                                    if (!string.IsNullOrEmpty(candidate))
                                    {
                                        int nullPos = candidate.IndexOf('\0');
                                        if (nullPos > 0) candidate = candidate.Substring(0, nullPos);

                                        if ((candidate.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) || candidate.Contains("|")) && candidate.Length <= 40)
                                        {
                                            rawProcessInfo = candidate;
                                            break;
                                        }
                                    }
                                }
                            }

                            string pidStr = "-";
                            string processName = "<unknown>";

                            if (!string.IsNullOrEmpty(rawProcessInfo))
                            {
                                if (rawProcessInfo.Contains("|"))
                                {
                                    string[] parts = rawProcessInfo.Split('|');
                                    pidStr = parts[0].Trim();
                                    processName = parts[1].Trim();
                                }
                                else
                                {
                                    processName = rawProcessInfo;
                                }
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