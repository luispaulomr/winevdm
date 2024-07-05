/*
 * Wine virtual DOS machine
 *
 * Copyright 2003 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#include "config.h"
#define CP_UNIXCP CP_ACP
#include "wine/port.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

#include "windef.h"
#include "winbase.h"
#include "wine/winbase16.h"
#include "wine/unicode.h"
#include "wine/debug.h"
#include "resource.h"
#include "windows.h"

#include <windows.h>
#include <stdio.h>
#include <conio.h>
#include <tchar.h>

WINE_DEFAULT_DEBUG_CHANNEL(winevdm);

extern void __wine_load_dos_exe(LPCSTR filename, LPCSTR cmdline);

/************************************************************
***************** begin LRIBEIRO changes ********************
*************************************************************
*/

struct vm86_opt {
    char aob_scan[10][100];
    char find_out_what_accesses[10][100];
};

TCHAR szName[] = TEXT("Local\\FileMapping");

static struct vm86_opt g_vm86_opts = { 0 };

static int _write_shared_data(void* msg, size_t len_msg)
{
    HANDLE hMapFile;
    LPCTSTR pBuf;

    hMapFile = CreateFileMapping(
        INVALID_HANDLE_VALUE,    // use paging file
        NULL,                    // default security
        PAGE_READWRITE,          // read/write access
        0,                       // maximum object size (high-order DWORD)
        len_msg,                       // maximum object size (low-order DWORD)
        szName);                 // name of mapping object

    if (hMapFile == NULL)
    {
        WINE_TRACE("Could not create file mapping object (%d).\n", GetLastError());
        return 1;
    }
    pBuf = (LPTSTR)MapViewOfFile(hMapFile,   // handle to map object
        FILE_MAP_ALL_ACCESS, // read/write permission
        0,
        0,
        len_msg);

    if (pBuf == NULL)
    {
        WINE_TRACE("Could not map view of file (%d).\n", GetLastError());

        CloseHandle(hMapFile);

        return 1;
    }

    CopyMemory((PVOID)pBuf, msg, len_msg);

    UnmapViewOfFile(pBuf);

    //CloseHandle(hMapFile);

    return 0;
}

/************************************************************
******************* end LRIBEIRO changes ********************
*************************************************************
*/

/***********************************************************************
 *           build_command_line
 *
 * Build the command line of a process from the argv array.
 * Copied from ENV_BuildCommandLine.
 */
static char *build_command_line( char **argv )
{
    int len;
    char *p, **arg, *cmd_line;

    if (!argv[0])
        return "";
    len = 0;
    for (arg = argv; *arg; arg++)
    {
        BOOL has_space;
        int bcount;
        char* a;

        has_space=FALSE;
        bcount=0;
        a=*arg;
        if( !*a ) has_space=TRUE;
        while (*a!='\0') {
            if (*a=='\\') {
                bcount++;
            } else {
                if (*a==' ' || *a=='\t') {
                    has_space=TRUE;
                } else if (*a=='"') {
                    /* doubling of '\' preceding a '"',
                     * plus escaping of said '"'
                     */
                    len+=2*bcount+1;
                }
                bcount=0;
            }
            a++;
        }
        len+=(a-*arg)+1 /* for the separating space */;
        if (has_space)
            len+=2; /* for the quotes */
    }

    if (!(cmd_line = HeapAlloc( GetProcessHeap(), 0, len ? len + 1 : 2 ))) 
        return NULL;

    p = cmd_line;
    *p++ = (len - 1 < 256) ? len - 1 /* remove last space */ : 255;
    for (arg = argv; *arg; arg++)
    {
        BOOL has_space,has_quote;
        char* a;

        /* Check for quotes and spaces in this argument */
        has_space=has_quote=FALSE;
        a=*arg;
        if( !*a ) has_space=TRUE;
        while (*a!='\0') {
            if (*a==' ' || *a=='\t') {
                has_space=TRUE;
                if (has_quote)
                    break;
            } else if (*a=='"') {
                has_quote=TRUE;
                if (has_space)
                    break;
            }
            a++;
        }

        /* Now transfer it to the command line */
        if (has_space)
            *p++='"';
        if (has_quote) {
            int bcount;

            bcount=0;
            a=*arg;
            while (*a!='\0') {
                if (*a=='\\') {
                    *p++=*a;
                    bcount++;
                } else {
                    if (*a=='"') {
                        int i;

                        /* Double all the '\\' preceding this '"', plus one */
                        for (i=0;i<=bcount;i++)
                            *p++='\\';
                        *p++='"';
                    } else {
                        *p++=*a;
                    }
                    bcount=0;
                }
                a++;
            }
        } else {
            strcpy(p,*arg);
            p+=strlen(*arg);
        }
        if (has_space)
            *p++='"';
        *p++=' ';
    }
    if (len) p--;  /* remove last space */
    *p = '\0';
    return cmd_line;
}

/***********************************************************************
 *           usage
 */
static void usage(void)
{
    WINE_MESSAGE( "Usage: winevdm.exe [--app-name app.exe] command line\n\n" );
    ExitProcess(1);
}

#include <winternl.h>

