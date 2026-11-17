// SPDX-License-Identifier: GPL-3.0-or-later
#include "debug.h"
#include "debug_stack.h"
#include "WWLib/Except.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

DebugStackwalk::DebugStackwalk() { Load_Image_Helper(); }
DebugStackwalk::~DebugStackwalk() {}
void *DebugStackwalk::GetDbghelpHandle() { Load_Image_Helper(); return GetModuleHandleA("dbghelp.dll"); }
bool DebugStackwalk::IsOldDbghelp() { return false; }
DebugStackwalk::Signature::Signature(const Signature &s) { *this = s; }
DebugStackwalk::Signature &DebugStackwalk::Signature::operator=(const Signature &s) {
    if (this != &s) { m_numAddr = s.m_numAddr; std::memcpy(m_addr, s.m_addr, m_numAddr * sizeof(*m_addr)); }
    return *this;
}
DebugAddress DebugStackwalk::Signature::GetAddress(int n) const { return n >= 0 && static_cast<unsigned>(n) < m_numAddr ? m_addr[n] : 0; }
int DebugStackwalk::StackWalk(Signature &sig, CONTEXT *ctx) {
    sig.m_numAddr = Stack_Walk(sig.m_addr, Signature::MAX_ADDR, ctx);
    return static_cast<int>(sig.m_numAddr);
}
void DebugStackwalk::Signature::GetSymbol(DebugAddress addr, char *buf, unsigned size) {
    if (!buf || !size) return;
    char symbol[256]; int offset=0;
    Lookup_Symbol(reinterpret_cast<void *>(addr), symbol, offset);
    std::snprintf(buf, size, "%016llx %s+%x", static_cast<unsigned long long>(addr), symbol, offset);
}
void DebugStackwalk::Signature::GetSymbol(DebugAddress addr,
    char *mod, unsigned modSize, unsigned *modOffset,
    char *sym, unsigned symSize, unsigned *symOffset,
    char *file, unsigned fileSize, unsigned *line, unsigned *lineOffset) {
    if (mod && modSize) mod[0]=0;
    if (sym && symSize) sym[0]=0;
    if (file && fileSize) file[0]=0;
    if (line) *line=0;
    if (lineOffset) *lineOffset=0;
    if (modOffset) *modOffset=0;
    if (symOffset) *symOffset=0;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<void *>(addr), &memory, sizeof(memory))) {
        if (mod && modSize) GetModuleFileNameA(static_cast<HMODULE>(memory.AllocationBase), mod, modSize);
        if (modOffset) *modOffset=static_cast<unsigned>(addr - reinterpret_cast<DebugAddress>(memory.AllocationBase));
    }
    char symbol[256]; int offset=0;
    if (Lookup_Symbol(reinterpret_cast<void *>(addr), symbol, offset)) {
        if (sym && symSize) { std::strncpy(sym,symbol,symSize-1); sym[symSize-1]=0; }
        if (symOffset) *symOffset=static_cast<unsigned>(offset);
    }
}
Debug &operator<<(Debug &dbg, const DebugStackwalk::Signature &sig) {
    for (unsigned i=0;i<sig.Size();++i) { char text[512]; sig.GetSymbol(sig.GetAddress(i),text,sizeof(text)); dbg << text << "\n"; }
    return dbg;
}
