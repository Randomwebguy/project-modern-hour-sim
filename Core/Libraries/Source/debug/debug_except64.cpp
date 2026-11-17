// SPDX-License-Identifier: GPL-3.0-or-later
#include "debug.h"
#include "internal_except.h"
#include "WWLib/Except.h"
#include <cstdio>

DebugExceptionhandler::DebugExceptionhandler() {}
const char *DebugExceptionhandler::GetExceptionType(EXCEPTION_POINTERS *info,char *explanation) {
    std::sprintf(explanation,"AMD64 exception 0x%08lx at %p",info->ExceptionRecord->ExceptionCode,info->ExceptionRecord->ExceptionAddress);
    return "AMD64_EXCEPTION";
}
void DebugExceptionhandler::LogExceptionLocation(Debug &dbg, EXCEPTION_POINTERS *info) {
    char text[512]; DebugStackwalk::Signature::GetSymbol(info->ContextRecord->Rip,text,sizeof(text)); dbg << text;
}
void DebugExceptionhandler::LogRegisters(Debug &dbg,EXCEPTION_POINTERS *info) {
    const auto &c=*info->ContextRecord;
    dbg << "RIP=" << Debug::Hex() << c.Rip << " RSP=" << c.Rsp << " RBP=" << c.Rbp << Debug::Dec();
}
void DebugExceptionhandler::LogFPURegisters(Debug &dbg,EXCEPTION_POINTERS *info) { dbg << "MXCSR=" << Debug::Hex() << info->ContextRecord->MxCsr << Debug::Dec(); }
LONG __stdcall DebugExceptionhandler::ExceptionFilter(EXCEPTION_POINTERS *info) {
    return Exception_Handler(info->ExceptionRecord->ExceptionCode,info);
}
