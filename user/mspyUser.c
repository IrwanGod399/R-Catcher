/*++

Copyright (c) 1989-2002  Microsoft Corporation

Module Name:

    mspyUser.c

Abstract:

    This file contains the implementation for the main function of the
    user application piece of MiniSpy.  This function is responsible for
    controlling the command mode available to the user to control the
    kernel mode driver.

Environment:

    User mode

--*/

#include <DriverSpecs.h>
_Analysis_mode_(_Analysis_code_type_user_code_)

#include <stdlib.h>
#include <stdio.h>
#include <windows.h>
#include <assert.h>
#include "mspyLog.h"
#include <strsafe.h>
#include <direct.h>

#define SUCCESS              0
#define USAGE_ERROR          1
#define EXIT_INTERPRETER     2
#define EXIT_PROGRAM         4

#define INTERPRETER_EXIT_COMMAND1 "go"
#define INTERPRETER_EXIT_COMMAND2 "g"
#define PROGRAM_EXIT_COMMAND      "exit"
#define CMDLINE_SIZE              256
#define NUM_PARAMS                40

#define MINISPY_NAME            L"MiniSpy"
// =============================================================
// [TAMBAHAN] GLOBAL VARIABLES UNTUK TRACKING FILE
// =============================================================
#define MAX_TRACKED_FILES 500  // Kapasitas maksimal file yang dicatat
#define TRACKER_PATH_LEN 512   // Panjang path

// Array 2D untuk menyimpan path: [Index][StringPath]
WCHAR g_TrackedFiles[MAX_TRACKED_FILES][TRACKER_PATH_LEN];
int g_TrackedCount = 0;

// Fungsi Helper: Mencatat file ke dalam Array
void TrackNewFile(WCHAR* FilePath) {
    if (g_TrackedCount < MAX_TRACKED_FILES) {
        // Salin path ke dalam array global
        if (SUCCEEDED(StringCchCopyW(g_TrackedFiles[g_TrackedCount], TRACKER_PATH_LEN, FilePath))) {
            g_TrackedCount++;
            // Uncomment baris bawah jika ingin debug print setiap file yang dicatat
            // wprintf(L"[TRACKER] File dicatat: %s\n", FilePath);
        }
    }
    else {
        wprintf(L"[TRACKER] Array penuh! Tidak bisa melacak file baru.\n");
    }
}

// Fungsi Helper: Menghapus semua file yang ada di Array
void DeleteTrackedFiles() {
    wprintf(L"\n[CLEANUP] Memulai penghapusan %d file fisik...\n", g_TrackedCount);

    for (int i = 0; i < g_TrackedCount; i++) {
        // Cek apakah file ada
        if (GetFileAttributesW(g_TrackedFiles[i]) != INVALID_FILE_ATTRIBUTES) {
            // Hapus File
            if (DeleteFileW(g_TrackedFiles[i])) {
                wprintf(L"  [OK] Deleted: %s\n", g_TrackedFiles[i]);
            }
            else {
                wprintf(L"  [FAIL] Error %d: %s\n", GetLastError(), g_TrackedFiles[i]);
            }
        }
    }

    // Reset counter setelah dihapus semua
    g_TrackedCount = 0;
    wprintf(L"[CLEANUP] Selesai.\n");
}
// =============================================================



typedef struct _MINISPY_USER_CONTEXT {
    HANDLE Port;       // Handle untuk komunikasi dengan driver
    HANDLE Completion; // Handle untuk I/O completion port
} MINISPY_USER_CONTEXT, * PMINISPY_USER_CONTEXT;
DWORD
InterpretCommand (
    _In_ int argc,
    _In_reads_(argc) char *argv[],
    _In_ PLOG_CONTEXT Context
    );

VOID
ListDevices (
    VOID
    );

VOID
DisplayError (
   _In_ DWORD Code
   )

/*++

Routine Description:

   This routine will display an error message based off of the Win32 error
   code that is passed in. This allows the user to see an understandable
   error message instead of just the code.

Arguments:

   Code - The error code to be translated.

Return Value:

   None.

--*/