typedef struct _CURDIR
{
    UNICODE_STRING DosPath;
    PVOID Handle;
} CURDIR, *PCURDIR;

typedef struct RTL_DRIVE_LETTER_CURDIR
{
    USHORT              Flags;
    USHORT              Length;
    ULONG               TimeStamp;
    UNICODE_STRING      DosPath;
} RTL_DRIVE_LETTER_CURDIR, *PRTL_DRIVE_LETTER_CURDIR;

typedef struct tagRTL_BITMAP {
    ULONG  SizeOfBitMap; /* Number of bits in the bitmap */
    PULONG Buffer; /* Bitmap data, assumed sized to a DWORD boundary */
} RTL_BITMAP, *PRTL_BITMAP;

typedef const RTL_BITMAP *PCRTL_BITMAP;

typedef struct tagRTL_BITMAP_RUN {
    ULONG StartingIndex; /* Bit position at which run starts */
    ULONG NumberOfBits;  /* Size of the run in bits */
} RTL_BITMAP_RUN, *PRTL_BITMAP_RUN;

typedef const RTL_BITMAP_RUN *PCRTL_BITMAP_RUN;

/* value for Flags field (FIXME: not the correct name) */
#define PROCESS_PARAMS_FLAG_NORMALIZED 1

typedef struct _GDI_TEB_BATCH
{
    ULONG  Offset;
    HANDLE HDC;
    ULONG  Buffer[0x136];
} GDI_TEB_BATCH;

typedef struct _RTL_ACTIVATION_CONTEXT_STACK_FRAME
{
    struct _RTL_ACTIVATION_CONTEXT_STACK_FRAME *Previous;
    struct _ACTIVATION_CONTEXT                 *ActivationContext;
    ULONG                                       Flags;
} RTL_ACTIVATION_CONTEXT_STACK_FRAME, *PRTL_ACTIVATION_CONTEXT_STACK_FRAME;

typedef struct _ACTIVATION_CONTEXT_STACK
{
    ULONG                               Flags;
    ULONG                               NextCookieSequenceNumber;
    RTL_ACTIVATION_CONTEXT_STACK_FRAME *ActiveFrame;
    LIST_ENTRY                          FrameListCache;
} ACTIVATION_CONTEXT_STACK, *PACTIVATION_CONTEXT_STACK;

