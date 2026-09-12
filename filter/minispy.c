/*++

Copyright (c) 1989-2002  Microsoft Corporation

Module Name:

    MiniSpy.c

Abstract:

    This is the main module for the MiniSpy mini-filter.

Environment:

    Kernel mode

--*/
#include "mspyKern.h"
#include <stdio.h>
#include <ntstrsafe.h>
//
//  Global variables
//


MINISPY_DATA MiniSpyData;
NTSTATUS StatusToBreakOn = 0;

LIST_ENTRY g_TargetListHead;
FAST_MUTEX g_TargetListLock;

// Flag proteksi kernel. FALSE saat driver baru dimuat (belum di-Start dari UI).
volatile BOOLEAN g_MonitoringActive = FALSE;

// Whitelist dinamis (baseline). Node = TARGET_ENTRY (ListEntry + UNICODE_STRING).
LIST_ENTRY g_WhitelistHead;
FAST_MUTEX g_WhitelistLock;

//---------------------------------------------------------------------------
//  Deklarasi untuk enumerasi proses (baseline) di kernel
//---------------------------------------------------------------------------
#define SystemProcessInformation 5

typedef struct _SPY_SYSTEM_PROCESS_INFORMATION {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER Reserved1[3];
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    KPRIORITY BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    // ... field lain tidak dipakai
} SPY_SYSTEM_PROCESS_INFORMATION, * PSPY_SYSTEM_PROCESS_INFORMATION;

NTSTATUS
ZwQuerySystemInformation(
    _In_ ULONG SystemInformationClass,
    _Inout_ PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength
);

//---------------------------------------------------------------------------
//  Function prototypes
//---------------------------------------------------------------------------
DRIVER_INITIALIZE DriverEntry;
NTSTATUS
DriverEntry (
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    );