{
    WCHAR buffer[MAX_PATH] = { 0 }; 
    DWORD count;
    HMODULE module = NULL;
    HRESULT status;

    count = FormatMessage (FORMAT_MESSAGE_FROM_SYSTEM,
                           NULL,
                           Code,
                           0,
                           buffer,
                           sizeof(buffer) / sizeof(WCHAR),
                           NULL);


    if (count == 0) {

        count = GetSystemDirectory( buffer,
                                    sizeof(buffer) / sizeof( WCHAR ) );

        if (count==0 || count > sizeof(buffer) / sizeof( WCHAR )) {

            //
            //  In practice we expect buffer to be large enough to hold the 
            //  system directory path. 
            //

            printf("    Could not translate error: %d\n", Code);
            return;
        }


        status = StringCchCat( buffer,
                               sizeof(buffer) / sizeof( WCHAR ),
                               L"\\fltlib.dll" );

        if (status != S_OK) {

            printf("    Could not translate error: %d\n", Code);
            return;
        }

        module = LoadLibraryExW( buffer, NULL, LOAD_LIBRARY_AS_DATAFILE );

        //
        //  Translate the Win32 error code into a useful message.
        //

        count = FormatMessage (FORMAT_MESSAGE_FROM_HMODULE,
                               module,
                               Code,
                               0,
                               buffer,
                               sizeof(buffer) / sizeof(WCHAR),
                               NULL);

        if (module != NULL) {

            FreeLibrary( module );
        }

        //
        //  If we still couldn't resolve the message, generate a string
        //

        if (count == 0) {

            printf("    Could not translate error: %d\n", Code);
            return;
        }
    }

    //
    //  Display the translated error.
    //

    printf("    %ws\n", buffer);
}

// Fungsi untuk menyalakan Driver Service secara programatis
BOOL StartDriverService(LPCWSTR ServiceName)
{
    SC_HANDLE hSCManager;
    SC_HANDLE hService;
    SERVICE_STATUS_PROCESS ssStatus;
    DWORD dwBytesNeeded;

    // 1. Buka Service Control Manager
    hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (NULL == hSCManager) {
        printf("Error: Tidak bisa membuka Service Manager. Apakah Anda Run as Administrator?\n");
        return FALSE;
    }

    // 2. Buka Service MiniSpy
    hService = OpenServiceW(hSCManager, ServiceName, SERVICE_START | SERVICE_QUERY_STATUS);
    if (NULL == hService) {
        printf("Error: Tidak bisa membuka service '%ws'. Pastikan driver sudah terinstall (sc create).\n", ServiceName);
        CloseServiceHandle(hSCManager);
        return FALSE;
    }

    // 3. Cek apakah sudah berjalan
    if (QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssStatus, sizeof(SERVICE_STATUS_PROCESS), &dwBytesNeeded)) {
        if (ssStatus.dwCurrentState == SERVICE_RUNNING) {
            printf("Info: Driver '%ws' sudah berjalan.\n", ServiceName);
            CloseServiceHandle(hService);
            CloseServiceHandle(hSCManager);
            return TRUE;
        }
    }

    // 4. Jalankan Service
    printf("Sedang menyalakan driver '%ws'...\n", ServiceName);
    if (!StartService(hService, 0, NULL)) {
        printf("Gagal menjalankan service. Error: %d\n", GetLastError());
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCManager);
        return FALSE;
    }

    printf("Driver berhasil dinyalakan.\n");
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCManager);
    return TRUE;
}


// KODE TAMBAHAN
HRESULT
SendAddTarget(
    _In_ HANDLE Port,
    _In_ LPCWSTR TargetPath
)
{
    MINISPY_COMMAND_MSG msg;
    DWORD bytesReturned;
    HRESULT hResult;

    // Siapkan pesan
    msg.Command = COMMAND_ADD_TARGET;

    // Copy path ke dalam buffer pesan dengan aman
    // Pastikan string NULL terminated
    if (wcscpy_s(msg.NameBuffer, 260, TargetPath) != 0) {
        return E_INVALIDARG;
    }

    printf("Sending target to driver: %ws\n", msg.NameBuffer);

    // Kirim ke driver
    hResult = FilterSendMessage(Port,
        &msg,
        sizeof(MINISPY_COMMAND_MSG),
        NULL,
        0,
        &bytesReturned);
    return hResult;
}

HRESULT
SendClearTargets(
    _In_ HANDLE Port
)
{
    MINISPY_COMMAND_MSG msg;
    DWORD bytesReturned;

    msg.Command = COMMAND_CLEAR_TARGETS;
    // NameBuffer tidak dipakai untuk command ini, biarkan saja

    printf("Sending CLEAR command to driver...\n");

    return FilterSendMessage(Port,
        &msg,
        sizeof(MINISPY_COMMAND_MSG),
        NULL,
        0,
        &bytesReturned);
}
//
//  Main uses a loop which has an assignment in the while 
//  conditional statement. Suppress the compiler's warning.
//

#pragma warning(push)
#pragma warning(disable:4706) // assignment within conditional expression

int _cdecl
main (
    _In_ int argc,
    _In_reads_(argc) char *argv[]
    )