/***********************************************************************
* PEB data structure
*/
typedef struct 
{                                                                 /* win32/win64 */
    BOOLEAN                      InheritedAddressSpace;             /* 000/000 */
    BOOLEAN                      ReadImageFileExecOptions;          /* 001/001 */
    BOOLEAN                      BeingDebugged;                     /* 002/002 */
    BOOLEAN                      SpareBool;                         /* 003/003 */
    HANDLE                       Mutant;                            /* 004/008 */
    HMODULE                      ImageBaseAddress;                  /* 008/010 */
    PPEB_LDR_DATA                LdrData;                           /* 00c/018 */
    RTL_USER_PROCESS_PARAMETERS *ProcessParameters;                 /* 010/020 */
    PVOID                        SubSystemData;                     /* 014/028 */
    HANDLE                       ProcessHeap;                       /* 018/030 */
    PRTL_CRITICAL_SECTION        FastPebLock;                       /* 01c/038 */
    PVOID /*PPEBLOCKROUTINE*/    FastPebLockRoutine;                /* 020/040 */
    PVOID /*PPEBLOCKROUTINE*/    FastPebUnlockRoutine;              /* 024/048 */
    ULONG                        EnvironmentUpdateCount;            /* 028/050 */
    PVOID                        KernelCallbackTable;               /* 02c/058 */
    ULONG                        Reserved[2];                       /* 030/060 */
    PVOID /*PPEB_FREE_BLOCK*/    FreeList;                          /* 038/068 */
    ULONG                        TlsExpansionCounter;               /* 03c/070 */
    PRTL_BITMAP                  TlsBitmap;                         /* 040/078 */
    ULONG                        TlsBitmapBits[2];                  /* 044/080 */
    PVOID                        ReadOnlySharedMemoryBase;          /* 04c/088 */
    PVOID                        ReadOnlySharedMemoryHeap;          /* 050/090 */
    PVOID                       *ReadOnlyStaticServerData;          /* 054/098 */
    PVOID                        AnsiCodePageData;                  /* 058/0a0 */
    PVOID                        OemCodePageData;                   /* 05c/0a8 */
    PVOID                        UnicodeCaseTableData;              /* 060/0b0 */
    ULONG                        NumberOfProcessors;                /* 064/0b8 */
    ULONG                        NtGlobalFlag;                      /* 068/0bc */
    LARGE_INTEGER                CriticalSectionTimeout;            /* 070/0c0 */
    SIZE_T                       HeapSegmentReserve;                /* 078/0c8 */
    SIZE_T                       HeapSegmentCommit;                 /* 07c/0d0 */
    SIZE_T                       HeapDeCommitTotalFreeThreshold;    /* 080/0d8 */
    SIZE_T                       HeapDeCommitFreeBlockThreshold;    /* 084/0e0 */
    ULONG                        NumberOfHeaps;                     /* 088/0e8 */
    ULONG                        MaximumNumberOfHeaps;              /* 08c/0ec */
    PVOID                       *ProcessHeaps;                      /* 090/0f0 */
    PVOID                        GdiSharedHandleTable;              /* 094/0f8 */
    PVOID                        ProcessStarterHelper;              /* 098/100 */
    PVOID                        GdiDCAttributeList;                /* 09c/108 */
    PVOID                        LoaderLock;                        /* 0a0/110 */
    ULONG                        OSMajorVersion;                    /* 0a4/118 */
    ULONG                        OSMinorVersion;                    /* 0a8/11c */
    ULONG                        OSBuildNumber;                     /* 0ac/120 */
    ULONG                        OSPlatformId;                      /* 0b0/124 */
    ULONG                        ImageSubSystem;                    /* 0b4/128 */
    ULONG                        ImageSubSystemMajorVersion;        /* 0b8/12c */
    ULONG                        ImageSubSystemMinorVersion;        /* 0bc/130 */
    ULONG                        ImageProcessAffinityMask;          /* 0c0/134 */
    HANDLE                       GdiHandleBuffer[28];               /* 0c4/138 */
    ULONG                        unknown[6];                        /* 134/218 */
    PVOID                        PostProcessInitRoutine;            /* 14c/230 */
    PRTL_BITMAP                  TlsExpansionBitmap;                /* 150/238 */
    ULONG                        TlsExpansionBitmapBits[32];        /* 154/240 */
    ULONG                        SessionId;                         /* 1d4/2c0 */
    ULARGE_INTEGER               AppCompatFlags;                    /* 1d8/2c8 */
    ULARGE_INTEGER               AppCompatFlagsUser;                /* 1e0/2d0 */
    PVOID                        ShimData;                          /* 1e8/2d8 */
    PVOID                        AppCompatInfo;                     /* 1ec/2e0 */
    UNICODE_STRING               CSDVersion;                        /* 1f0/2e8 */
    PVOID                        ActivationContextData;             /* 1f8/2f8 */
    PVOID                        ProcessAssemblyStorageMap;         /* 1fc/300 */
    PVOID                        SystemDefaultActivationData;       /* 200/308 */
    PVOID                        SystemAssemblyStorageMap;          /* 204/310 */
    SIZE_T                       MinimumStackCommit;                /* 208/318 */
    PVOID                       *FlsCallback;                       /* 20c/320 */
    LIST_ENTRY                   FlsListHead;                       /* 210/328 */
    PRTL_BITMAP                  FlsBitmap;                         /* 218/338 */
    ULONG                        FlsBitmapBits[4];                  /* 21c/340 */
} PEB2, *PPEB2;
/***********************************************************************
* TEB data structure
*/
typedef struct
{                                                                 /* win32/win64 */
    NT_TIB                       Tib;                               /* 000/0000 */
    PVOID                        EnvironmentPointer;                /* 01c/0038 */
    HANDLE                    ClientId1, ClientId2;                          /* 020/0040 */
    PVOID                        ActiveRpcHandle;                   /* 028/0050 */
    PVOID                        ThreadLocalStoragePointer;         /* 02c/0058 */
    PPEB2                         Peb;                               /* 030/0060 */
} TEB2, *PTEB2;

//===reactos/sdk/include/ndk/pstypes.h==
// PEB.AppCompatFlags
// Tag FLAG_MASK_KERNEL
//
typedef enum _APPCOMPAT_FLAGS
{
    GetShortPathNameNT4 = 0x1,
    GetDiskFreeSpace2GB = 0x8,
    FTMFromCurrentAPI = 0x20,
    DisallowCOMBindingNotifications = 0x40,
    Ole32ValidatePointers = 0x80,
    DisableCicero = 0x100,
    Ole32EnableAsyncDocFile = 0x200,
    EnableLegacyExceptionHandlinginOLE = 0x400,
    DisableAdvanceRPCClientHardening = 0x800,
    DisableMaybeNULLSizeisConsistencycheck = 0x1000,
    DisableAdvancedRPCrangeCheck = 0x4000,
    EnableLegacyExceptionHandlingInRPC = 0x8000,
    EnableLegacyNTFSFlagsForDocfileOpens = 0x10000,
    DisableNDRIIDConsistencyCheck = 0x20000,
    UserDisableForwarderPatch = 0x40000,
    DisableNewWMPAINTDispatchInOLE = 0x100000,
    Unknown0x00400000 = 0x00400000,
    DoNotAddToCache = 0x80000000,
} APPCOMPAT_FLAGS;


