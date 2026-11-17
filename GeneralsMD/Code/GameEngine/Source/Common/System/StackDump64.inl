// SPDX-License-Identifier: GPL-3.0-or-later
#include "WWLib/Except.h"
#include <cstdio>
#include <cstring>
AsciiString g_LastErrorDump;
static void ModernStackLine(void *address,void (*callback)(const char *)) {
    char name[256],line[512];int displacement=0;
    Lookup_Symbol(address,name,displacement);
    std::snprintf(line,sizeof(line),"%p %s+0x%x\n",address,name,displacement);
    if (callback) callback(line); else OutputDebugStringA(line);
}
void FillStackAddresses(void **addresses,unsigned count,unsigned skip) {
    if (!addresses) return;
    for(unsigned i=0;i<count;++i) addresses[i]=nullptr;
    CaptureStackBackTrace(skip+1,count,addresses,nullptr);
}
void StackDumpFromAddresses(void **addresses,unsigned count,void (*callback)(const char *)) {
    for(unsigned i=0;i<count && addresses[i];++i) ModernStackLine(addresses[i],callback);
}
void StackDump(void (*callback)(const char *)) {
    void *addresses[128];FillStackAddresses(addresses,128,1);StackDumpFromAddresses(addresses,128,callback);
}
void StackDumpFromContext(DWORD_PTR rip,DWORD_PTR rsp,DWORD_PTR rbp,void (*callback)(const char *)) {
    CONTEXT context{};RtlCaptureContext(&context);context.Rip=rip;context.Rsp=rsp;context.Rbp=rbp;
    std::uintptr_t addresses[128];int count=Stack_Walk(addresses,128,&context);
    for(int i=0;i<count;++i)ModernStackLine(reinterpret_cast<void *>(addresses[i]),callback);
}
void GetFunctionDetails(void *pointer,char *name,char *file,unsigned *line,unsigned *address) {
    int offset=0;Lookup_Symbol(pointer,name,offset);
    if(file) file[0]=0;if(line)*line=0;
    // Legacy consumers request a module-relative 32-bit offset, not a pointer.
    if(address)*address=static_cast<unsigned>(offset);
}
void DumpExceptionInfo(unsigned code,EXCEPTION_POINTERS *info) { Exception_Handler(code,info); }
