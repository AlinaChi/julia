// This file is a part of Julia. License is MIT: http://julialang.org/license

/*
  stackwalk.c
  utilities for walking the stack and looking up information about code addresses
*/
#include <inttypes.h>
#include "julia.h"
#include "julia_internal.h"
#include "threading.h"

// define `jl_unw_get` as a macro, since (like setjmp)
// returning from the callee function will invalidate the context
#ifdef _OS_WINDOWS_
#define jl_unw_get(context) RtlCaptureContext(context)
#else
#define jl_unw_get(context) unw_getcontext(context)
#endif

#ifdef __cplusplus
extern "C" {
#endif

static int jl_unw_init(bt_cursor_t *cursor, bt_context_t *context);
static int jl_unw_step(bt_cursor_t *cursor, uintptr_t *ip, uintptr_t *sp);

size_t jl_unw_stepn(bt_cursor_t *cursor, uintptr_t *ip, uintptr_t *sp, size_t maxsize)
{
    volatile size_t n = 0;
    uintptr_t nullsp;
#if defined(_OS_WINDOWS_) && !defined(_CPU_X86_64_)
    assert(!jl_in_stackwalk);
    jl_in_stackwalk = 1;
#endif
#if !defined(_OS_WINDOWS_)
    jl_jmp_buf *old_buf = jl_safe_restore;
    jl_jmp_buf buf;
    if (!jl_setjmp(buf, 0)) {
        jl_safe_restore = &buf;
#endif
        while (1) {
           if (n >= maxsize) {
               n = maxsize; // return maxsize + 1 if ran out of space
               break;
           }
           if (!jl_unw_step(cursor, &ip[n], sp ? &sp[n] : &nullsp))
               break;
           n++;
        }
        n++;
#if !defined(_OS_WINDOWS_)
    }
    else {
        // The unwinding fails likely because a invalid memory read.
        // Back off one frame since it is likely invalid.
        // This seems to be good enough on x86 to make the LLVM debug info
        // reader happy.
        if (n > 0) n -= 1;
    }
    jl_safe_restore = old_buf;
#endif
#if defined(_OS_WINDOWS_) && !defined(_CPU_X86_64_)
    jl_in_stackwalk = 0;
#endif
    return n;
}

size_t rec_backtrace_ctx(uintptr_t *data, size_t maxsize,
                         bt_context_t *context)
{
    size_t n = 0;
    bt_cursor_t cursor;
    if (!jl_unw_init(&cursor, context))
        return 0;
    n = jl_unw_stepn(&cursor, data, NULL, maxsize);
    return n > maxsize ? maxsize : n;
}

size_t rec_backtrace(uintptr_t *data, size_t maxsize)
{
    bt_context_t context;
    memset(&context, 0, sizeof(context));
    jl_unw_get(&context);
    return rec_backtrace_ctx(data, maxsize, &context);
}

static jl_value_t *array_ptr_void_type = NULL;
JL_DLLEXPORT jl_value_t *jl_backtrace_from_here(int returnsp)
{
    jl_svec_t *tp = NULL;
    jl_array_t *ip = NULL;
    jl_array_t *sp = NULL;
    JL_GC_PUSH3(&tp, &ip, &sp);
    if (array_ptr_void_type == NULL) {
        tp = jl_svec2(jl_voidpointer_type, jl_box_long(1));
        array_ptr_void_type = jl_apply_type((jl_value_t*)jl_array_type, tp);
    }
    ip = jl_alloc_array_1d(array_ptr_void_type, 0);
    sp = returnsp ? jl_alloc_array_1d(array_ptr_void_type, 0) : NULL;
    const size_t maxincr = 1000;
    bt_context_t context;
    bt_cursor_t cursor;
    memset(&context, 0, sizeof(context));
    jl_unw_get(&context);
    if (jl_unw_init(&cursor, &context)) {
        size_t n = 0, offset = 0;
        do {
            jl_array_grow_end(ip, maxincr);
            if (returnsp) jl_array_grow_end(sp, maxincr);
            n = jl_unw_stepn(&cursor, (uintptr_t*)jl_array_data(ip) + offset,
                    returnsp ? (uintptr_t*)jl_array_data(sp) + offset : NULL, maxincr);
            offset += maxincr;
        } while (n > maxincr);
        jl_array_del_end(ip, maxincr - n);
        if (returnsp) jl_array_del_end(sp, maxincr - n);
    }
    jl_value_t *bt = returnsp ? (jl_value_t*)jl_svec2(ip, sp) : (jl_value_t*)ip;
    JL_GC_POP();
    return bt;
}

JL_DLLEXPORT jl_value_t *jl_get_backtrace(void)
{
    jl_svec_t *tp = NULL;
    jl_array_t *bt = NULL;
    JL_GC_PUSH2(&tp, &bt);
    if (array_ptr_void_type == NULL) {
        tp = jl_svec2(jl_voidpointer_type, jl_box_long(1));
        array_ptr_void_type = jl_apply_type((jl_value_t*)jl_array_type, tp);
    }
    bt = jl_alloc_array_1d(array_ptr_void_type, jl_bt_size);
    memcpy(bt->data, jl_bt_data, jl_bt_size * sizeof(void*));
    JL_GC_POP();
    return (jl_value_t*)bt;
}



#if defined(_OS_WINDOWS_)
#ifdef _CPU_X86_64_
static UNWIND_HISTORY_TABLE HistoryTable;
#else
static struct {
    DWORD64 dwAddr;
    DWORD64 ImageBase;
} HistoryTable;
#endif
static PVOID CALLBACK JuliaFunctionTableAccess64(
        _In_  HANDLE hProcess,
        _In_  DWORD64 AddrBase)
{
    //jl_printf(JL_STDOUT, "lookup %d\n", AddrBase);
#ifdef _CPU_X86_64_
    DWORD64 ImageBase;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(AddrBase, &ImageBase, &HistoryTable);
    if (fn) return fn;
    if (jl_in_stackwalk) {
        return 0;
    }
    jl_in_stackwalk = 1;
    PVOID ftable = SymFunctionTableAccess64(hProcess, AddrBase);
    jl_in_stackwalk = 0;
    return ftable;
#else
    return SymFunctionTableAccess64(hProcess, AddrBase);
#endif
}
static DWORD64 WINAPI JuliaGetModuleBase64(
        _In_  HANDLE hProcess,
        _In_  DWORD64 dwAddr)
{
    //jl_printf(JL_STDOUT, "lookup base %d\n", dwAddr);
#ifdef _CPU_X86_64_
    DWORD64 ImageBase;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(dwAddr, &ImageBase, &HistoryTable);
    if (fn) return ImageBase;
    if (jl_in_stackwalk) {
        return 0;
    }
    jl_in_stackwalk = 1;
    DWORD64 fbase = SymGetModuleBase64(hProcess, dwAddr);
    jl_in_stackwalk = 0;
    return fbase;
#else
    if (dwAddr == HistoryTable.dwAddr) return HistoryTable.ImageBase;
    DWORD64 ImageBase = jl_getUnwindInfo(dwAddr);
    if (ImageBase) {
        HistoryTable.dwAddr = dwAddr;
        HistoryTable.ImageBase = ImageBase;
        return ImageBase;
    }
    return SymGetModuleBase64(hProcess, dwAddr);
#endif
}

int needsSymRefreshModuleList;
BOOL (WINAPI *hSymRefreshModuleList)(HANDLE);
static int jl_unw_init(bt_cursor_t *cursor, bt_context_t *Context)
{
    // Might be called from unmanaged thread.
    if (needsSymRefreshModuleList && hSymRefreshModuleList != 0 && !jl_in_stackwalk) {
        jl_in_stackwalk = 1;
        hSymRefreshModuleList(GetCurrentProcess());
        jl_in_stackwalk = 0;
        needsSymRefreshModuleList = 0;
    }
#if !defined(_CPU_X86_64_)
    if (jl_in_stackwalk) {
        return 0;
    }
    jl_in_stackwalk = 1;
    memset(&cursor->stackframe, 0, sizeof(cursor->stackframe));
    cursor->stackframe.AddrPC.Offset = Context->Eip;
    cursor->stackframe.AddrStack.Offset = Context->Esp;
    cursor->stackframe.AddrFrame.Offset = Context->Ebp;
    cursor->stackframe.AddrPC.Mode = AddrModeFlat;
    cursor->stackframe.AddrStack.Mode = AddrModeFlat;
    cursor->stackframe.AddrFrame.Mode = AddrModeFlat;
    cursor->context = *Context;
    BOOL result = StackWalk64(IMAGE_FILE_MACHINE_I386, GetCurrentProcess(), hMainThread,
        &cursor->stackframe, &cursor->context, NULL, JuliaFunctionTableAccess64, JuliaGetModuleBase64, NULL);
    jl_in_stackwalk = 0;
    return result;
#else
    *cursor = *Context;
    return 1;
#endif
}

static int jl_unw_step(bt_cursor_t *cursor, uintptr_t *ip, uintptr_t *sp)
{
    // Might be called from unmanaged thread.
#ifndef _CPU_X86_64_
    *ip = (uintptr_t)cursor->stackframe.AddrPC.Offset;
    *sp = (uintptr_t)cursor->stackframe.AddrStack.Offset;
    BOOL result = StackWalk64(IMAGE_FILE_MACHINE_I386, GetCurrentProcess(), hMainThread,
        &cursor->stackframe, &cursor->context, NULL, JuliaFunctionTableAccess64, JuliaGetModuleBase64, NULL);
    return result;
#else
    *ip = (uintptr_t)cursor->Rip;
    *sp = (uintptr_t)cursor->Rsp;
    DWORD64 ImageBase = JuliaGetModuleBase64(GetCurrentProcess(), cursor->Rip);
    if (!ImageBase)
        return 0;

    PRUNTIME_FUNCTION FunctionEntry = (PRUNTIME_FUNCTION)JuliaFunctionTableAccess64(GetCurrentProcess(), cursor->Rip);
    if (!FunctionEntry) { // assume this is a NO_FPO RBP-based function
        MEMORY_BASIC_INFORMATION mInfo;

        cursor->Rsp = cursor->Rbp;                 // MOV RSP, RBP

        // Check whether the pointer is valid and executable before dereferencing
        // to avoid segfault while recording. See #10638.
        if (VirtualQuery((LPCVOID)cursor->Rsp, &mInfo, sizeof(MEMORY_BASIC_INFORMATION)) == 0)
            return 0;
        DWORD X = mInfo.AllocationProtect;
        if (!((X&PAGE_READONLY) || (X&PAGE_READWRITE) || (X&PAGE_WRITECOPY) || (X&PAGE_EXECUTE_READ)) ||
              (X&PAGE_GUARD) || (X&PAGE_NOACCESS))
            return 0;

        cursor->Rbp = *(DWORD64*)cursor->Rsp;      // POP RBP
        cursor->Rsp = cursor->Rsp + sizeof(void*);
        cursor->Rip = *(DWORD64*)cursor->Rsp;      // POP RIP (aka RET)
        cursor->Rsp = cursor->Rsp + sizeof(void*);
    }
    else {
        PVOID HandlerData;
        DWORD64 EstablisherFrame;
        (void)RtlVirtualUnwind(
                0 /*UNW_FLAG_NHANDLER*/,
                ImageBase,
                cursor->Rip,
                FunctionEntry,
                cursor,
                &HandlerData,
                &EstablisherFrame,
                NULL);
    }
    return cursor->Rip != 0;
#endif
}

#elif defined(_CPU_ARM_)
// platforms on which libunwind may be broken

static int jl_unw_init(bt_cursor_t *cursor, bt_context_t *context) { return 0; }
static int jl_unw_step(bt_cursor_t *cursor, uintptr_t *ip, uintptr_t *sp) { return 0; }

#else
// stacktrace using libunwind

static int jl_unw_init(bt_cursor_t *cursor, bt_context_t *context)
{
    return unw_init_local(cursor, context) == 0;
}

static int jl_unw_step(bt_cursor_t *cursor, uintptr_t *ip, uintptr_t *sp)
{
    unw_word_t reg;
    if (unw_get_reg(cursor, UNW_REG_IP, &reg) < 0)
        return 0;
    *ip = reg;
    if (unw_get_reg(cursor, UNW_REG_SP, &reg) < 0)
        return 0;
    *sp = reg;
    return unw_step(cursor) > 0;
}

#ifdef LIBOSXUNWIND
int jl_unw_init_dwarf(bt_cursor_t *cursor, bt_context_t *uc)
{
    return unw_init_local_dwarf(cursor, uc) != 0;
}
size_t rec_backtrace_ctx_dwarf(uintptr_t *data, size_t maxsize,
                               bt_context_t *context)
{
    size_t n;
    bt_cursor_t cursor;
    if (!jl_unw_init_dwarf(&cursor, context))
        return 0;
    n = jl_unw_stepn(&cursor, data, NULL, maxsize);
    return n > maxsize ? maxsize : n;
}
#endif
#endif

// Always Set *func_name and *file_name to malloc'd pointers (non-NULL)
/*static int frame_info_from_ip(char **func_name,
                              char **file_name, size_t *line_num,
                              char **inlinedat_file, size_t *inlinedat_line,
                              jl_lambda_info_t **outer_linfo,
                              size_t ip, int skipC, int skipInline)
{
    // This function is not allowed to reference any TLS variables since
    // it can be called from an unmanaged thread on OSX.
    static const char *name_unknown = "???";
    int fromC = 0;

    jl_getFunctionInfo(func_name, file_name, line_num, inlinedat_file, inlinedat_line, outer_linfo,
            ip, &fromC, skipC, skipInline);
    if (!*func_name) {
        *func_name = strdup(name_unknown);
        *line_num = ip;
    }
    if (!*file_name) {
        *file_name = strdup(name_unknown);
    }
    return fromC;
    }*/

JL_DLLEXPORT jl_value_t *jl_lookup_code_address(void *ip, int skipC)
{
    jl_frame_t *frames = NULL;
    int8_t gc_state = jl_gc_safe_enter();
    int n = jl_getFunctionInfo(&frames, (uintptr_t)ip, skipC, 0);
    jl_gc_safe_leave(gc_state);
    jl_value_t *rs = (jl_value_t*)jl_alloc_svec(n);
    JL_GC_PUSH1(&rs);
    for (int i = 0; i < n; i++) {
        jl_frame_t frame = frames[i];
        jl_value_t *r = (jl_value_t*)jl_alloc_svec(7);
        jl_svecset(rs, i, r);
        if (frame.func_name)
            jl_svecset(r, 0, jl_symbol(frame.func_name));
        else
            jl_svecset(r, 0, empty_sym);
        free(frame.func_name);
        if (frame.file_name)
            jl_svecset(r, 1, jl_symbol(frame.file_name));
        else
            jl_svecset(r, 1, empty_sym);
        free(frame.file_name);
        jl_svecset(r, 2, jl_box_long(frame.line));
        jl_svecset(r, 3, frame.linfo != NULL ? (jl_value_t*)frame.linfo : jl_nothing);
        jl_svecset(r, 4, jl_box_bool(frame.fromC));
        jl_svecset(r, 5, jl_box_bool(frame.inlined));
        jl_svecset(r, 6, jl_box_long((intptr_t)ip));
    }
    free(frames);
    JL_GC_POP();
    return rs;
}

//for looking up functions from gdb:
JL_DLLEXPORT void jl_gdblookup(uintptr_t ip)
{
    // This function is not allowed to reference any TLS variables since
    // it can be called from an unmanaged thread on OSX.
    // it means calling getFunctionInfo with noInline = 1
    jl_frame_t *frames = NULL;
    int n = jl_getFunctionInfo(&frames, ip, 0, 1);
    int i;

    for(i=0; i < n; i++) {
        jl_frame_t frame = frames[i];
        if (!frame.func_name) {
            jl_safe_printf("unknown function (ip: %p)\n", (void*)ip);
        }
        else {
            if (frame.line != -1) {
                jl_safe_printf("%s at %s:%" PRIuPTR "\n", frame.func_name, frame.file_name, (uintptr_t)frame.line);
            }
            else {
                jl_safe_printf("%s at %s (unknown line)\n", frame.func_name, frame.file_name);
            }
            free(frame.func_name);
            free(frame.file_name);
        }
    }
    free(frames);
}

JL_DLLEXPORT void jlbacktrace(void)
{
    size_t n = jl_bt_size; // jl_bt_size > 40 ? 40 : jl_bt_size;
    for (size_t i=0; i < n; i++)
        jl_gdblookup(jl_bt_data[i] - 1);
}

#ifdef __cplusplus
}
#endif
