#pragma once

#include <stdint.h>

#include "arch/x86_64/syscall.hpp"

namespace syscall {

enum class SystemCall : uint64_t {
    // ABI and core system services.
    AbiMajor              = 0,
    AbiMinor              = 1,
    SystemInfo            = 2,
    Exit                  = 3,
    Yield                 = 4,
    Sleep                 = 5,
    TimeGet               = 6,
    ClockGet              = 7,
    RandomGet             = 8,

    // Generic descriptors.
    DescriptorOpen        = 9,
    DescriptorRead        = 10,
    DescriptorWrite       = 11,
    DescriptorClose       = 12,
    DescriptorGetType     = 13,
    DescriptorTestFlag    = 14,
    DescriptorGetFlags    = 15,
    DescriptorGetProperty = 16,
    DescriptorSetProperty = 17,
    DescriptorWait        = 18,

    // Files, directories, and filesystems.
    FileOpen              = 19,
    FileOpenFlags         = 20,
    FileOpenAt            = 21,
    FileCreate            = 22,
    FileCreateAt          = 23,
    FileClose             = 24,
    FileRead              = 25,
    FileReadAt            = 26,
    FileWrite             = 27,
    FileSeek              = 28,
    FileStat              = 29,
    PathStat              = 30,
    FileSync              = 31,
    FileRemove            = 32,
    FileGetAcl            = 33,
    FileSetAcl            = 34,
    DirectoryOpen         = 35,
    DirectoryOpenRoot     = 36,
    DirectoryOpenAt       = 37,
    DirectoryRead         = 38,
    DirectoryClose        = 39,
    DirectoryCreate       = 40,
    DirectoryRemove       = 41,
    Sync                  = 42,
    Mount                 = 43,
    RescanBlockDevices    = 44,

    // Virtual memory.
    MapAnonymous          = 45,
    MapAt                 = 46,
    MapFilePrivate        = 47,
    ProtectMemory         = 48,
    Unmap                 = 49,

    // Threads and synchronization.
    ThreadCreate          = 50,
    ThreadExit            = 51,
    ThreadJoin            = 52,
    ThreadDetach          = 53,
    ThreadId              = 54,
    ThreadSetTls          = 55,
    ThreadGetTls          = 56,
    FutexWait             = 57,
    FutexWaitTimed        = 58,
    FutexWake             = 59,

    // Processes and process groups.
    ProcessId             = 60,
    ProcessExec           = 61,
    Child                 = 62,
    ProcessWaitChild      = 63,
    ProcessSetCwd         = 64,
    ProcessGetCwd         = 65,
    ProcessEventSend      = 66,
    ProcessEventReceive   = 67,
    ProcessControl        = 68,
    ProcessTrace          = 69,
    ProcessSetGroup       = 70,
    ProcessGetGroup       = 71,
    ProcessCreateSession  = 72,
    ProcessGetSession     = 73,
    ProcessSetForeground  = 74,
    ProcessGetForeground  = 75,
    ProcessSetLimits      = 76,
    ProcessGetLimits      = 77,
    ProcessGetUsage       = 78,
    ChangeSlot            = 79,

    // Principals, capabilities, and users.
    PrincipalCreate       = 80,
    PrincipalSet          = 81,
    CapabilityGrant       = 82,
    CapabilityPass        = 83,
    UserCreate            = 84,
    UserFind              = 85,
    UserBumpGeneration    = 86,
    UserSetPassword       = 87,
    UserInfo              = 88,

    // Privileged system administration.
    Shutdown              = 89,
    ModuleLoad            = 90,
    ModuleCount           = 91,
    ModuleInfo            = 92,
    SettingsGet           = 93,
    SettingsSet           = 94,
    MachineSettingsGet    = 95,
    MachineSettingsSet    = 96,
    SettingsKeyAt         = 97,
    MachineSettingsKeyAt  = 98,
    MachineSettingsAccess = 99,

    // Runtime dynamic linking.
    DynamicLoad           = 100,
    DynamicSymbol         = 101,
    DynamicClose          = 102,
};

Result handle_syscall(SyscallFrame& frame);

}  // namespace syscall