NTSTATUS
SpyMessage (
    _In_ PVOID ConnectionCookie,
    _In_reads_bytes_opt_(InputBufferSize) PVOID InputBuffer,
    _In_ ULONG InputBufferSize,
    _Out_writes_bytes_to_opt_(OutputBufferSize,*ReturnOutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferSize,
    _Out_ PULONG ReturnOutputBufferLength
    );

NTSTATUS
SpyConnect(
    _In_ PFLT_PORT ClientPort,
    _In_ PVOID ServerPortCookie,
    _In_reads_bytes_(SizeOfContext) PVOID ConnectionContext,
    _In_ ULONG SizeOfContext,
    _Flt_ConnectionCookie_Outptr_ PVOID *ConnectionCookie
    );

VOID
SpyDisconnect(
    _In_opt_ PVOID ConnectionCookie
    );

NTSTATUS
SpyEnlistInTransaction (
    _In_ PCFLT_RELATED_OBJECTS FltObjects
    );

VOID
CoreSentinelWorkItemRoutine(
    _In_ PFLT_GENERIC_WORKITEM FltWorkItem,
    _In_ PVOID FltObjects, // Tidak kita gunakan
    _In_ PVOID Context     // Ini adalah pointer ke struct kita
);
VOID ClearTargetList(VOID);
NTSTATUS AddTargetToList(WCHAR* PathBuffer);
VOID ClearWhitelist(VOID);
NTSTATUS AddWhitelistEntry(_In_ PUNICODE_STRING Path);
VOID CaptureBaseline(VOID);
//---------------------------------------------------------------------------
//  Assign text sections for each routine.
//---------------------------------------------------------------------------

#ifdef ALLOC_PRAGMA
    #pragma alloc_text(INIT, DriverEntry)
    #pragma alloc_text(PAGE, SpyFilterUnload)
    #pragma alloc_text(PAGE, SpyQueryTeardown)
    #pragma alloc_text(PAGE, SpyConnect)
    #pragma alloc_text(PAGE, SpyDisconnect)
    #pragma alloc_text(PAGE, SpyMessage)
#endif


#define SetFlagInterlocked(_ptrFlags,_flagToSet) \
    ((VOID)InterlockedOr(((volatile LONG *)(_ptrFlags)),_flagToSet))
    
//---------------------------------------------------------------------------
//                      ROUTINES
//---------------------------------------------------------------------------

NTSTATUS
DriverEntry (
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
/*++

Routine Description:

    This routine is called when a driver first loads.  Its purpose is to
    initialize global state and then register with FltMgr to start filtering.

Arguments:

    DriverObject - Pointer to driver object created by the system to
        represent this driver.
    RegistryPath - Unicode string identifying where the parameters for this
        driver are located in the registry.

Return Value:

    Status of the operation.

--*/
{
    PSECURITY_DESCRIPTOR sd;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING uniString;
    NTSTATUS status = STATUS_SUCCESS;

    try {

        //
        // Initialize global data structures.
        //

        MiniSpyData.LogSequenceNumber = 0;
        MiniSpyData.MaxRecordsToAllocate = DEFAULT_MAX_RECORDS_TO_ALLOCATE;
        MiniSpyData.RecordsAllocated = 0;
        MiniSpyData.NameQueryMethod = DEFAULT_NAME_QUERY_METHOD;

        MiniSpyData.DriverObject = DriverObject;

        InitializeListHead( &MiniSpyData.OutputBufferList );
        KeInitializeSpinLock( &MiniSpyData.OutputBufferLock );
        InitializeListHead(&g_TargetListHead);
        ExInitializeFastMutex(&g_TargetListLock);
        InitializeListHead(&g_WhitelistHead);
        ExInitializeFastMutex(&g_WhitelistLock);
        ExInitializeNPagedLookasideList( &MiniSpyData.FreeBufferList,
                                         NULL,
                                         NULL,
                                         POOL_NX_ALLOCATION,
                                         RECORD_SIZE,
                                         SPY_TAG,
                                         0 );

#if MINISPY_VISTA

        //
        //  Dynamically import FilterMgr APIs for transaction support
        //

#pragma warning(push)
#pragma warning(disable:4055) // type cast from data pointer to function pointer
        MiniSpyData.PFltSetTransactionContext = (PFLT_SET_TRANSACTION_CONTEXT) FltGetRoutineAddress( "FltSetTransactionContext" );
        MiniSpyData.PFltGetTransactionContext = (PFLT_GET_TRANSACTION_CONTEXT) FltGetRoutineAddress( "FltGetTransactionContext" );
        MiniSpyData.PFltEnlistInTransaction = (PFLT_ENLIST_IN_TRANSACTION) FltGetRoutineAddress( "FltEnlistInTransaction" );
#pragma warning(pop)

#endif

        //
        // Read the custom parameters for MiniSpy from the registry
        //

        SpyReadDriverParameters(RegistryPath);

        //
        //  Now that our global configuration is complete, register with FltMgr.
        //

        status = FltRegisterFilter( DriverObject,
                                    &FilterRegistration,
                                    &MiniSpyData.Filter );

        if (!NT_SUCCESS( status )) {

           leave;
        }


        status  = FltBuildDefaultSecurityDescriptor( &sd,
                                                     FLT_PORT_ALL_ACCESS );

        if (!NT_SUCCESS( status )) {
            leave;
        }

        RtlInitUnicodeString( &uniString, MINISPY_PORT_NAME );

        InitializeObjectAttributes( &oa,
                                    &uniString,
                                    OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                                    NULL,
                                    sd );

        status = FltCreateCommunicationPort( MiniSpyData.Filter,
                                             &MiniSpyData.ServerPort,
                                             &oa,
                                             NULL,
                                             SpyConnect,
                                             SpyDisconnect,
                                             SpyMessage,
                                             1 );

        FltFreeSecurityDescriptor( sd );

        if (!NT_SUCCESS( status )) {
            leave;
        }

        //
        //  We are now ready to start filtering
        //

        status = FltStartFiltering( MiniSpyData.Filter );

    } finally {

        if (!NT_SUCCESS( status ) ) {

             if (NULL != MiniSpyData.ServerPort) {
                 FltCloseCommunicationPort( MiniSpyData.ServerPort );
             }

             if (NULL != MiniSpyData.Filter) {
                 FltUnregisterFilter( MiniSpyData.Filter );
             }

             ExDeleteNPagedLookasideList( &MiniSpyData.FreeBufferList );
        }
    }

    return status;
}

NTSTATUS
SpyConnect(
    _In_ PFLT_PORT ClientPort,
    _In_ PVOID ServerPortCookie,
    _In_reads_bytes_(SizeOfContext) PVOID ConnectionContext,
    _In_ ULONG SizeOfContext,
    _Flt_ConnectionCookie_Outptr_ PVOID *ConnectionCookie
    )
/*++

Routine Description

    This is called when user-mode connects to the server
    port - to establish a connection

Arguments

    ClientPort - This is the pointer to the client port that
        will be used to send messages from the filter.
    ServerPortCookie - unused
    ConnectionContext - unused
    SizeofContext   - unused
    ConnectionCookie - unused

Return Value

    STATUS_SUCCESS - to accept the connection
--*/
{

    PAGED_CODE();

    UNREFERENCED_PARAMETER( ServerPortCookie );
    UNREFERENCED_PARAMETER( ConnectionContext );
    UNREFERENCED_PARAMETER( SizeOfContext);
    UNREFERENCED_PARAMETER( ConnectionCookie );

    FLT_ASSERT( MiniSpyData.ClientPort == NULL );
    MiniSpyData.ClientPort = ClientPort;
    return STATUS_SUCCESS;
}


VOID
SpyDisconnect(
    _In_opt_ PVOID ConnectionCookie
   )
/*++

Routine Description

    This is called when the connection is torn-down. We use it to close our handle to the connection

Arguments

    ConnectionCookie - unused

Return value

    None
--*/
{

    PAGED_CODE();

    UNREFERENCED_PARAMETER( ConnectionCookie );

    //
    //  Close our handle
    //

    FltCloseClientPort( MiniSpyData.Filter, &MiniSpyData.ClientPort );
}

NTSTATUS
SpyFilterUnload (
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
    )
/*++

Routine Description:

    This is called when a request has been made to unload the filter.  Unload
    requests from the Operation System (ex: "sc stop minispy" can not be
    failed.  Other unload requests may be failed.

    You can disallow OS unload request by setting the
    FLTREGFL_DO_NOT_SUPPORT_SERVICE_STOP flag in the FLT_REGISTARTION
    structure.

Arguments:

    Flags - Flags pertinent to this operation

Return Value:

    Always success

--*/
{
    UNREFERENCED_PARAMETER( Flags );

    PAGED_CODE();

    //
    //  Close the server port. This will stop new connections.
    //

    FltCloseCommunicationPort( MiniSpyData.ServerPort );

    FltUnregisterFilter( MiniSpyData.Filter );

    SpyEmptyOutputBufferList();
    ExDeleteNPagedLookasideList( &MiniSpyData.FreeBufferList );

    //  Bebaskan memori target list & whitelist agar tidak bocor saat unload.
    ClearTargetList();
    ClearWhitelist();

    return STATUS_SUCCESS;
}


NTSTATUS
SpyQueryTeardown (
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags
    )
/*++

Routine Description:

    This allows our filter to be manually detached from a volume.

Arguments:

    FltObjects - Contains pointer to relevant objects for this operation.
        Note that the FileObject field will always be NULL.

    Flags - Flags pertinent to this operation

Return Value:

--*/
{
    UNREFERENCED_PARAMETER( FltObjects );
    UNREFERENCED_PARAMETER( Flags );
    PAGED_CODE();
    return STATUS_SUCCESS;
}


NTSTATUS
SpyMessage (
    _In_ PVOID ConnectionCookie,
    _In_reads_bytes_opt_(InputBufferSize) PVOID InputBuffer,
    _In_ ULONG InputBufferSize,
    _Out_writes_bytes_to_opt_(OutputBufferSize,*ReturnOutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferSize,
    _Out_ PULONG ReturnOutputBufferLength
    )
/*++

Routine Description:

    This is called whenever a user mode application wishes to communicate
    with this minifilter.

Arguments:

    ConnectionCookie - unused

    OperationCode - An identifier describing what type of message this
        is.  These codes are defined by the MiniFilter.
    InputBuffer - A buffer containing input data, can be NULL if there
        is no input data.
    InputBufferSize - The size in bytes of the InputBuffer.
    OutputBuffer - A buffer provided by the application that originated
        the communication in which to store data to be returned to this
        application.
    OutputBufferSize - The size in bytes of the OutputBuffer.
    ReturnOutputBufferSize - The size in bytes of meaningful data
        returned in the OutputBuffer.

Return Value:

    Returns the status of processing the message.

--*/
{
    MINISPY_COMMAND command;
    NTSTATUS status;

    PAGED_CODE();

    UNREFERENCED_PARAMETER( ConnectionCookie );

    //
    //                      **** PLEASE READ ****
    //
    //  The INPUT and OUTPUT buffers are raw user mode addresses.  The filter
    //  manager has already done a ProbedForRead (on InputBuffer) and
    //  ProbedForWrite (on OutputBuffer) which guarentees they are valid
    //  addresses based on the access (user mode vs. kernel mode).  The
    //  minifilter does not need to do their own probe.
    //
    //  The filter manager is NOT doing any alignment checking on the pointers.
    //  The minifilter must do this themselves if they care (see below).
    //
    //  The minifilter MUST continue to use a try/except around any access to
    //  these buffers.
    //

    if ((InputBuffer != NULL) &&
        (InputBufferSize >= (FIELD_OFFSET(COMMAND_MESSAGE,Command) +
                             sizeof(MINISPY_COMMAND)))) {

        try  {

            //
            //  Probe and capture input message: the message is raw user mode
            //  buffer, so need to protect with exception handler
            //

            command = ((PCOMMAND_MESSAGE) InputBuffer)->Command;

        } except (SpyExceptionFilter( GetExceptionInformation(), TRUE )) {
        
            return GetExceptionCode();
        }

        switch (command) {
            case COMMAND_CLEAR_TARGETS: // Biasanya didefinisikan sbg 2

                // Panggil fungsi helper yang sudah kita buat sebelumnya
                ClearTargetList();
                DbgPrint("CoreSentinel: Perintah CLEAR diterima dari User.\n");
                status = STATUS_SUCCESS;
                break;

            case COMMAND_ADD_TARGET: // Biasanya didefinisikan sbg 3

                // 1. Cek Validasi Ukuran Buffer
                // Kita butuh buffer sebesar struct MINISPY_COMMAND_MSG
                if (InputBufferSize < sizeof(MINISPY_COMMAND_MSG)) {
                    status = STATUS_BUFFER_TOO_SMALL;
                    break;
                }

                try {
                    // 2. Casting buffer ke struct kita
                    PMINISPY_COMMAND_MSG msg = (PMINISPY_COMMAND_MSG)InputBuffer;

                    // 3. Safety: Paksa karakter terakhir jadi NULL (Mencegah buffer overflow)
                    // Asumsi NameBuffer ukuran 260 WCHAR
                    msg->NameBuffer[259] = L'\0';

                    // 4. Panggil fungsi helper untuk memasukkan ke Linked List
                    // Pastikan fungsi AddTargetToList sudah ada di file ini atau di-include
                    status = AddTargetToList(msg->NameBuffer);

                    if (NT_SUCCESS(status)) {
                        DbgPrint("CoreSentinel: Target baru ditambahkan: %ws\n", msg->NameBuffer);
                    }
                    else {
                        DbgPrint("CoreSentinel: Gagal menambah target. Status: 0x%x\n", status);
                    }

                } except(SpyExceptionFilter(GetExceptionInformation(), TRUE)) {
                    status = GetExceptionCode();
                }
                break;

            case COMMAND_START_MONITORING:
                // Aktifkan proteksi: mulai memblokir/terminate proses yang menyentuh honeyfile.
                g_MonitoringActive = TRUE;
                DbgPrint("CoreSentinel: Monitoring AKTIF (proteksi ON).\n");
                status = STATUS_SUCCESS;
                break;

            case COMMAND_STOP_MONITORING:
                // Nonaktifkan proteksi (pause). Daftar target TIDAK dihapus,
                // jadi Start berikutnya bisa mengaktifkan lagi tanpa Deploy ulang.
                g_MonitoringActive = FALSE;
                DbgPrint("CoreSentinel: Monitoring NONAKTIF (proteksi OFF).\n");
                status = STATUS_SUCCESS;
                break;

            case COMMAND_CLEAR_WHITELIST:
                // Reset baseline sebelum mulai merekam yang baru.
                ClearWhitelist();
                DbgPrint("CoreSentinel: Whitelist di-clear.\n");
                status = STATUS_SUCCESS;
                break;

            case COMMAND_CAPTURE_BASELINE:
                // Enumerasi semua proses aktif -> tambahkan NT path-nya ke whitelist.
                // UI memanggil ini berulang selama periode baseline.
                CaptureBaseline();
                status = STATUS_SUCCESS;
                break;

            case COMMAND_GET_WHITELIST:
            {
                //  Kirim isi whitelist ke UI. Format per entri: [ULONG cbBytes][cbBytes WCHAR path].
                PUCHAR temp;
                ULONG off = 0;
                PLIST_ENTRY we;

                if ((OutputBuffer == NULL) || (OutputBufferSize == 0)) {
                    status = STATUS_INVALID_PARAMETER;
                    break;
                }

                //  Susun ke buffer kernel dulu (di bawah lock), baru copy ke user.
                temp = (PUCHAR)ExAllocatePoolZero(NonPagedPool, OutputBufferSize, 'wlGM');
                if (!temp) {
                    status = STATUS_INSUFFICIENT_RESOURCES;
                    break;
                }

                ExAcquireFastMutex(&g_WhitelistLock);
                we = g_WhitelistHead.Flink;
                while (we != &g_WhitelistHead) {
                    PTARGET_ENTRY wl = CONTAINING_RECORD(we, TARGET_ENTRY, ListEntry);
                    ULONG cb = wl->FileName.Length;
                    if (off + sizeof(ULONG) + cb > OutputBufferSize) {
                        break;  // sisa tidak muat, hentikan
                    }
                    RtlCopyMemory(temp + off, &cb, sizeof(ULONG));
                    off += sizeof(ULONG);
                    RtlCopyMemory(temp + off, wl->FileName.Buffer, cb);
                    off += cb;
                    we = we->Flink;
                }
                ExReleaseFastMutex(&g_WhitelistLock);

                try {
                    RtlCopyMemory(OutputBuffer, temp, off);
                    *ReturnOutputBufferLength = off;
                    status = STATUS_SUCCESS;
                } except(SpyExceptionFilter(GetExceptionInformation(), TRUE)) {
                    status = GetExceptionCode();
                }

                ExFreePool(temp);
                break;
            }

            case GetMiniSpyLog:

                //
                //  Return as many log records as can fit into the OutputBuffer
                //

                if ((OutputBuffer == NULL) || (OutputBufferSize == 0)) {

                    status = STATUS_INVALID_PARAMETER;
                    break;
                }

                //
                //  We want to validate that the given buffer is POINTER
                //  aligned.  But if this is a 64bit system and we want to
                //  support 32bit applications we need to be careful with how
                //  we do the check.  Note that the way SpyGetLog is written
                //  it actually does not care about alignment but we are
                //  demonstrating how to do this type of check.
                //

#if defined(_WIN64)

                if (IoIs32bitProcess( NULL )) {

                    //
                    //  Validate alignment for the 32bit process on a 64bit
                    //  system
                    //

                    if (!IS_ALIGNED(OutputBuffer,sizeof(ULONG))) {

                        status = STATUS_DATATYPE_MISALIGNMENT;
                        break;
                    }

                } else {

#endif

                    if (!IS_ALIGNED(OutputBuffer,sizeof(PVOID))) {

                        status = STATUS_DATATYPE_MISALIGNMENT;
                        break;
                    }

#if defined(_WIN64)

                }

#endif

                //
                //  Get the log record.
                //

                status = SpyGetLog( OutputBuffer,
                                    OutputBufferSize,
                                    ReturnOutputBufferLength );
                break;


            case GetMiniSpyVersion:

                //
                //  Return version of the MiniSpy filter driver.  Verify
                //  we have a valid user buffer including valid
                //  alignment
                //

                if ((OutputBufferSize < sizeof( MINISPYVER )) ||
                    (OutputBuffer == NULL)) {

                    status = STATUS_INVALID_PARAMETER;
                    break;
                }

                //
                //  Validate Buffer alignment.  If a minifilter cares about
                //  the alignment value of the buffer pointer they must do
                //  this check themselves.  Note that a try/except will not
                //  capture alignment faults.
                //

                if (!IS_ALIGNED(OutputBuffer,sizeof(ULONG))) {

                    status = STATUS_DATATYPE_MISALIGNMENT;
                    break;
                }

                //
                //  Protect access to raw user-mode output buffer with an
                //  exception handler
                //

                try {

                    ((PMINISPYVER)OutputBuffer)->Major = MINISPY_MAJ_VERSION;
                    ((PMINISPYVER)OutputBuffer)->Minor = MINISPY_MIN_VERSION;

                } except (SpyExceptionFilter( GetExceptionInformation(), TRUE )) {

                      return GetExceptionCode();
                }

                *ReturnOutputBufferLength = sizeof( MINISPYVER );
                status = STATUS_SUCCESS;
                break;

            default:
                status = STATUS_INVALID_PARAMETER;
                break;
        }

    } else {

        status = STATUS_INVALID_PARAMETER;
    }

    return status;
}


//---------------------------------------------------------------------------
//              Operation filtering routines
//---------------------------------------------------------------------------
// Fungsi untuk menghapus semua isi list (Reset)
VOID ClearTargetList() {
    PLIST_ENTRY entry;
    PTARGET_ENTRY targetEntry;

    ExAcquireFastMutex(&g_TargetListLock);

    while (!IsListEmpty(&g_TargetListHead)) {
        // Ambil item pertama lalu hapus dari rantai
        entry = RemoveHeadList(&g_TargetListHead);

        // Dapatkan pointer ke struct aslinya
        targetEntry = CONTAINING_RECORD(entry, TARGET_ENTRY, ListEntry);

        // Bebaskan memori string buffer
        if (targetEntry->FileName.Buffer) {
            ExFreePool(targetEntry->FileName.Buffer);
        }

        // Bebaskan memori struct itu sendiri
        ExFreePool(targetEntry);
    }

    ExReleaseFastMutex(&g_TargetListLock);
}

// Fungsi untuk menambah target baru
NTSTATUS AddTargetToList(WCHAR* PathBuffer) {
    PTARGET_ENTRY newEntry;
    SIZE_T strLen;
    SIZE_T strSize;

    strLen = wcslen(PathBuffer);
    strSize = (strLen + 1) * sizeof(WCHAR);

    // [SOLUSI] Gunakan ExAllocatePoolZero
    // Perhatikan: Parameternya kembali menggunakan 'NonPagedPool' (bukan POOL_FLAG_...)
    newEntry = (PTARGET_ENTRY)ExAllocatePoolZero(NonPagedPool, sizeof(TARGET_ENTRY), 'tgT1');

    if (!newEntry) return STATUS_INSUFFICIENT_RESOURCES;

    // Alokasi buffer string
    newEntry->FileName.Buffer = (PWCH)ExAllocatePoolZero(NonPagedPool, strSize, 'tgT2');

    if (!newEntry->FileName.Buffer) {
        ExFreePool(newEntry);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Salin data string
    newEntry->FileName.Length = (USHORT)(strLen * sizeof(WCHAR));
    newEntry->FileName.MaximumLength = (USHORT)strSize;
    RtlCopyMemory(newEntry->FileName.Buffer, PathBuffer, strSize);

    // Masukkan ke Linked List
    ExAcquireFastMutex(&g_TargetListLock);
    InsertTailList(&g_TargetListHead, &newEntry->ListEntry);
    ExReleaseFastMutex(&g_TargetListLock);

    return STATUS_SUCCESS;
}

// =============================================================
// ==            WHITELIST / BASELINE (di kernel)             ==
// =============================================================

// Kosongkan seluruh whitelist.
VOID ClearWhitelist(VOID) {
    PLIST_ENTRY entry;
    PTARGET_ENTRY item;

    ExAcquireFastMutex(&g_WhitelistLock);
    while (!IsListEmpty(&g_WhitelistHead)) {
        entry = RemoveHeadList(&g_WhitelistHead);
        item = CONTAINING_RECORD(entry, TARGET_ENTRY, ListEntry);
        if (item->FileName.Buffer) {
            ExFreePool(item->FileName.Buffer);
        }
        ExFreePool(item);
    }
    ExReleaseFastMutex(&g_WhitelistLock);
}

// Tambah satu NT path ke whitelist (dengan dedup). Aman dipanggil berulang.
NTSTATUS AddWhitelistEntry(_In_ PUNICODE_STRING Path) {
    PTARGET_ENTRY newEntry;
    PLIST_ENTRY e;

    if (Path == NULL || Path->Length == 0 || Path->Buffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    ExAcquireFastMutex(&g_WhitelistLock);

    //  Dedup: kalau path sudah ada, tidak usah ditambah lagi.
    e = g_WhitelistHead.Flink;
    while (e != &g_WhitelistHead) {
        PTARGET_ENTRY w = CONTAINING_RECORD(e, TARGET_ENTRY, ListEntry);
        if (RtlCompareUnicodeString(Path, &w->FileName, TRUE) == 0) {
            ExReleaseFastMutex(&g_WhitelistLock);
            return STATUS_SUCCESS;
        }
        e = e->Flink;
    }

    //  Alokasi node + buffer string (NonPagedPool aman di APC_LEVEL/FAST_MUTEX).
    newEntry = (PTARGET_ENTRY)ExAllocatePoolZero(NonPagedPool, sizeof(TARGET_ENTRY), 'wtT1');
    if (!newEntry) {
        ExReleaseFastMutex(&g_WhitelistLock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    newEntry->FileName.Buffer = (PWCH)ExAllocatePoolZero(NonPagedPool, Path->Length + sizeof(WCHAR), 'wtT2');
    if (!newEntry->FileName.Buffer) {
        ExFreePool(newEntry);
        ExReleaseFastMutex(&g_WhitelistLock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    newEntry->FileName.Length = Path->Length;
    newEntry->FileName.MaximumLength = Path->Length + sizeof(WCHAR);
    RtlCopyMemory(newEntry->FileName.Buffer, Path->Buffer, Path->Length);

    InsertTailList(&g_WhitelistHead, &newEntry->ListEntry);
    ExReleaseFastMutex(&g_WhitelistLock);
    return STATUS_SUCCESS;
}

// Enumerasi SEMUA proses aktif dan masukkan NT path lengkapnya ke whitelist.
// Kernel bisa membaca path proses terproteksi sekalipun (beda dgn userspace).
VOID CaptureBaseline(VOID) {
    NTSTATUS status;
    ULONG bufLen = 0;
    PVOID buffer = NULL;
    PSPY_SYSTEM_PROCESS_INFORMATION spi;

    //  1. Tanya ukuran buffer yang dibutuhkan.
    status = ZwQuerySystemInformation(SystemProcessInformation, NULL, 0, &bufLen);
    if (bufLen == 0) {
        return;
    }
    bufLen += 8192;  // slack untuk proses yang muncul di antara dua panggilan

    buffer = ExAllocatePoolZero(NonPagedPool, bufLen, 'lbSM');
    if (!buffer) {
        return;
    }

    //  2. Ambil daftar proses.
    status = ZwQuerySystemInformation(SystemProcessInformation, buffer, bufLen, &bufLen);
    if (!NT_SUCCESS(status)) {
        ExFreePool(buffer);
        return;
    }

    //  3. Iterasi tiap proses -> ambil NT path lengkap -> masuk whitelist.
    spi = (PSPY_SYSTEM_PROCESS_INFORMATION)buffer;
    for (;;) {
        if (spi->UniqueProcessId != NULL) {
            PEPROCESS proc = NULL;
            if (NT_SUCCESS(PsLookupProcessByProcessId(spi->UniqueProcessId, &proc))) {
                PUNICODE_STRING imgPath = NULL;
                if (NT_SUCCESS(SeLocateProcessImageName(proc, &imgPath)) && imgPath != NULL) {
                    if (imgPath->Length > 0) {
                        AddWhitelistEntry(imgPath);
                    }
                    ExFreePool(imgPath);
                }
                ObDereferenceObject(proc);
            }
        }

        if (spi->NextEntryOffset == 0) {
            break;
        }
        spi = (PSPY_SYSTEM_PROCESS_INFORMATION)((PUCHAR)spi + spi->NextEntryOffset);
    }

    ExFreePool(buffer);
}

FLT_PREOP_CALLBACK_STATUS
#pragma warning(suppress: 6262)
SpyPreOperationCallback(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
)
{
    FLT_PREOP_CALLBACK_STATUS returnStatus = FLT_PREOP_SUCCESS_NO_CALLBACK;
    PRECORD_LIST recordList;
    PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
    PUNICODE_STRING nameToUse;
    NTSTATUS status;

#if MINISPY_VISTA
    PUNICODE_STRING ecpDataToUse = NULL;
    UNICODE_STRING ecpData;
    WCHAR ecpDataBuffer[MAX_NAME_SPACE / sizeof(WCHAR)];
#endif

    UNREFERENCED_PARAMETER(CompletionContext);

    recordList = SpyNewRecord();

    if (recordList) {

        if (FltObjects->FileObject != NULL) {
            status = FltGetFileNameInformation(Data,
                FLT_FILE_NAME_NORMALIZED |
                MiniSpyData.NameQueryMethod,
                &nameInfo);
        }
        else {
            status = STATUS_UNSUCCESSFUL;
        }

        if (NT_SUCCESS(status)) {

            BOOLEAN isTargetFile = FALSE;

            //  Proteksi hanya berjalan bila monitoring AKTIF (di-Start dari UI).
            //  Kalau nonaktif (di-Stop), lewati pengecekan target sepenuhnya sehingga
            //  tidak ada pemblokiran/terminate — daftar target tetap tersimpan.
            if (g_MonitoringActive) {
                ExAcquireFastMutex(&g_TargetListLock);
                if (!IsListEmpty(&g_TargetListHead)) {
                    PLIST_ENTRY entry = g_TargetListHead.Flink;
                    while (entry != &g_TargetListHead) {
                        PTARGET_ENTRY targetItem = CONTAINING_RECORD(entry, TARGET_ENTRY, ListEntry);
                        if (RtlCompareUnicodeString(&nameInfo->Name, &targetItem->FileName, TRUE) == 0) {
                            isTargetFile = TRUE;
                            break;
                        }
                        entry = entry->Flink;
                    }
                }
                ExReleaseFastMutex(&g_TargetListLock);
            }

            if (isTargetFile) {

                HANDLE ProcessId = (HANDLE)FltGetRequestorProcessId(Data);
                PEPROCESS pProcess = NULL;
                NTSTATUS nameStatus;
                BOOLEAN isWhitelisted = FALSE;
                CHAR capturedName[64] = { 0 }; // Penampung nama proses lokal

                nameStatus = PsLookupProcessByProcessId(ProcessId, &pProcess);

                if (NT_SUCCESS(nameStatus)) {

                    PCHAR imageName = (PCHAR)PsGetProcessImageFileName(pProcess);

                    if (imageName) {
                        // Simpan nama proses (untuk log) sebelum proses dimatikan
                        RtlStringCbCopyA(capturedName, sizeof(capturedName), imageName);
                    }

                    //  Whitelist DINAMIS: cocokkan NT PATH LENGKAP proses (mis.
                    //  \Device\HarddiskVolume2\...\chrome.exe) dengan baseline yang direkam.
                    {
                        PUNICODE_STRING procImagePath = NULL;
                        NTSTATUS pathStatus = SeLocateProcessImageName(pProcess, &procImagePath);

                        if (NT_SUCCESS(pathStatus) && procImagePath != NULL && procImagePath->Length > 0) {

                            //  Path bisa di-resolve -> cocokkan ke whitelist.
                            //  Ada di baseline -> tepercaya; tidak ada -> ancaman (di-terminate).
                            ExAcquireFastMutex(&g_WhitelistLock);
                            PLIST_ENTRY we = g_WhitelistHead.Flink;
                            while (we != &g_WhitelistHead) {
                                PTARGET_ENTRY wl = CONTAINING_RECORD(we, TARGET_ENTRY, ListEntry);
                                if (RtlCompareUnicodeString(procImagePath, &wl->FileName, TRUE) == 0) {
                                    isWhitelisted = TRUE;
                                    break;
                                }
                                we = we->Flink;
                            }
                            ExReleaseFastMutex(&g_WhitelistLock);
                        }
                        else {
                            //  Path TIDAK bisa di-resolve: ini proses kernel/System (PID<=4)
                            //  atau I/O paging/cache-flush yang berjalan di konteks System.
                            //  Bukan ransomware user-mode -> ANGGAP TEPERCAYA agar tidak
                            //  salah blokir/terminate (System tidak boleh dimatikan).
                            isWhitelisted = TRUE;
                        }

                        if (procImagePath != NULL) {
                            ExFreePool(procImagePath);
                        }
                    }

                    if (!isWhitelisted) {
                        HANDLE hProcess = NULL;
                        NTSTATUS termStatus = ObOpenObjectByPointer(pProcess, OBJ_KERNEL_HANDLE, NULL, PROCESS_TERMINATE, *PsProcessType, KernelMode, &hProcess);
                        if (NT_SUCCESS(termStatus)) {
                            ZwTerminateProcess(hProcess, STATUS_ACCESS_DENIED);
                            ZwClose(hProcess);
                        }
                    }

                    ObDereferenceObject(pProcess);
                }

                if (isWhitelisted) {
                    FltReleaseFileNameInformation(nameInfo);
                    nameInfo = NULL;
                    SpyFreeRecord(recordList);
                    return FLT_PREOP_SUCCESS_NO_CALLBACK;
                }

                nameToUse = &nameInfo->Name;
                Data->IoStatus.Status = STATUS_ACCESS_DENIED;
                Data->IoStatus.Information = 0;

                if (FlagOn(MiniSpyData.DebugFlags, SPY_DEBUG_PARSE_NAMES)) {
#ifdef DBG
                    FLT_ASSERT(NT_SUCCESS(FltParseFileNameInformation(nameInfo)));
#else
                    FltParseFileNameInformation(nameInfo);
#endif
                }

#if MINISPY_VISTA
                if (Data->Iopb->MajorFunction == IRP_MJ_CREATE) {
                    RtlInitEmptyUnicodeString(&ecpData, ecpDataBuffer, MAX_NAME_SPACE / sizeof(WCHAR));
                    SpyParseEcps(Data, recordList, &ecpData);
                    ecpDataToUse = &ecpData;
                }
                SpySetRecordNameAndEcpData(&(recordList->LogRecord), nameToUse, ecpDataToUse);
#else
                SpySetRecordName(&(recordList->LogRecord), nameToUse);
#endif

                if (NULL != nameInfo) {
                    FltReleaseFileNameInformation(nameInfo);
                    nameInfo = NULL;
                }

                // 1. Eksekusi fungsi bawaan Minispy
                recordList->LogRecord.Data.Status = Data->IoStatus.Status;
                SpyLogPreOperationData(Data, FltObjects, recordList);

                // 2. TITIK UBAH UTAMA: Tulis setelah SpyLogPreOperationData agar TIDAK tertimpa
                // ProcessName cukup berisi NAMA proses saja (mis. "cmd.exe").
                // PID dikirim terpisah lewat field Data.ProcessId di bawah, yang dibaca UI
                // dari offset tetap — jadi tidak perlu lagi format gabungan "PID|nama".
                if (capturedName[0] != '\0') {
                    RtlStringCbCopyA(recordList->LogRecord.Data.ProcessName,
                        sizeof(recordList->LogRecord.Data.ProcessName),
                        capturedName);
                }
                else {
                    RtlStringCbCopyA(recordList->LogRecord.Data.ProcessName,
                        sizeof(recordList->LogRecord.Data.ProcessName),
                        "<NoName>");
                }

                recordList->LogRecord.Data.ProcessId = (ULONG_PTR)ProcessId;
                recordList->LogRecord.Data.ThreadId = (ULONG_PTR)PsGetCurrentThreadId();

                // 3. Kirim data ke user-mode
                if (Data->Iopb->MajorFunction == IRP_MJ_SHUTDOWN) {
                    returnStatus = FLT_PREOP_SUCCESS_NO_CALLBACK;
                }
                else {
                    SpyLog(recordList);
                    returnStatus = FLT_PREOP_COMPLETE;
                }

            }
            else {
                FltReleaseFileNameInformation(nameInfo);
                SpyFreeRecord(recordList);
                returnStatus = FLT_PREOP_SUCCESS_NO_CALLBACK;
            }

        }
        else {
            SpyFreeRecord(recordList);
            returnStatus = FLT_PREOP_SUCCESS_NO_CALLBACK;
        }

    }

    return returnStatus;
}

NTSTATUS
SpyEnlistInTransaction (
    _In_ PCFLT_RELATED_OBJECTS FltObjects
    )
/*++

Routine Description

    Minispy calls this function to enlist in a transaction of interest. 

Arguments

    FltObjects - Contains parameters required to enlist in a transaction.

Return value

    Returns STATUS_SUCCESS if we were able to successfully enlist in a new transcation or if we
    were already enlisted in the transaction. Returns an appropriate error code on a failure.
    
--*/
{

#if MINISPY_VISTA

    PMINISPY_TRANSACTION_CONTEXT transactionContext = NULL;
    PMINISPY_TRANSACTION_CONTEXT oldTransactionContext = NULL;
    PRECORD_LIST recordList;
    NTSTATUS status;
    static ULONG Sequence=1;

    //
    //  This code is only built in the Vista environment, but
    //  we need to ensure this binary still runs down-level.  Return
    //  at this point if the transaction dynamic imports were not found.
    //
    //  If we find FltGetTransactionContext, we assume the other
    //  transaction APIs are also present.
    //

    if (NULL == MiniSpyData.PFltGetTransactionContext) {

        return STATUS_SUCCESS;
    }

    //
    //  Try to get our context for this transaction. If we get
    //  one we have already enlisted in this transaction.
    //

    status = (*MiniSpyData.PFltGetTransactionContext)( FltObjects->Instance,
                                                       FltObjects->Transaction,
                                                       &transactionContext );

    if (NT_SUCCESS( status )) {

        // 
        //  Check if we have already enlisted in the transaction. 
        //

        if (FlagOn(transactionContext->Flags, MINISPY_ENLISTED_IN_TRANSACTION)) {

            //
            //  FltGetTransactionContext puts a reference on the context. Release
            //  that now and return success.
            //
            
            FltReleaseContext( transactionContext );
            return STATUS_SUCCESS;
        }

        //
        //  If we have not enlisted then we need to try and enlist in the transaction.
        //
        
        goto ENLIST_IN_TRANSACTION;
    }

    //
    //  If the context does not exist create a new one, else return the error
    //  status to the caller.
    //

    if (status != STATUS_NOT_FOUND) {

        return status;
    }

    //
    //  Allocate a transaction context.
    //

    status = FltAllocateContext( FltObjects->Filter,
                                 FLT_TRANSACTION_CONTEXT,
                                 sizeof(MINISPY_TRANSACTION_CONTEXT),
                                 PagedPool,
                                 &transactionContext );

    if (!NT_SUCCESS( status )) {

        return status;
    }

    //
    //  Set the context into the transaction
    //

    RtlZeroMemory(transactionContext, sizeof(MINISPY_TRANSACTION_CONTEXT));
    transactionContext->Count = Sequence++;

    FLT_ASSERT( MiniSpyData.PFltSetTransactionContext );

    status = (*MiniSpyData.PFltSetTransactionContext)( FltObjects->Instance,
                                                       FltObjects->Transaction,
                                                       FLT_SET_CONTEXT_KEEP_IF_EXISTS,
                                                       transactionContext,
                                                       &oldTransactionContext );

    if (!NT_SUCCESS( status )) {

        FltReleaseContext( transactionContext );    //this will free the context

        if (status != STATUS_FLT_CONTEXT_ALREADY_DEFINED) {

            return status;
        }

        FLT_ASSERT(oldTransactionContext != NULL);
        
        if (FlagOn(oldTransactionContext->Flags, MINISPY_ENLISTED_IN_TRANSACTION)) {

            //
            //  If this context is already enlisted then release the reference
            //  which FltSetTransactionContext put on it and return success.
            //
            
            FltReleaseContext( oldTransactionContext );
            return STATUS_SUCCESS;
        }

        //
        //  If we found an existing transaction then we should try and
        //  enlist in it. There is a race here in which the thread 
        //  which actually set the transaction context may fail to 
        //  enlist in the transaction and delete it later. It might so
        //  happen that we picked up a reference to that context here
        //  and successfully enlisted in that transaction. For now
        //  we have chosen to ignore this scenario.
        //

        //
        //  If we are not enlisted then assign the right transactionContext
        //  and attempt enlistment.
        //

        transactionContext = oldTransactionContext;            
    }

ENLIST_IN_TRANSACTION: 

    //
    //  Enlist on this transaction for notifications.
    //

    FLT_ASSERT( MiniSpyData.PFltEnlistInTransaction );

    status = (*MiniSpyData.PFltEnlistInTransaction)( FltObjects->Instance,
                                                     FltObjects->Transaction,
                                                     transactionContext,
                                                     FLT_MAX_TRANSACTION_NOTIFICATIONS );

    //
    //  If the enlistment failed we might have to delete the context and remove
    //  our count.
    //

    if (!NT_SUCCESS( status )) {

        //
        //  If the error is that we are already enlisted then we do not need
        //  to delete the context. Otherwise we have to delete the context
        //  before releasing our reference.
        //
        
        if (status == STATUS_FLT_ALREADY_ENLISTED) {

            status = STATUS_SUCCESS;

        } else {

            //
            //  It is worth noting that only the first caller of
            //  FltDeleteContext will remove the reference added by
            //  filter manager when the context was set.
            //
            
            FltDeleteContext( transactionContext );
        }
        
        FltReleaseContext( transactionContext );
        return status;
    }

    //
    //  Set the flag so that future enlistment efforts know that we
    //  successfully enlisted in the transaction.
    //

    SetFlagInterlocked( &transactionContext->Flags, MINISPY_ENLISTED_IN_TRANSACTION );
    
    //
    //  The operation succeeded, remove our count
    //

    FltReleaseContext( transactionContext );

    //
    //  Log a record that a new transaction has started.
    //

    recordList = SpyNewRecord();

    if (recordList) {

        SpyLogTransactionNotify( FltObjects, recordList, 0 );

        //
        //  Send the logged information to the user service.
        //

        SpyLog( recordList );
    }

#endif // MINISPY_VISTA

    return STATUS_SUCCESS;
}


#if MINISPY_VISTA

NTSTATUS
SpyKtmNotificationCallback (
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ PFLT_CONTEXT TransactionContext,
    _In_ ULONG TransactionNotification
    )
{
    PRECORD_LIST recordList;

    UNREFERENCED_PARAMETER( TransactionContext );

    //
    //  Try and get a log record
    //

    recordList = SpyNewRecord();

    if (recordList) {

        SpyLogTransactionNotify( FltObjects, recordList, TransactionNotification );

        //
        //  Send the logged information to the user service.
        //

        SpyLog( recordList );
    }

    return STATUS_SUCCESS;
}

#endif // MINISPY_VISTA

VOID
SpyDeleteTxfContext (
    _Inout_ PMINISPY_TRANSACTION_CONTEXT Context,
    _In_ FLT_CONTEXT_TYPE ContextType
    )
{
    UNREFERENCED_PARAMETER( Context );
    UNREFERENCED_PARAMETER( ContextType );

    FLT_ASSERT(FLT_TRANSACTION_CONTEXT == ContextType);
    FLT_ASSERT(Context->Count != 0);
}

//
// Ini adalah implementasi dari fungsi "worker"
//
VOID
CoreSentinelWorkItemRoutine(
    _In_ PFLT_GENERIC_WORKITEM FltWorkItem,
    _In_ PVOID FltObjects,
    _In_ PVOID Context
)
{
    UNREFERENCED_PARAMETER(FltObjects);

    PCORE_SENTINEL_WORK_ITEM pWorkItem = (PCORE_SENTINEL_WORK_ITEM)Context;
    PEPROCESS pProcess = NULL;
    NTSTATUS status;

    PAGED_CODE(); // Memverifikasi bahwa kita berada di PASSIVE_LEVEL

    status = PsLookupProcessByProcessId(pWorkItem->ProcessId, &pProcess);

    if (NT_SUCCESS(status)) {

        PCHAR imageName = (PCHAR)PsGetProcessImageFileName(pProcess);
        BOOLEAN isWhitelisted = FALSE;

        if (imageName) {

            // =======================================================
            // == INI ADALAH KODE WHITELIST YANG BENAR (KERNEL-SAFE) ==
            // =======================================================

            // 1. Definisikan daftar whitelist Anda (sebagai ANSI/CHAR)
            const CHAR* processWhitelist[] = {
                "huhuhuh.exe"
            };
            ULONG whitelistCount = sizeof(processWhitelist) / sizeof(processWhitelist[0]);

            // 2. Buat struct ANSI_STRING sementara untuk nama proses
            ANSI_STRING ansiImageName;
            RtlInitAnsiString(&ansiImageName, imageName); // Fungsi kernel

            // 3. Loop dan bandingkan menggunakan cara kernel
            for (ULONG i = 0; i < whitelistCount; i++) {

                ANSI_STRING ansiWhitelistEntry;
                RtlInitAnsiString(&ansiWhitelistEntry, processWhitelist[i]); // Fungsi kernel
                BOOLEAN bIsEqual = RtlEqualString(&ansiImageName, &ansiWhitelistEntry, TRUE);
                DbgPrint("CoreSentinel DEBUG: Comparing [%s] with [%s]. Result: %s\n",
                    ansiImageName.Buffer,
                    ansiWhitelistEntry.Buffer,
                    bIsEqual ? "TRUE (MATCH)" : "FALSE");
                // 4. Bandingkan (TRUE = case-insensitive)
                if (RtlEqualString(&ansiImageName, &ansiWhitelistEntry, TRUE)) { // Fungsi kernel
                    isWhitelisted = TRUE;
                    break;
                }
            }
            // =======================================================
            // == AKHIR KODE WHITELIST ==
            // =======================================================
        }

        //
        // 5. Periksa hasilnya
        //
        if (!isWhitelisted) {

            // Proses ini TIDAK ADA di whitelist. Hentikan.
            HANDLE hProcess = NULL;

            status = ObOpenObjectByPointer(
                pProcess,
                OBJ_KERNEL_HANDLE,
                NULL,
                PROCESS_TERMINATE,
                *PsProcessType,
                KernelMode,
                &hProcess);

            if (NT_SUCCESS(status)) {
                DbgPrint("CoreSentinel: Terminating non-whitelisted process: %s\n", imageName ? imageName : "unknown");
                ZwTerminateProcess(hProcess, STATUS_ACCESS_DENIED);
                ZwClose(hProcess);
            }
        }
        else {
            // Proses ini ADA di whitelist. Biarkan.
            DbgPrint("CoreSentinel: Whitelisted process allowed: %s\n", imageName);
        }

        // 6. Selalu lepaskan objek
        ObDereferenceObject(pProcess);
    }

    // 7. Bersihkan memori "paket pekerjaan"
    FltFreeGenericWorkItem(FltWorkItem);
}

LONG
SpyExceptionFilter (
    _In_ PEXCEPTION_POINTERS ExceptionPointer,
    _In_ BOOLEAN AccessingUserBuffer
    )
/*++

Routine Description:

    Exception filter to catch errors touching user buffers.

Arguments:

    ExceptionPointer - The exception record.

    AccessingUserBuffer - If TRUE, overrides FsRtlIsNtStatusExpected to allow
                          the caller to munge the error to a desired status.

Return Value:

    EXCEPTION_EXECUTE_HANDLER - If the exception handler should be run.

    EXCEPTION_CONTINUE_SEARCH - If a higher exception handler should take care of
                                this exception.

--*/
{
    NTSTATUS Status;

    Status = ExceptionPointer->ExceptionRecord->ExceptionCode;

    //
    //  Certain exceptions shouldn't be dismissed within the namechanger filter
    //  unless we're touching user memory.
    //

    if (!FsRtlIsNtstatusExpected( Status ) &&
        !AccessingUserBuffer) {

        return EXCEPTION_CONTINUE_SEARCH;
    }

    return EXCEPTION_EXECUTE_HANDLER;
}