//
// PEB.AppCompatFlagsUser.LowPart
// Tag FLAG_MASK_USER
//
typedef enum _APPCOMPAT_USERFLAGS
{
    DisableAnimation = 0x1,
    DisableKeyboardCues = 0x2,
    No50StylebitsInSetWindowLong = 0x4,
    DisableDrawPatternRect = 0x8,
    MSShellDialog = 0x10,
    NoDDETerminateDuringDestroy = 0x20,
    GiveupForeground = 0x40,
    AlwaysActiveMenus = 0x80,
    NoMouseHideInEdit = 0x100,
    NoGdiBatching = 0x200,
    FontSubstitution = 0x400,
    No50StylebitsInCreateWindow = 0x800,
    NoCustomPaperSizes = 0x1000,
    AllTheDdeHacks = 0x2000,
    UseDefaultCharset = 0x4000,
    NoCharDeadKey = 0x8000,
    NoTryExceptForWindowProc = 0x10000,
    NoInitInsertReplaceFlags = 0x20000,
    NoDdeSync = 0x40000,
    NoGhost = 0x80000,
    NoDdeAsyncReg = 0x100000,
    StrictLLHook = 0x200000,
    NoShadow = 0x400000,
    NoTimerCallbackProtection = 0x1000000,
    HighDpiAware = 0x2000000,
    OpenGLEmfAware = 0x4000000,
    EnableTransparantBltMirror = 0x8000000,
    NoPaddedBorder = 0x10000000,
    ForceLegacyResizeCM = 0x20000000,
    HardwareAudioMixer = 0x40000000,
    DisableSWCursorOnMoveSize = 0x80000000,
#if 0
    DisableWindowArrangement = 0x100000000,
    ReorderWaveForCommunications = 0x200000000,
    NoGdiHwAcceleration = 0x400000000,
#endif
} APPCOMPAT_USERFLAGS;

//
// PEB.AppCompatFlagsUser.HighPart
// Tag FLAG_MASK_USER
//
typedef enum _APPCOMPAT_USERFLAGS_HIGHPART
{
    DisableWindowArrangement = 0x1,
    ReorderWaveForCommunications = 0x2,
    NoGdiHwAcceleration = 0x4,
} APPCOMPAT_USERFLAGS_HIGHPART;
//===reactos/sdk/include/ndk/pstypes.h==

void set_dll_path()
{
    WCHAR mod_path[MAX_PATH];
    DWORD attr;
    GetModuleFileNameW(NULL, mod_path, _countof(mod_path));
    LPWSTR last = wcsrchr(mod_path, L'\\');
    last[0] = 0;
    LPCWSTR dir = L"\\dll";
    if (wcslen(mod_path) + wcslen(dir) + 1 < MAX_PATH)
    {
        memcpy(last, dir, (wcslen(dir) + 1) * sizeof(*dir));
        attr = GetFileAttributesW(mod_path);
        if (attr != INVALID_FILE_ATTRIBUTES && attr & FILE_ATTRIBUTE_DIRECTORY)
        {
            WINE_TRACE("%s\n", wine_dbgstr_wn(mod_path, -1));
            SetDllDirectoryW(mod_path);
            return;
        }
    }
    last[0] = 0;
    WINE_TRACE("%s\n", wine_dbgstr_wn(mod_path, -1));
    SetDllDirectoryW(mod_path);
}

static BOOL set_peb_compatible_flag()
{
    BOOL success = TRUE;
    TEB2 *teb = (TEB2*)NtCurrentTeb();
    APPCOMPAT_FLAGS flags1 = (APPCOMPAT_FLAGS)teb->Peb->AppCompatFlags.LowPart;
    APPCOMPAT_USERFLAGS flags2 = (APPCOMPAT_USERFLAGS)teb->Peb->AppCompatFlagsUser.LowPart;
    APPCOMPAT_USERFLAGS_HIGHPART flags3 = (APPCOMPAT_USERFLAGS_HIGHPART)teb->Peb->AppCompatFlagsUser.HighPart;
    APPCOMPAT_FLAGS f = NoPaddedBorder | NoGhost;
    HMODULE user32 = GetModuleHandleA("user32.dll");
    set_dll_path();
    /* is old windows? */
    if (GetProcAddress(GetModuleHandleA("kernel32"), "GetFileInformationByHandleEx") == NULL)
        return TRUE;
    if ((flags2 & f) != f && user32 != NULL)
    {
        /* Lazy loading currently only works with MSVC. */
#ifdef _MSC_VER
        WINE_ERR("user32.dll has already been loaded. (Anti-virus software may be the cause.)\n");
#else
        WINE_ERR("user32.dll has already been loaded.\n");
#endif
        WINE_ERR("Some compatibility flags can not be applied.\n");
        success = FALSE;
    }
    teb->Peb->AppCompatFlagsUser.LowPart |= f;
    //teb->Peb->AppCompatFlagsUser.LowPart = -1;
    //teb->Peb->AppCompatFlagsUser.HighPart = -1;
    //teb->Peb->AppCompatFlags.LowPart = -1;
    return success;
}