/*++

Routine Description:

    Main routine for minispy

Arguments:

Return Value:

--*/
{
    HANDLE port = INVALID_HANDLE_VALUE;
    HRESULT hResult = S_OK;
    DWORD result;
    ULONG threadId;
    HANDLE thread = NULL;
    LOG_CONTEXT context;
    CHAR inputChar;


    // Mulai Driver

    if (!StartDriverService(L"MiniSpy")) {
        printf("PERINGATAN: Gagal mengelola driver. Aplikasi mungkin gagal connect.\n");
    }

    //
    //  Initialize handle in case of error
    //

    context.ShutDown = NULL;

    //
    //  Open the port that is used to talk to
    //  MiniSpy.
    //

    printf( "Connecting to filter's port...\n" );

    hResult = FilterConnectCommunicationPort( MINISPY_PORT_NAME,
                                              0,
                                              NULL,
                                              0,
                                              NULL,
                                              &port );

    if (IS_ERROR( hResult )) {

        printf( "Could not connect to filter: 0x%08x\n", hResult );
        DisplayError( hResult );
        goto Main_Exit;
    }

    //
    // Initialize the fields of the LOG_CONTEXT
    //

    context.Port = port;
    context.ShutDown = CreateSemaphore( NULL,
                                        0,
                                        1,
                                        L"MiniSpy shut down" );
    context.CleaningUp = FALSE;
    context.LogToFile = FALSE;
    context.LogToScreen = FALSE;        //don't start logging yet
    context.NextLogToScreen = TRUE;
    context.OutputFile = NULL;

    if (context.ShutDown == NULL) {

        result = GetLastError();
        printf( "Could not create semaphore: %d\n", result );
        DisplayError( result );
        goto Main_Exit;
    }

    //
    // Check the valid parameters for startup
    //

    if (argc > 1) {

        if (InterpretCommand( argc - 1, &(argv[1]), &context ) == USAGE_ERROR) {

            goto Main_Exit;
        }
    }

    //
    // Create the thread to read the log records that are gathered
    // by MiniSpy.sys.
    //
    printf( "Creating logging thread...\n" );
    thread = CreateThread( NULL,
                           0,
                           RetrieveLogRecords,
                           (LPVOID)&context,
                           0,
                           &threadId);

    if (!thread) {

        result = GetLastError();
        printf( "Could not create logging thread: %d\n", result );
        DisplayError( result );
        goto Main_Exit;
    }

    //
    // Check to see what devices we are attached to from
    // previous runs of this program.
    //

    ListDevices();

    //
    //  Process commands from the user
    //

    printf( "\nHit [Enter] to begin command mode...\n\n" );
    fflush( stdout );

    //
    //  set screen logging state
    //

    context.LogToScreen = context.NextLogToScreen;

    while (inputChar = (CHAR)getchar()) {

        CHAR *parms[NUM_PARAMS];
        CHAR commandLine[CMDLINE_SIZE+1];
        INT parmCount, count;
        DWORD returnValue = SUCCESS;
        BOOL newParm;
        CHAR ch;

        if (inputChar == '\n') {

            //
            // Start command interpreter.  First we must turn off logging
            // to screen if we are.  Also, remember the state of logging
            // to the screen, so that we can reinstate that when command
            // interpreter is finished.
            //

            context.NextLogToScreen = context.LogToScreen;
            context.LogToScreen = FALSE;

            while (returnValue != EXIT_INTERPRETER) {

                //
                // Print prompt
                //
                printf( ">" );

                //
                // Read in next line, keeping track of the number of parameters
                // as we go.
                //

                parmCount = 0;
                newParm = TRUE;
                for ( count = 0;
                      (count < CMDLINE_SIZE) && ((ch = (CHAR)getchar()) != '\n');
                      count++)
                {
                    commandLine[count] = ch;

                    if (newParm && (ch != ' ')) {

                        parms[parmCount++] = &commandLine[count];
                    }

                    if (parmCount >= NUM_PARAMS) {

                        break;
                    }

                    //
                    //  Always insert NULL's for spaces
                    //

                    if (ch == ' ') {

                        newParm = TRUE;
                        commandLine[count] = 0;

                    } else {

                        newParm = FALSE;
                    }
                }

                commandLine[count] = '\0';

                if (parmCount == 0) {

                    continue;
                }

                //
                // We've got our parameter count and parameter list, so
                // send it off to be interpreted.
                //

                returnValue = InterpretCommand( parmCount, parms, &context );

                if (returnValue == EXIT_PROGRAM) {

                    // Time to stop the program
                    goto Main_Cleanup;
                }
            }

            //
            // Set LogToScreen appropriately based on any commands seen
            //

            context.LogToScreen = context.NextLogToScreen;

            if (context.LogToScreen) {

                printf( "Should be logging to screen...\n" );
            }
        }
    }

Main_Cleanup:

    //
    // Clean up the threads, then fall through to Main_Exit
    //

    printf( "Cleaning up...\n" );

    //
    // Set the Cleaning up flag to TRUE to notify other threads
    // that we are cleaning up
    //
    context.CleaningUp = TRUE;

    //
    // Wait for everyone to shut down
    //

    WaitForSingleObject( context.ShutDown, INFINITE );

    if (context.LogToFile) {

        fclose( context.OutputFile );
    }

Main_Exit:

    //
    // Clean up the data that is always around and exit
    //

    if(context.ShutDown) {

        CloseHandle( context.ShutDown );
    }

    if (thread) {

        CloseHandle( thread );
    }

    if (INVALID_HANDLE_VALUE != port) {
        CloseHandle( port );
    }
    return 0;
}

