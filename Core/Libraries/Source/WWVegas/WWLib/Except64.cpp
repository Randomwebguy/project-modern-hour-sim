// SPDX-License-Identifier: GPL-3.0-or-later
// AMD64 diagnostics: addresses must never pass through Windows' 32-bit LONG.
#include "Except.h"
#include <dbghelp.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <atomic>

std::uintptr_t ExceptionReturnStack = 0;
std::uintptr_t ExceptionReturnAddress = 0;
std::uintptr_t ExceptionReturnFrame = 0;
static std::recursive_mutex symbolMutex;
static bool symbolsReady = false;
static std::atomic<unsigned long> mainThread{0};
static std::atomic<bool> tryingToExit{false};
static std::atomic_flag handling = ATOMIC_FLAG_INIT;
static bool exitOnException = false;
static void (*appCallback)() = nullptr;
static char *(*versionCallback)() = nullptr;

void Load_Image_Helper() {
    std::lock_guard<std::recursive_mutex> lock(symbolMutex);
    if (!symbolsReady) {
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
        symbolsReady = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    }
}
int Stack_Walk(std::uintptr_t *addresses, int capacity, CONTEXT *supplied) {
    if (!addresses || capacity <= 0) return 0;
    std::lock_guard<std::recursive_mutex> lock(symbolMutex);
    Load_Image_Helper();
    CONTEXT context{};
    if (supplied) context = *supplied;
    else RtlCaptureContext(&context);
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrPC.Mode = frame.AddrStack.Mode = frame.AddrFrame.Mode = AddrModeFlat;
    int count = 0;
    if (frame.AddrPC.Offset) addresses[count++] = static_cast<std::uintptr_t>(frame.AddrPC.Offset);
    while (count < capacity && StackWalk64(IMAGE_FILE_MACHINE_AMD64,
        GetCurrentProcess(), GetCurrentThread(), &frame, &context, nullptr,
        SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
        if (!frame.AddrPC.Offset || frame.AddrPC.Offset == addresses[count - 1]) break;
        addresses[count++] = static_cast<std::uintptr_t>(frame.AddrPC.Offset);
    }
    return count;
}
bool Lookup_Symbol(void *address, char *name, int &displacement) {
    if (!name) return false;
    std::lock_guard<std::recursive_mutex> lock(symbolMutex);
    Load_Image_Helper();
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 offset = 0;
    if (!symbolsReady || !SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(address), &offset, symbol)) {
        name[0] = 0; displacement = 0; return false;
    }
    // The legacy API promises a 256-byte symbol buffer.
    std::strncpy(name, symbol->Name, 255); name[255] = 0;
    displacement = offset > INT_MAX ? INT_MAX : static_cast<int>(offset);
    return true;
}
void Register_Thread_ID(unsigned long id, char *, bool main) { if (main) mainThread = id; }
void Unregister_Thread_ID(unsigned long id, char *) { if (mainThread == id) mainThread = 0; }
unsigned long Get_Main_Thread_ID() { return mainThread.load(); }
void Register_Application_Exception_Callback(void (*callback)()) { appCallback = callback; }
void Register_Application_Version_Callback(char *(*callback)()) { versionCallback = callback; }
void Set_Exit_On_Exception(bool set) { exitOnException = set; }
bool Is_Trying_To_Exit() { return tryingToExit.load(); }

int Exception_Handler(int code, EXCEPTION_POINTERS *info) {
    if (handling.test_and_set()) return EXCEPTION_EXECUTE_HANDLER;
    tryingToExit = true;
    const char *directory = std::getenv("GENZH_USER_DATA_DIR");
    if (!directory) directory = ".";
    char path[32768];
    std::snprintf(path, sizeof(path), "%s/CrashInfo-x64.txt", directory);
    if (FILE *file = std::fopen(path, "wb")) {
        std::fprintf(file, "Zero Hour AMD64 exception 0x%08x, PID %lu, thread %lu\nAddress %p\n",
            static_cast<unsigned>(code), GetCurrentProcessId(), GetCurrentThreadId(),
            info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr);
        if (info && info->ContextRecord) {
            const auto &c = *info->ContextRecord;
            std::fprintf(file, "RIP=%016llx RSP=%016llx RBP=%016llx\n",
                static_cast<unsigned long long>(c.Rip), static_cast<unsigned long long>(c.Rsp), static_cast<unsigned long long>(c.Rbp));
            std::uintptr_t addresses[128];
            const int count = Stack_Walk(addresses, 128, info->ContextRecord);
            for (int i = 0; i < count; ++i) std::fprintf(file, "%016llx\n", static_cast<unsigned long long>(addresses[i]));
        }
        std::fclose(file);
    }
    std::snprintf(path, sizeof(path), "%s/CrashDump-x64.dmp", directory);
    HANDLE dump = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
        std::lock_guard<std::recursive_mutex> lock(symbolMutex);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, MiniDumpNormal,
            info ? &exception : nullptr, nullptr, nullptr);
        CloseHandle(dump);
    }
    if (appCallback) appCallback();
    if (exitOnException) ExitProcess(static_cast<UINT>(code));
    return EXCEPTION_EXECUTE_HANDLER;
}