static BOOL fix_compatible(void)
{
    WCHAR image_path[MAX_PATH];
    LPWSTR arg = GetCommandLineW();
    DWORD pid = GetCurrentProcessId();
    LPWSTR compat_arg = L"--fix-compat-mode ";
    PROCESS_BASIC_INFORMATION pbi;
    PWSTR arg2;
    PROCESS_INFORMATION proc_inf;
    STARTUPINFOW start_inf = { sizeof(STARTUPINFOW) };
    GetModuleFileNameW(NULL, image_path, sizeof(image_path));
#ifdef SET_COMPAT_LAYER
    {
        HKEY hkey;
        LSTATUS stat = RegOpenKeyW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers", &hkey);
        if (stat)
            return FALSE;
        LPWSTR reg_value = L"VistaRTM";// L"~ Win7RTM";
        stat = RegSetValueExW(hkey, image_path, 0, REG_SZ, reg_value, strlenW(reg_value) * sizeof(WCHAR));
        if (stat)
            return FALSE;
        RegCloseKey(hkey);
    }
#endif
    arg2 = (PWSTR)malloc(sizeof(WCHAR) * (11 + strlenW(arg) + strlenW(compat_arg)));
    sprintfW(arg2, L"%s%d %s", compat_arg, pid, arg);
    if (!CreateProcessW(image_path, arg2, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &start_inf, &proc_inf))
    {
        free(arg2);
        return FALSE;
    }
    free(arg2);
    WINE_ERR("child pid = %d\n", proc_inf.dwProcessId);
    if (NT_SUCCESS(NtQueryInformationProcess(proc_inf.hProcess, ProcessBasicInformation, &pbi, sizeof(pbi), NULL)))
    {
        ULARGE_INTEGER AppCompatFlagsUser;
        if (ReadProcessMemory(proc_inf.hProcess, (ULONG_PTR)pbi.PebBaseAddress + offsetof(PEB2, AppCompatFlagsUser), &AppCompatFlagsUser, sizeof(AppCompatFlagsUser), NULL))
        {
            AppCompatFlagsUser.LowPart |= NoPaddedBorder;
            WriteProcessMemory(proc_inf.hProcess, (ULONG_PTR)pbi.PebBaseAddress + offsetof(PEB2, AppCompatFlagsUser), &AppCompatFlagsUser, sizeof(AppCompatFlagsUser), NULL);
        }
    }
    ResumeThread(proc_inf.hThread);
    WaitForSingleObject(proc_inf.hProcess, INFINITE);
    CloseHandle(proc_inf.hProcess);
    CloseHandle(proc_inf.hThread);
    return TRUE;
}