#pragma warning(pop)
// Fungsi Helper: Mengubah "C:\File.txt" menjadi "\Device\HarddiskVolume2\File.txt"
BOOL ConvertDosPathToNtPath(LPCWSTR DosPath, LPWSTR NtPathBuffer, DWORD BufferSizeInChars)
{
    WCHAR driveLetter[3]; // Untuk menyimpan "C:"
    WCHAR devicePath[MAX_PATH];
    LPCWSTR remainingPath;

    // 1. Cek format path (Harus ada Drive Letter, misal "C:\")
    if (wcslen(DosPath) < 3 || DosPath[1] != L':') {
        printf("Error: Format path harus ada drive letter (contoh: C:\\File.txt)\n");
        return FALSE;
    }

    // 2. Ambil Drive Letternya saja (misal "C:")
    driveLetter[0] = DosPath[0];
    driveLetter[1] = DosPath[1];
    driveLetter[2] = L'\0';

    // 3. Tanya Windows: "C:" itu device aslinya apa?
    // Hasilnya akan masuk ke devicePath (misal: "\Device\HarddiskVolume2")
    if (QueryDosDeviceW(driveLetter, devicePath, MAX_PATH) == 0) {
        printf("Error: Gagal resolve drive letter %ws (Error: %d)\n", driveLetter, GetLastError());
        return FALSE;
    }

    // 4. Gabungkan Device Name + Sisa Path
    // DosPath[2] adalah backslash pertama ('\') setelah titik dua
    remainingPath = &DosPath[2];

    // Format final: "\Device\HarddiskVolume2" + "\Folder\File.txt"
    swprintf_s(NtPathBuffer, BufferSizeInChars, L"%s%s", devicePath, remainingPath);

    return TRUE;
}

// Daftar ekstensi yang ingin dibuat
const WCHAR* extensions[] = { L".log", L".bin", L".exe", L".docx", L".pdf", L".txt" };
// Daftar nama folder
const WCHAR* folders[] = { L"1", L"z" };

// --- Update Fungsi Ini ---
// Pastikan definisi ini ada atau sesuai dengan yang ada di file Anda
// Biasanya PLOG_CONTEXT sudah didefinisikan di struct user.c asli