static BOOL is_win32_exe(LPCSTR appname)
{
    HANDLE file = CreateFileA(appname, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    IMAGE_DOS_HEADER header;
    DWORD read = 0;
    ReadFile(file, &header, sizeof(header), &read, NULL);
    if (read == sizeof(header) && header.e_lfanew >= sizeof(header))
    {
        SetFilePointer(file, header.e_lfanew, 0, FILE_BEGIN);
        WORD magic;
        ReadFile(file, &magic, sizeof(WORD), &read, NULL);
        if (read == sizeof(WORD))
        {
            if (magic == IMAGE_NT_SIGNATURE)
            {
                CloseHandle(file);
                return TRUE;
            }
        }
    }
    CloseHandle(file);
    return FALSE;
}
static void exec16(LOADPARAMS16 params, LPCSTR appname, LPCSTR cmdline, BOOL exit)
{
    char *p;
    HINSTANCE16 instance;
    char shortpath[MAX_PATH];
    GetShortPathNameA(appname, shortpath, MAX_PATH);
    appname = shortpath;

    if (is_win32_exe(appname))
    {
        WINE_ERR("%s is a win32 executable file!\n", appname);
    }
    if ((instance = LoadModule16(appname, &params)) < 32)
    {
        if (instance == 11)
        {
            __wine_load_dos_exe(appname, cmdline + 1);
            /* if we get back here it failed */
            instance = GetLastError();
        }

        WINE_MESSAGE("winevdm: can't exec '%s': ", appname);
        switch (instance)
        {
        case  2: WINE_MESSAGE("file not found\n"); break;
        case 11: WINE_MESSAGE("invalid program file\n"); break;
        default: WINE_MESSAGE("error=%d\n", instance); break;
        }
        if (exit)
            ExitProcess(instance);
    }

}

#include <pshpack1.h>
#define SHARED_WOW_CURDIR_SUPPORTED 1
typedef struct {
    WORD header;
    WORD showCmd;
    CHAR appname[MAX_PATH];
    CHAR cmdline[MAX_PATH];
    CHAR curdir[MAX_PATH];
    /* environment variables... */
} shared_wow_exec;
#include <poppack.h>
DWORD exec16_thread(LPVOID args)
{
    shared_wow_exec exec_data = *(shared_wow_exec*)args;
    HeapFree(GetProcessHeap(), 0, args);
    LOADPARAMS16 params;
    WORD showCmd[2];
    params.hEnvironment = 0;
    params.reserved = 0;
    if (exec_data.header == SHARED_WOW_CURDIR_SUPPORTED)
    {
        params.hEnvironment = 0x0bef;
        params.reserved = &exec_data.curdir;
    }
    showCmd[0] = 2;
    showCmd[1] = SW_SHOW;

    params.cmdLine = MapLS(exec_data.cmdline);
    params.showCmd = MapLS(showCmd);
    exec16(params, exec_data.appname, exec_data.cmdline, FALSE);
    return 0;
}

/* Returns TRUE if the pipe is pending. */
static BOOL connect_new_client(HANDLE handle, LPOVERLAPPED lpov)
{
    DWORD err;
    if (ConnectNamedPipe(handle, lpov))
    {
        return FALSE;
    }
    err = GetLastError();
    if (err == ERROR_IO_PENDING)
    {
        return TRUE;
    }
    if (err == ERROR_PIPE_CONNECTED)
    {
        SetEvent(lpov->hEvent);
    }
    return FALSE;
}

/* \\.\pipe\otvdmpipe */
HANDLE run_shared_wow_server()
{
    OVERLAPPED overlapped = { 0 };
    BOOL pending;
    HANDLE server = CreateNamedPipeA("\\\\.\\pipe\\otvdmpipe", PIPE_ACCESS_INBOUND | PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT, 10, 0, 0, 1000, NULL);
    HANDLE event = CreateEventA(NULL, TRUE, TRUE, NULL);
    overlapped.hEvent = event;
    pending = connect_new_client(server, &overlapped);
    while (TRUE)
    {
        HANDLE handles[1] = { overlapped.hEvent };
        MSG msg;
        DWORD ret = MsgWaitForMultipleObjects(1, handles, FALSE, INFINITE, QS_ALLINPUT);

        if (ret == WAIT_OBJECT_0 + 1 && PeekMessageA(&msg, NULL, 0, 0, TRUE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        else if (pending)
        {
            DWORD read;
            shared_wow_exec exec_data = { 0 };
            BOOL r = ReadFile(server, &exec_data, sizeof(exec_data), &read, NULL);
            DisconnectNamedPipe(server);
            if (r)
            {
                WINE_TRACE("%s %s\n", exec_data.appname, exec_data.cmdline);
                LPVOID data = HeapAlloc(GetProcessHeap(), 0, sizeof(exec_data));
                memcpy(data, &exec_data, sizeof(exec_data));
                /* LoadModule16 blocks thread */
                HANDLE hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)exec16_thread, data, 0, NULL);
                CloseHandle(hThread);
            }
            pending = connect_new_client(server, &overlapped);
        }
    }
}

BOOL run_shared_wow(LPCSTR appname, WORD showCmd, LPCSTR cmdline)
{
    ULONG pid;
    HANDLE client = CreateFileA("\\\\.\\pipe\\otvdmpipe", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    BOOL (WINAPI *pGetNamedPipeServerProcessId)(HANDLE, PULONG) = (BOOL(WINAPI *)(HANDLE, PULONG))GetProcAddress(GetModuleHandleA("kernel32"), "GetNamedPipeServerProcessId");
    if (client == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    if (pGetNamedPipeServerProcessId && pGetNamedPipeServerProcessId(client, &pid))
    {
        AllowSetForegroundWindow(pid);
    }
    shared_wow_exec exec_data = { 0 };
    exec_data.header = SHARED_WOW_CURDIR_SUPPORTED;
    exec_data.showCmd = showCmd;
    lstrcpynA(exec_data.cmdline, cmdline, MAX_PATH);
    lstrcpynA(exec_data.appname, appname, MAX_PATH);
    GetCurrentDirectoryA(MAX_PATH, exec_data.curdir);
    WriteFile(client, &exec_data, sizeof(exec_data), NULL, NULL);
    CloseHandle(client);
    return TRUE;

}

/***********************************************************************
 *           main
 */
#ifdef _CONSOLE
int main( int argc, char *argv[] )
#else
int entry_point( int argc, char *argv[] )
#endif
{
    DWORD count;
    LOADPARAMS16 params;
    WORD showCmd[2];
    char buffer[MAX_PATH];
    STARTUPINFOA info;
    char *cmdline, *appname, **first_arg;
    DWORD pid;
    const char *cmdline1 = strstr(argv[0], "  --ntvdm64:");
    char **argv_copy = HeapAlloc(GetProcessHeap(), 0, sizeof(*argv) * (argc + 1));
    BOOL compat_success = set_peb_compatible_flag();
    BOOL use_shared_wow_server;
#ifdef __CI_VERSION
#define STR(x) #x
#define STRSTR(x) STR(x)
    fprintf(stderr, "version: %s\n", STRSTR(__CI_VERSION));
#undef STR
#undef STRSTR
#endif
    memcpy(argv_copy, argv, argc * sizeof(*argv));
    argv = argv_copy;
    /*
    NtVdm64 command line handling is buggy.
    Can not be handled if the extension is omitted
    CommandLine = "%c"

    >A.EXE
    GetCommandLineA()="A.EXE "
    >"A.EXE"
    GetCommandLineA()="\"A.EXE\" "
    >A.EXE a b c d
    GetCommandLineA()="A.EXE a b c d"

    >A
    GetCommandLineA()="A "
    >"A"
    GetCommandLineA()="\"A \""
                          ^????????????
    >A a b c d
    GetCommandLineA()="A a b c d"
    >"A" a b c d
    GetCommandLineA()="\"A \"  a b c d"


    CommandLine = "aaaa%cbbbb"

    >A.EXE
    GetCommandLineA()="A.EXE aaaabbbb"
    >A.EXE 1 2 3 4
    GetCommandLineA()="A.EXE aaaa1 2 3 4bbbb"
    >"A.EXE"
    GetCommandLineA()="\"A\" aaaabbbb"
    >"A.EXE" 1 2 3 4
    GetCommandLineA()="\"A.EXE\" aaaa1 2 3 4bbbb"

    >A
    GetCommandLineA()="A aaaabbbb"
    >A 1 2 3 4
    GetCommandLineA()="A aaaa1 2 3 4bbbb"
    >"A"
    GetCommandLineA()="\"A aaaa\"bbbb"
    >"A" 1 2 3 4
    GetCommandLineA()="\"A aaaa\"  1 2 3 4bbbb~"
    Workaround
    " --ntvdm64: "%m" --ntvdm64-args: %c"

    if argv[0] contains " --ntvdm64:"
       "\"A  --ntvdm64: \"full-dos-path\A.EXE\" --ntvdm64-args \"  1 2 3 4"
    =>"\"A\"            \"full-dos-path\A.EXE\" --ntvdm64-args    1 2 3 4"
    */
    if (cmdline1)
    {
        LPWSTR raw = GetCommandLineW();
        if (raw[0] == L'"')
        {
            LPWSTR new_cmdline = HeapAlloc(GetProcessHeap(), 0, (wcslen(raw) + 1) * sizeof(WCHAR));
            memcpy(new_cmdline, raw, (wcslen(raw) + 1) * sizeof(WCHAR));
            LPWSTR magic = wcsstr(new_cmdline, L"  --ntvdm64:");
            if (magic)
            {
                magic[0] = L'\"';
            }
            LPWSTR magic2 = wcsstr(new_cmdline, L"--ntvdm64-args: \"");
            if (magic2)
            {
                *wcschr(magic2, L'\"') = L' ';
            }
            LPWSTR *argvw = CommandLineToArgvW(new_cmdline, &argc);
            HeapFree(GetProcessHeap(), 0, argv_copy);
            argv_copy = HeapAlloc(GetProcessHeap(), 0, sizeof(*argv) * (argc + 1));
            for (int i = 0; i < argc; i++)
            {
                int length = WideCharToMultiByte(CP_ACP, 0, argvw[i], -1, NULL, 0, NULL, NULL);
                LPSTR arg = HeapAlloc(GetProcessHeap(), NULL, (length + 1) * sizeof(CHAR));
                WideCharToMultiByte(CP_ACP, 0, argvw[i], -1, arg, length, NULL, NULL);
                arg[length] = '\0';
                argv_copy[i] = arg;
            }
            LocalFree(argvw);
            argv = argv_copy;
        }
    }
    if (argc > 1 && !strcmp(argv[1], "--ntvdm64:"))
    {
        //remove argv[1]
        for (int i = 1; i < argc - 1; i++)
        {
            argv[i] = argv[i + 1];
        }
        argc--;
    }
    if (argc > 2 && !strcmp(argv[2], "--ntvdm64-args:"))
    {
        //remove argv[2]
        for (int i = 2; i < argc - 1; i++)
        {
            argv[i] = argv[i + 1];
        }
        argc--;
    }
    /* argv must be null-terminated */
    argv[argc] = NULL;


    if (!strcmp(argv[0], "--fix-compat-mode"))
    {
        pid = atoi(argv[1]);
        WINE_ERR("parent pid = %d\n", pid);
        argv += 2;
        argc -= 2;
        compat_success = TRUE;
    }
    //compatible mode
    else if ((!compat_success

#ifdef FIX_COMPAT_MODE
            || 1
#endif
            )
#ifdef SET_COMPAT_LAYER
        && getenv("__COMPAT_LAYER") == NULL
#endif
        )
    {
#ifdef SET_COMPAT_LAYER
        WINE_ERR("Set compatible mode to VistaRTM\n");
#else
        WINE_ERR("Spawn a child process to apply compatible flags.\n");
#endif
        if (fix_compatible())
        {
            return 0;
        }
        else
        {
            WINE_ERR("failed");
        }
    }

    if (!argv[1]) usage();

    if (!strcmp( argv[1], "--app-name" ))
    {
        if (!(appname = argv[2])) usage();
        first_arg = argv + 2;
    }
    else
    {
        if (!SearchPathA( ".", argv[1], ".exe", sizeof(buffer), buffer, NULL ) &&
            !SearchPathA( ".", argv[1], ".com", sizeof(buffer), buffer, NULL ) &&
            !SearchPathA( NULL, argv[1], ".exe", sizeof(buffer), buffer, NULL ) &&
            !SearchPathA( NULL, argv[1], ".com", sizeof(buffer), buffer, NULL ))
        {
            WINE_MESSAGE( "winevdm: unable to exec '%s': file not found\n", argv[1] );
            ExitProcess(1);
        }
        appname = buffer;
        first_arg = argv + 1;
    }

    if (*first_arg) first_arg++;  /* skip program name */
    cmdline = build_command_line( first_arg );

/************************************************************
***************** begin LRIBEIRO changes ********************
*************************************************************
*/

    /* CLI options */

    if (WINE_TRACE_ON(winevdm))
    {
        char** _argv = first_arg;
        //for (int i = 0; _argv[i]; ++i) {
        //    WINE_TRACE("argv: '%s'\n", _argv[i]);
        //}

        UINT32 i = 0;
        UINT32 i_aob_scan = 0;
        UINT32 i_find_out = 0;
        while (_argv[i]) {
            if (strcmp(_argv[i], "--aob-scan") == 0) {
                strncpy(g_vm86_opts.aob_scan[i_aob_scan], _argv[i + 1], sizeof(g_vm86_opts.aob_scan[i_aob_scan]));
                i_aob_scan++;
            }
            else if (strcmp(_argv[i], "--find-out-what-accesses") == 0) {
                strncpy(g_vm86_opts.find_out_what_accesses[i_find_out], _argv[i + 1], sizeof((g_vm86_opts.aob_scan[i_aob_scan])));
                i_find_out++;
            }
            i += 2;
        }

        //char** p = g_vm86_opts.aob_scan;
        //while (*p) {
        //    WINE_TRACE("aob_scan: '%s'\n", p++);
        //}
        //p = g_vm86_opts.find_out_what_accesses;
        //while (*p) {
        //    WINE_TRACE("find_out_what_accesses: '%s'\n", p++);
        //}
    }

    if (_write_shared_data((void*)&g_vm86_opts, sizeof(g_vm86_opts)) != 0) {
        WINE_TRACE("error writing to mapped file\n");
    }

/************************************************************
***************** end LRIBEIRO changes **********************
*************************************************************
*/

    if (WINE_TRACE_ON(winevdm))
    {
        int i;
        WINE_TRACE( "GetCommandLine = '%s'\n", GetCommandLineA() );
        WINE_TRACE( "appname = '%s'\n", appname );
        WINE_TRACE( "cmdline = '%.*s'\n", cmdline[0], cmdline+1 );
        for (i = 0; argv[i]; i++) WINE_TRACE( "argv[%d]: '%s'\n", i, argv[i] );
    }

    GetStartupInfoA( &info );
    showCmd[0] = 2;
    showCmd[1] = (info.dwFlags & STARTF_USESHOWWINDOW) ? info.wShowWindow : SW_SHOWNORMAL;

    params.hEnvironment = 0;

    use_shared_wow_server = !krnl386_get_config_int("otvdm", "SeparateWOWVDM", TRUE);
    if (use_shared_wow_server && run_shared_wow(appname, showCmd[1], cmdline))
    {
        return 0;
    }

    params.cmdLine = MapLS( cmdline );
    params.showCmd = MapLS( showCmd );
    params.reserved = 0;

    RestoreThunkLock(1);  /* grab the Win16 lock */

    if (krnl386_get_config_int("otvdm", "ForceSingleProcessor", FALSE))
        SetProcessAffinityMask(GetCurrentProcess(), 1);

    /* some programs assume mmsystem is always present */
    LoadLibrary16( "gdi.exe" );
    LoadLibrary16( "user.exe" );
    LoadLibrary16( "mmsystem.dll" );
    LoadLibrary16( "shell.dll" );

    krnl386_set_compat_path(appname);

    if (krnl386_get_config_int("otvdm", "EnableVisualStyle", FALSE))
    {
        ACTCTXA actctx = { 0 };
        actctx.cbSize = sizeof(actctx);
        actctx.hModule = GetModuleHandleW(NULL);
        actctx.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
        actctx.lpResourceName = MAKEINTRESOURCE(IDS_MANIFEST);
        HANDLE result = CreateActCtxA(&actctx);
        ULONG_PTR lpCookie;
        BOOL res = ActivateActCtx(result, &lpCookie);
    }

    exec16(params, appname, cmdline, TRUE);
    /* wait forever; the process will be killed when the last task exits */
    ReleaseThunkLock(&count);
    if (use_shared_wow_server)
        run_shared_wow_server();
    Sleep(1000);  // sleep for a second so the loaded task can start, allows waitforinputidle to work
    while (TRUE)
    {
        MSG msg;
        GetMessageA(&msg, NULL, 0, 0);
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    Sleep( INFINITE );
    return 0;
}

#ifndef _CONSOLE
/* otvdmw.exe entry point */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInst, LPSTR lpszCmdLine, int nShowCmd)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInst);
    UNREFERENCED_PARAMETER(lpszCmdLine);
    UNREFERENCED_PARAMETER(nShowCmd);
    set_peb_compatible_flag();
    if (__argc == 1)
    {
        OPENFILENAMEA ofn = { 0 };
        char file[MAX_PATH] = { 0 };
        ofn.lpstrFilter = "EXE(*.EXE)\0*.EXE\0";
        ofn.nFilterIndex = 1;
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFile = file;
        ofn.nMaxFile = sizeof(file);
        ofn.Flags = OFN_FILEMUSTEXIST;
        ofn.lpstrDefExt = "EXE";
        ofn.lpstrTitle = "";
        if (!GetOpenFileNameA(&ofn))
            return 1;
        LPSTR *argv = HeapAlloc(GetProcessHeap(), 0, sizeof(char*) * 3);
        argv[0] = __argv[0];
        argv[1] = ofn.lpstrFile;
        argv[2] = 0;
        if (is_win32_exe(argv[1]))
        {
            char buf[1000];
            snprintf(buf, sizeof(buf), "\"%s\" is a win32 executable file!\n", argv[1]);
            WINE_ERR("%s is a win32 executable file!\n", argv[1]);
            MessageBoxA(NULL, buf, NULL, MB_OK | MB_ICONWARNING);
        }
        return entry_point(2, argv);
    }
    return entry_point(__argc, __argv);
}
#endif