void GenerateAndRegisterHoneyfiles(PLOG_CONTEXT Context, LPCWSTR TargetFolder) {
    WCHAR fullFolderPath[MAX_PATH];
    WCHAR fullFilePath[MAX_PATH];
    WCHAR ntPathBuffer[512];
    HRESULT hResult;
    HANDLE hFile;
    int mkDirResult;
    DWORD bytesWritten;

    // Buffer data dummy (Misal 2 KB)
    char dummyData[2048];

    // --- LOGIKA ISIAN DATA (REQUEST ANDA) ---
    // Mengisi buffer dengan pola byte: 0x00, 0x01, ... 0xFE, 0x00 ...
    // Ini membuat file terlihat memiliki struktur data (bukan kosong/text biasa)
    for (int x = 0; x < sizeof(dummyData); x++) {
        dummyData[x] = (char)(x % 255);
    }
    // ----------------------------------------

    // Nama-nama file yang "menggoda"
    const WCHAR* fileNames[] = { L"passwords", L"laporan_keuangan", L"gaji_karyawan", L"private_key", L"backup_db" };
    // Ekstensi file
    const WCHAR* targetExtensions[] = { L".log", L".bin", L".docx", L".pdf", L".xlsx" };
    // Subfolder wajib
    const WCHAR* targetSubFolders[] = { L"1", L"z" };

    wprintf(L"\n[INFO] Membuat Honeyfiles di: %s\n", TargetFolder);

    // 1. Buat Root Folder
    mkDirResult = _wmkdir(TargetFolder);

    // 2. Loop Sub-Folder (1 & z)
    for (int i = 0; i < ARRAYSIZE(targetSubFolders); i++) {

        swprintf_s(fullFolderPath, MAX_PATH, L"%s\\%s", TargetFolder, targetSubFolders[i]);

        // Buat Folder
        mkDirResult = _wmkdir(fullFolderPath);
        if (mkDirResult != 0 && errno != EEXIST) {
            wprintf(L"  [!] Gagal folder: %s\n", fullFolderPath);
            continue;
        }

        // 3. Loop Variasi Nama File
        for (int k = 0; k < ARRAYSIZE(fileNames); k++) {

            // 4. Loop Ekstensi
            for (int j = 0; j < ARRAYSIZE(targetExtensions); j++) {

                // Format: Path\Folder\NamaFile.Ext
                swprintf_s(fullFilePath, MAX_PATH, L"%s\\%s%s",
                    fullFolderPath, fileNames[k], targetExtensions[j]);

                // Buat File
                hFile = CreateFileW(fullFilePath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

                if (hFile != INVALID_HANDLE_VALUE) {

                    // --- TULIS DATA BERPOLA KE FILE ---
                    if (!WriteFile(hFile, dummyData, sizeof(dummyData), &bytesWritten, NULL)) {
                        wprintf(L"      [!] Gagal tulis data.\n");
                    }
                    // ----------------------------------

                    CloseHandle(hFile);

                    TrackNewFile(fullFilePath);
                    
                    // Register ke Driver
                    if (ConvertDosPathToNtPath(fullFilePath, ntPathBuffer, 512)) {
                        hResult = SendAddTarget(Context->Port, ntPathBuffer);

                        if (IS_ERROR(hResult)) {
                            wprintf(L"      [x] Gagal register: %s%s\n", fileNames[k], targetExtensions[j]);
                        }
                        else {
                            // Print status sukses (opsional, bisa dikomentari agar tidak spam console)
                            wprintf(L"      [v] %s%s (Size: %d bytes)\n", fileNames[k], targetExtensions[j], bytesWritten);
                        }
                    }
                }
            }
        }
    }
    wprintf(L"[INFO] Selesai. Semua file memiliki isi data dummy.\n\n");
}
DWORD
InterpretCommand (
    _In_ int argc,
    _In_reads_(argc) char *argv[],
    _In_ PLOG_CONTEXT Context
    )
/*++

Routine Description:

    Process options from the user

Arguments:

Return Value:

--*/
{
    LONG parmIndex;
    PCHAR parm;
    HRESULT hResult;
    DWORD returnValue = SUCCESS;
    CHAR buffer[BUFFER_SIZE];
    DWORD bufferLength;
    PWCHAR instanceString;
    WCHAR instanceName[INSTANCE_NAME_MAX_CHARS + 1];

    //
    // Interpret the command line parameters
    //
    for (parmIndex = 0; parmIndex < argc; parmIndex++) {

        parm = argv[parmIndex];

        if (parm[0] == '/') {

            //
            // Have the beginning of a switch
            //

            switch (parm[1]) {
                case 'c': 
            case 'C':
                // Perintah: /c (Clear Targets)
                hResult = SendClearTargets( Context->Port );
                if (IS_ERROR( hResult )) {
                    printf( "Gagal menghapus target: 0x%08x\n", hResult );
                } else {
                    printf( "Sukses menghapus semua target.\n" );
                    DeleteTrackedFiles();
                }
                break;
            case 'g':
            case 'G':
                // Perintah: /g <path folder tujuan>
                // Contoh: /g C:\Users\Admin\Documents\Honey
                parmIndex++;

                if (parmIndex >= argc) {
                    printf("Error: Masukkan path folder tujuan untuk digenerate!\n");
                    printf("Usage: /g <path>\n");
                    // Pastikan label ini ada di kode anda, atau gunakan break;
                    // goto InterpretCommand_Usage; 
                    break;
                }

                parm = argv[parmIndex];

                // 1. Konversi input user (char*) ke Wide String (WCHAR*)
                // Kita gunakan buffer yang sama dengan case 't'
                bufferLength = MultiByteToWideChar(CP_ACP,
                    MB_ERR_INVALID_CHARS,
                    parm,
                    -1,
                    (LPWSTR)buffer,
                    BUFFER_SIZE / sizeof(WCHAR));

                if (bufferLength > 0) {
                    // Panggil fungsi generator dengan path yang sudah dikonversi
                    g_TrackedCount = 0;
                    GenerateAndRegisterHoneyfiles(Context, (LPWSTR)buffer);
                }
                else {
                    printf("Error konversi string path.\n");
                }
                break;
            case 't':
            case 'T':
                // Perintah: /t <path file>
                parmIndex++;

                if (parmIndex >= argc) {
                    printf("Error: Masukkan path file target!\n");
                    goto InterpretCommand_Usage;
                }

                parm = argv[parmIndex];

                // 1. Konversi input user (char*) ke Wide String (WCHAR*)
                // Simpan sementara di variabel 'buffer' (Dos Path)
                bufferLength = MultiByteToWideChar(CP_ACP,
                    MB_ERR_INVALID_CHARS,
                    parm,
                    -1,
                    (LPWSTR)buffer,
                    BUFFER_SIZE / sizeof(WCHAR));

                if (bufferLength > 0) {
                    WCHAR ntPathBuffer[512]; // Buffer untuk hasil konversi

                    // 2. Lakukan Konversi C: -> \Device\HarddiskVolumeX
                    if (ConvertDosPathToNtPath((LPWSTR)buffer, ntPathBuffer, 512)) {

                        printf("Original Path: %ws\n", (LPWSTR)buffer);
                        printf("Translated Path: %ws\n", ntPathBuffer);

                        // 3. Kirim PATH HASIL TERJEMAHAN ke Driver
                        hResult = SendAddTarget(Context->Port, ntPathBuffer);

                        if (IS_ERROR(hResult)) {
                            printf("Gagal mengirim target: 0x%08x\n", hResult);
                        }
                        else {
                            printf("Target berhasil ditambahkan ke Kernel.\n");
                        }
                    }
                }
                else {
                    printf("Error konversi string.\n");
                }
                break;

            case 'a':
            case 'A':

                //
                // Attach to the specified drive letter
                //

                parmIndex++;

                if (parmIndex >= argc) {

                    //
                    // Not enough parameters
                    //

                    goto InterpretCommand_Usage;
                }

                parm = argv[parmIndex];

                printf( "    Attaching to %s... ", parm );

                bufferLength = MultiByteToWideChar( CP_ACP,
                                                    MB_ERR_INVALID_CHARS,
                                                    parm,
                                                    -1,
                                                    (LPWSTR)buffer,
                                                    BUFFER_SIZE/sizeof( WCHAR ) );

                if (bufferLength == 0) {

                    //
                    //  We do not expect the user to provide a parm that
                    //  causes buffer to overflow. 
                    //

                    goto InterpretCommand_Usage;
                }

                hResult = FilterAttach( MINISPY_NAME,
                                        (PWSTR)buffer,
                                        NULL, // instance name
                                        sizeof( instanceName ),
                                        instanceName );

                if (SUCCEEDED( hResult )) {

                    printf( "    Instance name: %S\n", instanceName );

                } else {

                    printf( "\n    Could not attach to device: 0x%08x\n", hResult );
                    DisplayError( hResult );
                    returnValue = SUCCESS;
                }

                break;

            case 'd':
            case 'D':

                //
                // Detach to the specified drive letter
                //

                parmIndex++;

                if (parmIndex >= argc) {

                    //
                    // Not enough parameters
                    //

                    goto InterpretCommand_Usage;
                }

                parm = argv[parmIndex];

                printf( "    Detaching from %s\n", parm );
                bufferLength = MultiByteToWideChar( CP_ACP,
                                                    MB_ERR_INVALID_CHARS,
                                                    parm,
                                                    -1,
                                                    (LPWSTR)buffer,
                                                    BUFFER_SIZE/sizeof( WCHAR ) );

                if (bufferLength == 0) {

                    //
                    //  We do not expect the user to provide a parm that
                    //  causes buffer to overflow.
                    //

                    goto InterpretCommand_Usage; 
                }

                //
                //  Get the next argument to see if it is an InstanceId
                //

                parmIndex++;

                if (parmIndex >= argc) {

                    instanceString = NULL;

                } else {

                    if (argv[parmIndex][0] == '/') {

                        //
                        //  This is just the next command, so don't
                        //  internet it as the InstanceId.
                        //

                        instanceString = NULL;
                        parmIndex--;

                    } else {

                        parm = argv[parmIndex];
                        bufferLength = MultiByteToWideChar( CP_ACP,
                                                            MB_ERR_INVALID_CHARS,
                                                            parm,
                                                            -1,
                                                            (LPWSTR)instanceName,
                                                            sizeof( instanceName )/sizeof( WCHAR ) );

                        if (bufferLength == 0) {

                            //
                            //  We do not expect the user to provide a parm that
                            //  causes buffer to overflow.
                            //

                            goto InterpretCommand_Usage;
                        }

                        instanceString = instanceName;
                    }
                }

                //
                //  Detach from the volume and instance specified.
                //

                hResult = FilterDetach( MINISPY_NAME,
                                        (PWSTR)buffer,
                                        instanceString );

                if (IS_ERROR( hResult )) {

                    printf( "    Could not detach from device: 0x%08x\n", hResult );
                    DisplayError( hResult );
                    returnValue = SUCCESS;
                }
                break;

            case 'l':
            case 'L':

                //
                // List all devices that are currently being monitored
                //

                ListDevices();
                break;

            case 's':
            case 'S':

                //
                // Output logging results to screen, save new value to
                // instate when command interpreter is exited.
                //
                if (Context->NextLogToScreen) {

                    printf( "    Turning off logging to screen\n" );

                } else {

                    printf( "    Turning on logging to screen\n" );
                }

                Context->NextLogToScreen = !Context->NextLogToScreen;
                break;

            case 'f':
            case 'F':

                //
                // Output logging results to file
                //

                if (Context->LogToFile) {

                    printf( "    Stop logging to file \n" );
                    Context->LogToFile = FALSE;
                    assert( Context->OutputFile );
                    _Analysis_assume_( Context->OutputFile != NULL );
                    fclose( Context->OutputFile );
                    Context->OutputFile = NULL;

                } else {

                    parmIndex++;

                    if (parmIndex >= argc) {

                        //
                        // Not enough parameters
                        //

                        goto InterpretCommand_Usage;
                    }

                    parm = argv[parmIndex];
                    printf( "    Log to file %s\n", parm );
                    
                    if (fopen_s( &Context->OutputFile, parm, "w" ) != 0 ) {
                        assert( Context->OutputFile );
                    }
                    
                    Context->LogToFile = TRUE;
                }
                break;

            default:

                //
                // Invalid switch, goto usage
                //
                goto InterpretCommand_Usage;
            }

        } else {

            //
            // Look for "go" or "g" to see if we should exit interpreter
            //

            if (!_strnicmp( parm,
                            INTERPRETER_EXIT_COMMAND1,
                            sizeof( INTERPRETER_EXIT_COMMAND1 ))) {

                returnValue = EXIT_INTERPRETER;
                goto InterpretCommand_Exit;
            }

            if (!_strnicmp( parm,
                            INTERPRETER_EXIT_COMMAND2,
                            sizeof( INTERPRETER_EXIT_COMMAND2 ))) {

                returnValue = EXIT_INTERPRETER;
                goto InterpretCommand_Exit;
            }

            //
            // Look for "exit" to see if we should exit program
            //

            if (!_strnicmp( parm,
                            PROGRAM_EXIT_COMMAND,
                            sizeof( PROGRAM_EXIT_COMMAND ))) {

                returnValue = EXIT_PROGRAM;
                goto InterpretCommand_Exit;
            }

            //
            // Invalid parameter
            //
            goto InterpretCommand_Usage;
        }
    }

InterpretCommand_Exit:
    return returnValue;

InterpretCommand_Usage:
    printf("Valid switches: [/a <drive>] [/d <drive>] [/l] [/s] [/f [<file name>]]\n"
           "    [/a <drive>] starts monitoring <drive>\n"
           "    [/d <drive> [<instance id>]] detaches filter <instance id> from <drive>\n"
           "    [/l] lists all the drives the monitor is currently attached to\n"
           "    [/s] turns on and off showing logging output on the screen\n"
           "    [/f [<file name>]] turns on and off logging to the specified file\n"
           "  If you are in command mode:\n"
           "    [enter] will enter command mode\n"
           "    [go|g] will exit command mode\n"
           "    [exit] will terminate this program\n"
           );
    returnValue = USAGE_ERROR;
    goto InterpretCommand_Exit;
}


ULONG
IsAttachedToVolume(
    _In_ LPCWSTR VolumeName
    )
/*++

Routine Description:

    Determine if our filter is attached to this volume

Arguments:

    VolumeName - The volume we are checking

Return Value:

    TRUE - we are attached
    FALSE - we are not attached (or we couldn't tell)

--*/
{
    PWCHAR filtername;
    CHAR buffer[1024];
    PINSTANCE_FULL_INFORMATION data = (PINSTANCE_FULL_INFORMATION)buffer;
    HANDLE volumeIterator = INVALID_HANDLE_VALUE;
    ULONG bytesReturned;
    ULONG instanceCount = 0;
    HRESULT hResult;

    //
    //  Enumerate all instances on this volume
    //

    hResult = FilterVolumeInstanceFindFirst( VolumeName,
                                             InstanceFullInformation,
                                             data,
                                             sizeof(buffer)-sizeof(WCHAR),
                                             &bytesReturned,
                                             &volumeIterator );

    if (IS_ERROR( hResult )) {

        return instanceCount;
    }

    do {

        assert((data->FilterNameBufferOffset+data->FilterNameLength) <= (sizeof(buffer)-sizeof(WCHAR)));
        _Analysis_assume_((data->FilterNameBufferOffset+data->FilterNameLength) <= (sizeof(buffer)-sizeof(WCHAR)));

        //
        //  Get the name.  Note that we are NULL terminating the buffer
        //  in place.  We can do this because we don't care about the other
        //  information and we have guaranteed that there is room for a NULL
        //  at the end of the buffer.
        //


        filtername = Add2Ptr(data,data->FilterNameBufferOffset);
        filtername[data->FilterNameLength/sizeof( WCHAR )] = L'\0';

        //
        //  Bump the instance count when we find a match
        //

        if (_wcsicmp(filtername,MINISPY_NAME) == 0) {

            instanceCount++;
        }

    } while (SUCCEEDED( FilterVolumeInstanceFindNext( volumeIterator,
                                                                  InstanceFullInformation,
                                                                  data,
                                                                  sizeof(buffer)-sizeof(WCHAR),
                                                                  &bytesReturned ) ));

    //
    //  Close the handle
    //

    FilterVolumeInstanceFindClose( volumeIterator );
    return instanceCount;
}


void
ListDevices(
    VOID
    )
/*++

Routine Description:

    Display the volumes we are attached to

Arguments:

Return Value:

--*/
{
    UCHAR buffer[1024];
    PFILTER_VOLUME_BASIC_INFORMATION volumeBuffer = (PFILTER_VOLUME_BASIC_INFORMATION)buffer;
    HANDLE volumeIterator = INVALID_HANDLE_VALUE;
    ULONG volumeBytesReturned;
    HRESULT hResult = S_OK;
    WCHAR driveLetter[15] = { 0 };
    ULONG instanceCount;

    try {

        //
        //  Find out size of buffer needed
        //

        hResult = FilterVolumeFindFirst( FilterVolumeBasicInformation,
                                         volumeBuffer,
                                         sizeof(buffer)-sizeof(WCHAR),   //save space to null terminate name
                                         &volumeBytesReturned,
                                         &volumeIterator );

        if (IS_ERROR( hResult )) {

             leave;
        }

        assert( INVALID_HANDLE_VALUE != volumeIterator );

        //
        //  Output the header
        //

        printf( "\n"
                "Dos Name        Volume Name                            Status \n"
                "--------------  ------------------------------------  --------\n" );

        //
        //  Loop through all of the filters, displaying instance information
        //

        do {

            assert((FIELD_OFFSET(FILTER_VOLUME_BASIC_INFORMATION,FilterVolumeName) + volumeBuffer->FilterVolumeNameLength) <= (sizeof(buffer)-sizeof(WCHAR)));
            _Analysis_assume_((FIELD_OFFSET(FILTER_VOLUME_BASIC_INFORMATION,FilterVolumeName) + volumeBuffer->FilterVolumeNameLength) <= (sizeof(buffer)-sizeof(WCHAR)));

            volumeBuffer->FilterVolumeName[volumeBuffer->FilterVolumeNameLength/sizeof( WCHAR )] = UNICODE_NULL;

            instanceCount = IsAttachedToVolume(volumeBuffer->FilterVolumeName);

            printf( "%-14ws  %-36ws  %s",
                    (SUCCEEDED( FilterGetDosName(
                                volumeBuffer->FilterVolumeName,
                                driveLetter,
                                sizeof(driveLetter)/sizeof(WCHAR) )) ? driveLetter : L""),
                    volumeBuffer->FilterVolumeName,
                    (instanceCount > 0) ? "Attached" : "");

            if (instanceCount > 1) {

                printf( " (%d)\n", instanceCount );

            } else {

                printf( "\n" );
            }

        } while (SUCCEEDED( hResult = FilterVolumeFindNext( volumeIterator,
                                                                        FilterVolumeBasicInformation,
                                                                        volumeBuffer,
                                                                        sizeof(buffer)-sizeof(WCHAR),    //save space to null terminate name
                                                                        &volumeBytesReturned ) ));

        if (HRESULT_FROM_WIN32( ERROR_NO_MORE_ITEMS ) == hResult) {

            hResult = S_OK;
        }

    } finally {

        if (INVALID_HANDLE_VALUE != volumeIterator) {

            FilterVolumeFindClose( volumeIterator );
        }

        if (IS_ERROR( hResult )) {

            if (HRESULT_FROM_WIN32( ERROR_NO_MORE_ITEMS ) == hResult) {

                printf( "No volumes found.\n" );

            } else {

                printf( "Volume listing failed with error: 0x%08x\n",
                        hResult );
            }
        }
    }
}

