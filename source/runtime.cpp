/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The original code is copyright (c) 2026, 1therealcloud.
 */

#include "runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <cstdio>
#include <new>

#if defined(_WIN32) && defined(_MSC_VER)
#   include <windows.h>
#elif defined(__linux__)
#   include <setjmp.h>
#   include <signal.h>
#   include <ucontext.h>
#   include <sys/mman.h>
#   include <unistd.h>
#endif

PawnJITRuntime g_pawnJITRuntime;

namespace
{
    unsigned char* GetData(AMX* amx)
    {
        if (amx == nullptr || amx->base == nullptr)
            return nullptr;

        if (amx->data != nullptr)
            return amx->data;

        auto* hdr = reinterpret_cast<AMX_HEADER*>(amx->base);
        return amx->base + hdr->dat;
    }

    bool CheckAddressStart(const JitContext* context, cell address)
    {
        if (context == nullptr || context->data == nullptr)
            return false;

        const ucell value = static_cast<ucell>(address);
        const ucell hea = static_cast<ucell>(context->hea);
        const ucell stk = static_cast<ucell>(context->stk);
        const ucell stp = static_cast<ucell>(context->stp);
        return value < stp && !(value >= hea && value < stk);
    }

    bool CheckPortableEndpoints(const JitContext* context, cell address, cell bytes)
    {
        if (context == nullptr || bytes < 0 || !CheckAddressStart(context, address))
            return false;

        const uint64_t end64 = static_cast<uint64_t>(static_cast<ucell>(address)) + static_cast<uint64_t>(static_cast<ucell>(bytes));
        if (end64 > std::numeric_limits<ucell>::max())
            return false;

        const ucell end = static_cast<ucell>(end64);
        const ucell hea = static_cast<ucell>(context->hea);
        const ucell stk = static_cast<ucell>(context->stk);
        const ucell stp = static_cast<ucell>(context->stp);

        if (end > stp)
            return false;
        if (end > hea && end < stk)
            return false;
        return true;
    }

    bool IsCellAligned(cell value)
    {
        return (static_cast<ucell>(value) & static_cast<ucell>(sizeof(cell) - 1)) == 0;
    }

    void SyncForExternalCall(JitContext* context, cell cip)
    {
        AMX* amx = context->amx;
        amx->cip = cip;
        amx->frm = context->frm;
        amx->stk = context->stk;
        amx->hea = context->hea;
    }

    /*
        amx_Exec is normally re-entered synchronously from a native on the
        same thread. keep the currently executing AMX/state/context in TLS
        so that a nested public call can bypass the global unordered_map
        lookup and the top-level compilation/invalidation path. void*
        avoids exposing the private PawnJITRuntime::State type outside its
        member functions.
    */
    thread_local AMX* g_activeAmx = nullptr;
    thread_local void* g_activeState = nullptr;
    thread_local JitContext* g_activeContext = nullptr;

    struct AmxExecutionSnapshot
    {
        cell pri = 0;
        cell alt = 0;
        cell cip = 0;
        cell frm = 0;
        cell stk = 0;
        cell hea = 0;
        cell resetStk = 0;
        cell resetHea = 0;
        int error = AMX_ERR_NONE;
        int paramcount = 0;
    };

    AmxExecutionSnapshot CaptureAmxState(const AMX* amx)
    {
        AmxExecutionSnapshot snapshot;
        if (amx == nullptr)
            return snapshot;

        snapshot.pri = amx->pri;
        snapshot.alt = amx->alt;
        snapshot.cip = amx->cip;
        snapshot.frm = amx->frm;
        snapshot.stk = amx->stk;
        snapshot.hea = amx->hea;
        snapshot.resetStk = amx->reset_stk;
        snapshot.resetHea = amx->reset_hea;
        snapshot.error = amx->error;
        snapshot.paramcount = amx->paramcount;
        return snapshot;
    }

    void RestoreAmxState(AMX* amx, const AmxExecutionSnapshot& snapshot)
    {
        if (amx == nullptr)
            return;

        amx->pri = snapshot.pri;
        amx->alt = snapshot.alt;
        amx->cip = snapshot.cip;
        amx->frm = snapshot.frm;
        amx->stk = snapshot.stk;
        amx->hea = snapshot.hea;
        amx->reset_stk = snapshot.resetStk;
        amx->reset_hea = snapshot.resetHea;
        amx->error = snapshot.error;
        amx->paramcount = snapshot.paramcount;
    }

    int MapCpuFaultToAmxError(uint32_t code)
    {
#if defined(_WIN32) && defined(_MSC_VER)
        switch (code)
        {
            case EXCEPTION_ACCESS_VIOLATION:
            case EXCEPTION_IN_PAGE_ERROR:
            case EXCEPTION_DATATYPE_MISALIGNMENT:
            case EXCEPTION_GUARD_PAGE:
                return AMX_ERR_MEMACCESS;
            case EXCEPTION_INT_DIVIDE_BY_ZERO:
            case EXCEPTION_INT_OVERFLOW:
                return AMX_ERR_DIVIDE;
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
                return AMX_ERR_BOUNDS;
            case EXCEPTION_ILLEGAL_INSTRUCTION:
            case EXCEPTION_PRIV_INSTRUCTION:
                return AMX_ERR_INVINSTR;
            default:
                return AMX_ERR_NATIVE;
        }
#elif defined(__linux__)
        switch (static_cast<int>(code))
        {
            case SIGSEGV:
            case SIGBUS:
                return AMX_ERR_MEMACCESS;
            case SIGFPE:
                return AMX_ERR_DIVIDE;
            case SIGILL:
                return AMX_ERR_INVINSTR;
            default:
                return AMX_ERR_NATIVE;
        }
#else
        (void)code;
        return AMX_ERR_NATIVE;
#endif
    }

    struct GuardedEntryResult
    {
        int error = AMX_ERR_NONE;
        uint32_t faultCode = 0;
        bool faulted = false;
    };

#if defined(_WIN32) && defined(_MSC_VER)
    thread_local bool g_internalMemoryHelperFaultScope = false;
#elif defined(__linux__)
    thread_local volatile sig_atomic_t g_internalMemoryHelperFaultScope = 0;
#endif

    void BeginInternalMemoryFaultScope() noexcept
    {
#if defined(_WIN32) && defined(_MSC_VER)
        g_internalMemoryHelperFaultScope = true;
#elif defined(__linux__)
        g_internalMemoryHelperFaultScope = 1;
#endif
    }

    void EndInternalMemoryFaultScope() noexcept
    {
#if defined(_WIN32) && defined(_MSC_VER)
        g_internalMemoryHelperFaultScope = false;
#elif defined(__linux__)
        g_internalMemoryHelperFaultScope = 0;
#endif
    }

#if defined(_WIN32) && defined(_MSC_VER)
    int FilterGeneratedCodeFault(EXCEPTION_POINTERS* pointers, const CompiledProgram* program, uint32_t* faultCode)
    {
        if (pointers == nullptr || pointers->ExceptionRecord == nullptr || program == nullptr || program->nativeCode == nullptr || program->nativeCodeSize == 0)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        const uintptr_t address = reinterpret_cast<uintptr_t>(pointers->ExceptionRecord->ExceptionAddress);
        const uintptr_t begin = reinterpret_cast<uintptr_t>(program->nativeCode);
        const uintptr_t end = begin + program->nativeCodeSize;
        const bool generatedFault = end >= begin && address >= begin && address < end;
        if (!generatedFault && !g_internalMemoryHelperFaultScope)
            return EXCEPTION_CONTINUE_SEARCH;

        const uint32_t code = pointers->ExceptionRecord->ExceptionCode;
        switch (code)
        {
            case EXCEPTION_ACCESS_VIOLATION:
            case EXCEPTION_IN_PAGE_ERROR:
            case EXCEPTION_DATATYPE_MISALIGNMENT:
            case EXCEPTION_GUARD_PAGE:
            case EXCEPTION_INT_DIVIDE_BY_ZERO:
            case EXCEPTION_INT_OVERFLOW:
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            case EXCEPTION_ILLEGAL_INSTRUCTION:
            case EXCEPTION_PRIV_INSTRUCTION:
                g_internalMemoryHelperFaultScope = false;
                if (faultCode != nullptr)
                    *faultCode = code;
                return EXCEPTION_EXECUTE_HANDLER;
            default:
                return EXCEPTION_CONTINUE_SEARCH;
        }
    }
#endif

#if defined(__linux__)
    struct PosixFaultGuard
    {
        sigjmp_buf jump;
        const CompiledProgram* program = nullptr;
        PosixFaultGuard* previous = nullptr;
        volatile sig_atomic_t active = 0;
        volatile sig_atomic_t signalNumber = 0;
    };

    struct PosixSignalSlot
    {
        int signalNumber = 0;
        struct sigaction previous{};
        bool installed = false;
    };

    thread_local PosixFaultGuard* g_posixFaultGuard = nullptr;
    PosixSignalSlot g_posixSignalSlots[] = {
        {SIGSEGV, {}, false},
        {SIGBUS, {}, false},
        {SIGFPE, {}, false},
        {SIGILL, {}, false},
    };
    bool g_posixFaultHandlersInstalled = false;

    struct PosixAltStackState
    {
        void* memory = nullptr;
        std::size_t size = 0;
        bool ownsStack = false;
        bool ready = false;

        ~PosixAltStackState()
        {
            if (!ownsStack || memory == nullptr)
                return;

            stack_t current{};
            if (sigaltstack(nullptr, &current) == 0 && (current.ss_flags & SS_DISABLE) == 0 && current.ss_sp == memory)
            {
                stack_t disabled{};
                disabled.ss_flags = SS_DISABLE;
                sigaltstack(&disabled, nullptr);
            }

            munmap(memory, size);
        }
    };

    thread_local PosixAltStackState g_posixAltStack;

    bool EnsurePosixAltSignalStack()
    {
        /*
            do not trust a one-time TLS "ready" bit. sigaltstack() is
            per-thread mutable process state: a native/plugin may replace
            or disable it between two PawnJIT entries. re-read the kernel
            state every time before generated code is allowed to borrow
            the Pawn stack as ESP.
        */
        stack_t current{};
        if (sigaltstack(nullptr, &current) != 0)
            return false;

        const std::size_t minimum = static_cast<std::size_t>(MINSIGSTKSZ);
        if ((current.ss_flags & SS_DISABLE) == 0)
        {
            if (current.ss_sp == nullptr || current.ss_size < minimum)
                return false;

            /*
                respect an alternate stack installed by the host or
                another component. SA_ONSTACK will use it and PawnJIT
                need not own it. if it is our stack, this also validates
                that it is still active.
            */
            g_posixAltStack.ready = true;
            return true;
        }

        /*
            our previously allocated stack may have been disabled
            temporarily. reuse the same mapping instead of leaking a
            fresh mapping per change.
        */
        if (g_posixAltStack.ownsStack && g_posixAltStack.memory != nullptr && g_posixAltStack.size >= minimum)
        {
            stack_t alternate{};
            alternate.ss_sp = g_posixAltStack.memory;
            alternate.ss_size = g_posixAltStack.size;
            alternate.ss_flags = 0;
            if (sigaltstack(&alternate, nullptr) != 0)
                return false;

            g_posixAltStack.ready = true;
            return true;
        }

        constexpr std::size_t kPawnJITAltStackBytes = 64u * 1024u;
        const std::size_t size = kPawnJITAltStackBytes > minimum ? kPawnJITAltStackBytes : minimum;
        void* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED)
            return false;

        stack_t alternate{};
        alternate.ss_sp = memory;
        alternate.ss_size = size;
        alternate.ss_flags = 0;
        if (sigaltstack(&alternate, nullptr) != 0)
        {
            munmap(memory, size);
            return false;
        }

        g_posixAltStack.memory = memory;
        g_posixAltStack.size = size;
        g_posixAltStack.ownsStack = true;
        g_posixAltStack.ready = true;
        return true;
    }

    uintptr_t PosixInstructionPointer(void* rawContext)
    {
        if (rawContext == nullptr)
            return 0;

        auto* context = static_cast<ucontext_t*>(rawContext);
#   if defined(__i386__) && defined(REG_EIP)
        return static_cast<uintptr_t>(context->uc_mcontext.gregs[REG_EIP]);
#   elif defined(__x86_64__) && defined(REG_RIP)
        return static_cast<uintptr_t>(context->uc_mcontext.gregs[REG_RIP]);
#   else
        (void)context;
        return 0;
#   endif
    }

    bool IsGeneratedInstructionPointer(const CompiledProgram* program, uintptr_t address)
    {
        if (program == nullptr || program->nativeCode == nullptr || program->nativeCodeSize == 0)
            return false;

        const uintptr_t begin = reinterpret_cast<uintptr_t>(program->nativeCode);
        if (program->nativeCodeSize > std::numeric_limits<uintptr_t>::max() - begin)
            return false;

        const uintptr_t end = begin + program->nativeCodeSize;
        return address >= begin && address < end;
    }

    PosixSignalSlot* PosixSignalState(int signalNumber)
    {
        for (PosixSignalSlot& slot : g_posixSignalSlots)
        {
            if (slot.signalNumber == signalNumber)
                return &slot;
        }
        return nullptr;
    }

    void ForwardPosixSignal(int signalNumber, siginfo_t* info, void* rawContext)
    {
        PosixSignalSlot* slot = PosixSignalState(signalNumber);
        if (slot == nullptr)
            return;

        /*
            copy the action because SA_RESETHAND changes the stored
            disposition before invoking the one-shot handler, matching
            kernel semantics while PawnJIT keeps ownership of the process
            signal entry itself.
        */
        const struct sigaction previous = slot->previous;
        if (previous.sa_handler == SIG_IGN)
            return;

        if (previous.sa_handler != SIG_DFL && previous.sa_handler != nullptr)
        {
            if ((previous.sa_flags & SA_RESETHAND) != 0)
            {
                struct sigaction reset{};
                sigemptyset(&reset.sa_mask);
                reset.sa_handler = SIG_DFL;
                slot->previous = reset;
            }

            /*
                the kernel entered PawnJIT using PawnJIT's own sa_mask.
                recreate the mask that would have been active for the
                previous handler so SA_NODEFER and its custom mask retain
                their observable meaning.
            */
            sigset_t savedMask{};
            bool changedMask = false;
            if (rawContext != nullptr)
            {
                auto* context = static_cast<ucontext_t*>(rawContext);
                sigset_t desired = context->uc_sigmask;
                for (int sig = 1; sig < NSIG; ++sig)
                {
                    if (sigismember(&previous.sa_mask, sig) == 1)
                        sigaddset(&desired, sig);
                }
                if ((previous.sa_flags & SA_NODEFER) == 0)
                    sigaddset(&desired, signalNumber);
                changedMask = sigprocmask(SIG_SETMASK, &desired, &savedMask) == 0;
            }

            if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr)
                previous.sa_sigaction(signalNumber, info, rawContext);
            else
                previous.sa_handler(signalNumber);

            if (changedMask)
                sigprocmask(SIG_SETMASK, &savedMask, nullptr);
            return;
        }

        /*
            preserve the process' original default disposition for
            unrelated faults. sigaction()/raise() are async-signal-safe on
            POSIX systems. re-install PawnJIT only if raise unexpectedly
            returns (for example if the signal was blocked by an exotic
            host configuration).
        */
        struct sigaction pawnAction{};
        if (sigaction(signalNumber, nullptr, &pawnAction) != 0)
            return;
        if (sigaction(signalNumber, &previous, nullptr) != 0)
            return;

        /*
            the kernel normally blocks the currently delivered signal
            while its handler runs. a plain raise() here could therefore
            only queue the signal, after which reinstalling PawnJIT would
            accidentally intercept it again. temporarily unblock just this
            signal so SIG_DFL receives the re-raise immediately, exactly
            as if PawnJIT were not installed.
        */
        sigset_t oneSignal{};
        sigset_t savedMask{};
        sigemptyset(&oneSignal);
        sigaddset(&oneSignal, signalNumber);
        const bool changedMask = sigprocmask(SIG_UNBLOCK, &oneSignal, &savedMask) == 0;
        raise(signalNumber);

        /*
            fault signals used by PawnJIT have terminating default
            dispositions, so reaching here is exceptional (for example an
            unusual blocked/host configuration). restore process state
            rather than leaving our signal hook detached.
        */
        if (changedMask)
            sigprocmask(SIG_SETMASK, &savedMask, nullptr);
        sigaction(signalNumber, &pawnAction, nullptr);
    }

    void PawnJITPosixSignalHandler(int signalNumber, siginfo_t* info, void* rawContext)
    {
        PosixFaultGuard* guard = g_posixFaultGuard;
        const bool synchronousFault = info != nullptr && info->si_code > 0;
        const bool internalHelperFault = g_internalMemoryHelperFaultScope != 0;
        if (guard != nullptr && guard->active != 0 && synchronousFault &&
            (internalHelperFault || IsGeneratedInstructionPointer(guard->program, PosixInstructionPointer(rawContext))))
        {
            g_internalMemoryHelperFaultScope = 0;
            guard->signalNumber = signalNumber;
            guard->active = 0;
            siglongjmp(guard->jump, 1);
        }

        /*
            do not leave TLS pointing at a stack guard if a pre-existing
            signal handler performs its own non-local jump. if it returns
            normally, restore the current guard and let generated
            execution continue.
        */
        if (guard != nullptr)
            g_posixFaultGuard = guard->previous;
        ForwardPosixSignal(signalNumber, info, rawContext);
        if (guard != nullptr)
            g_posixFaultGuard = guard;
    }

    bool InstallPosixFaultHandlers()
    {
        if (g_posixFaultHandlersInstalled)
            return true;

        std::size_t installedCount = 0;
        for (PosixSignalSlot& slot : g_posixSignalSlots)
        {
            struct sigaction previous{};
            if (sigaction(slot.signalNumber, nullptr, &previous) != 0)
                goto rollback;

            struct sigaction action{};
            sigemptyset(&action.sa_mask);
            action.sa_sigaction = &PawnJITPosixSignalHandler;
            action.sa_flags = SA_SIGINFO | SA_ONSTACK;
            /*
                preserve restart behaviour for unrelated signals that are
                merely forwarded through PawnJIT's process-level handler.
            */
            if ((previous.sa_flags & SA_RESTART) != 0)
                action.sa_flags |= SA_RESTART;

            if (sigaction(slot.signalNumber, &action, nullptr) != 0)
                goto rollback;

            slot.previous = previous;
            slot.installed = true;
            ++installedCount;
        }

        g_posixFaultHandlersInstalled = true;
        return true;

    rollback:
        for (std::size_t i = 0; i < installedCount; ++i)
        {
            PosixSignalSlot& installed = g_posixSignalSlots[i];
            sigaction(installed.signalNumber, &installed.previous, nullptr);
            installed.installed = false;
        }
        return false;
    }

    void UninstallPosixFaultHandlers()
    {
        if (!g_posixFaultHandlersInstalled)
            return;

        for (PosixSignalSlot& slot : g_posixSignalSlots)
        {
            if (!slot.installed)
                continue;

            struct sigaction current{};
            if (sigaction(slot.signalNumber, nullptr, &current) == 0)
            {
                const bool stillOwned = (current.sa_flags & SA_SIGINFO) != 0 && current.sa_sigaction == &PawnJITPosixSignalHandler;
                if (stillOwned)
                    sigaction(slot.signalNumber, &slot.previous, nullptr);
            }

            slot.installed = false;
        }

        g_posixFaultHandlersInstalled = false;
    }
#endif

    GuardedEntryResult RunGeneratedCodeGuarded(const CompiledProgram& program, JitContext* context)
    {
        GuardedEntryResult result;
#if defined(_WIN32) && defined(_MSC_VER)
        __try
        {
            result.error = program.entry(context);
        }
        __except (FilterGeneratedCodeFault(GetExceptionInformation(), &program, &result.faultCode))
        {
            result.faulted = true;
            result.error = MapCpuFaultToAmxError(result.faultCode);
        }
#elif defined(__linux__)
        PosixFaultGuard guard{};
        guard.program = &program;
        PosixFaultGuard* const previousGuard = g_posixFaultGuard;
        guard.previous = previousGuard;
        g_posixFaultGuard = &guard;

        if (sigsetjmp(guard.jump, 1) == 0)
        {
            guard.active = 1;
            result.error = program.entry(context);
            guard.active = 0;
        }
        else
        {
            guard.active = 0;
            result.faulted = true;
            result.faultCode = static_cast<uint32_t>(guard.signalNumber);
            result.error = MapCpuFaultToAmxError(result.faultCode);
        }

        g_posixFaultGuard = previousGuard;
#else
        result.error = program.entry(context);
#endif
        return result;
    }

}

bool InitializePawnJITFaultContainment()
{
#if defined(__linux__)
    if (!InstallPosixFaultHandlers())
        return false;
    if (!EnsurePosixAltSignalStack())
    {
        UninstallPosixFaultHandlers();
        return false;
    }
    return true;
#else
    return true;
#endif
}

bool PreparePawnJITThreadFaultContainment()
{
#if defined(__linux__)
    return g_posixFaultHandlersInstalled && EnsurePosixAltSignalStack();
#else
    return true;
#endif
}

void ShutdownPawnJITFaultContainment()
{
#if defined(__linux__)
    UninstallPosixFaultHandlers();
#endif
}

int JitCallNative(JitContext* context, cell index, cell nextCip)
{
    if (context == nullptr || context->amx == nullptr || context->amx->callback == nullptr)
    {
        if (context != nullptr)
        {
            context->error = AMX_ERR_CALLBACK;
            context->exitKind = static_cast<int>(JitExitKind::ExternalError);
            return context->error;
        }
        return AMX_ERR_CALLBACK;
    }

    context->cip = nextCip;
    SyncForExternalCall(context, nextCip);

    /*
        generated code validates the complete params envelope before
        reaching this helper. keep the helper itself free of
        untrusted-memory reads so a malformed data pointer faults while
        EIP is still in generated code and is therefore handled by the
        JIT CPU-fault boundary.
    */
    cell result = context->pri;
    cell* params = reinterpret_cast<cell*>(context->data + context->stk);
    int callbackError = AMX_ERR_NONE;
    try
    {
        callbackError = context->amx->callback(context->amx, index, &result, params);
    }
    catch (const std::bad_alloc&)
    {
        callbackError = AMX_ERR_MEMORY;
    }
    catch (...)
    {
        callbackError = AMX_ERR_GENERAL;
    }

    context->pri = result;

    if (context->unloadRequested != 0)
        return context->error = kInternalUnload;

    if (callbackError != AMX_ERR_NONE)
    {
        context->error = callbackError;
        context->exitKind = static_cast<int>(JitExitKind::ExternalError);
    }

    return context->error;
}

int JitSignedDivide(JitContext* context, int reverse)
{
    if (context == nullptr)
        return AMX_ERR_PARAMS;

    const int32_t dividend = reverse ? static_cast<int32_t>(context->alt) : static_cast<int32_t>(context->pri);
    const int32_t divisor = reverse ? static_cast<int32_t>(context->pri) : static_cast<int32_t>(context->alt);

    if (divisor == 0)
    {
        context->error = AMX_ERR_DIVIDE;
        context->exitKind = static_cast<int>(JitExitKind::Abort);
        return context->error;
    }

    int64_t quotient = static_cast<int64_t>(dividend) / static_cast<int64_t>(divisor);
    int64_t remainder = 0;

    if (!reverse)
    {
        /*
            preserve open.mp's portable Pawn C VM OP_SDIV quirk exactly.
            its TRUNC_SDIV path overwrites PRI with the quotient first and
            then evaluates `alt = pri % offs`. OP_SDIV_ALT keeps the true
            dividend.
        */
        remainder = quotient % static_cast<int64_t>(divisor);
    }
    else
    {
        remainder = static_cast<int64_t>(dividend) % static_cast<int64_t>(divisor);
    }

    if (remainder != 0 && ((remainder < 0) != (divisor < 0)))
    {
        --quotient;
        remainder += divisor;
    }

    context->pri = static_cast<cell>(static_cast<uint32_t>(quotient));
    context->alt = static_cast<cell>(static_cast<uint32_t>(remainder));
    return AMX_ERR_NONE;
}

int JitUnsignedDivide(JitContext* context, int reverse)
{
    if (context == nullptr)
        return AMX_ERR_PARAMS;

    const uint32_t dividend = reverse ? static_cast<uint32_t>(context->alt) : static_cast<uint32_t>(context->pri);
    const uint32_t divisor = reverse ? static_cast<uint32_t>(context->pri) : static_cast<uint32_t>(context->alt);

    if (divisor == 0)
    {
        context->error = AMX_ERR_DIVIDE;
        context->exitKind = static_cast<int>(JitExitKind::Abort);
        return context->error;
    }

    context->pri = static_cast<cell>(dividend / divisor);
    context->alt = static_cast<cell>(dividend % divisor);
    return AMX_ERR_NONE;
}

int JitMoveMemory(JitContext* context, cell bytes)
{
    if (context == nullptr || bytes < 0)
    {
        if (context != nullptr)
        {
            context->error = AMX_ERR_MEMACCESS;
            context->exitKind = static_cast<int>(JitExitKind::Abort);
            return context->error;
        }
        return AMX_ERR_MEMACCESS;
    }

    if (!CheckPortableEndpoints(context, context->pri, bytes) || !CheckPortableEndpoints(context, context->alt, bytes))
    {
        context->error = AMX_ERR_MEMACCESS;
        context->exitKind = static_cast<int>(JitExitKind::Abort);
        return context->error;
    }

    /*
        current open.mp C runtime uses memcpy(). the Linux signal boundary
        normally claims only generated-code instruction pointers so faults
        in external natives still behave exactly like the reference VM.
        mark this tiny internal helper section explicitly: a
        stale/mprotected AMX data mapping must become a contained JIT
        fault rather than a process crash.
    */
    BeginInternalMemoryFaultScope();
    std::memcpy(context->data + static_cast<ucell>(context->alt), context->data + static_cast<ucell>(context->pri), static_cast<std::size_t>(bytes));
    EndInternalMemoryFaultScope();
    return AMX_ERR_NONE;
}

int JitCompareMemory(JitContext* context, cell bytes)
{
    if (context == nullptr || bytes < 0)
    {
        if (context != nullptr)
        {
            context->error = AMX_ERR_MEMACCESS;
            context->exitKind = static_cast<int>(JitExitKind::Abort);
            return context->error;
        }
        return AMX_ERR_MEMACCESS;
    }

    if (!CheckPortableEndpoints(context, context->pri, bytes) || !CheckPortableEndpoints(context, context->alt, bytes))
    {
        context->error = AMX_ERR_MEMACCESS;
        context->exitKind = static_cast<int>(JitExitKind::Abort);
        return context->error;
    }

    BeginInternalMemoryFaultScope();
    const int comparison = std::memcmp(context->data + static_cast<ucell>(context->alt), context->data + static_cast<ucell>(context->pri), static_cast<std::size_t>(bytes));
    EndInternalMemoryFaultScope();
    context->pri = static_cast<cell>(comparison);
    return AMX_ERR_NONE;
}

int JitFillMemory(JitContext* context, cell bytes)
{
    if (context == nullptr || bytes < 0)
    {
        if (context != nullptr)
        {
            context->error = AMX_ERR_MEMACCESS;
            context->exitKind = static_cast<int>(JitExitKind::Abort);
            return context->error;
        }
        return AMX_ERR_MEMACCESS;
    }

    if (!CheckPortableEndpoints(context, context->alt, bytes))
    {
        context->error = AMX_ERR_MEMACCESS;
        context->exitKind = static_cast<int>(JitExitKind::Abort);
        return context->error;
    }

    BeginInternalMemoryFaultScope();
    for (cell offset = 0; offset + static_cast<cell>(sizeof(cell)) <= bytes; offset += static_cast<cell>(sizeof(cell)))
    {
        std::memcpy(context->data + static_cast<ucell>(context->alt + offset), &context->pri, sizeof(cell));
    }
    EndInternalMemoryFaultScope();

    return AMX_ERR_NONE;
}

int JitDebugBreak(JitContext* context, cell cip)
{
    if (context == nullptr || context->amx == nullptr || context->amx->debug == nullptr)
        return AMX_ERR_NONE;

    context->cip = cip;
    SyncForExternalCall(context, cip);

    int error = AMX_ERR_NONE;
    try
    {
        error = context->amx->debug(context->amx);
    }
    catch (const std::bad_alloc&)
    {
        error = AMX_ERR_MEMORY;
    }
    catch (...)
    {
        error = AMX_ERR_GENERAL;
    }

    if (context->unloadRequested != 0)
        return context->error = kInternalUnload;

    if (error != AMX_ERR_NONE)
    {
        context->error = error;
        context->exitKind = static_cast<int>(JitExitKind::ExternalError);
    }

    return context->error;
}

void PawnJITRuntime::OnLoad(AMX* amx)
{
    if (amx == nullptr)
        return;

    try
    {
        auto [it, inserted] = states_.try_emplace(amx);
        if (!inserted)
            return;

        State& state = it->second;
        state.loadedBase = amx->base;
        state.loadedData = amx->data;
        if (amx->base != nullptr)
        {
            /*
                onAmxLoad runs after open.mp has initialized/relocated the
                AMX. snapshot only the fixed-size immutable header here.
                later JIT entries compare this snapshot before trusting
                any variable structural offset such as COD/DAT/name-table
                locations.
            */
            std::memcpy(&state.loadedHeader, amx->base, sizeof(state.loadedHeader));
            state.headerCaptured = true;
        }
    }
    catch (...)
    {
        /*
            state caching is an optimization. OOM or container failure
            must not escape through the open.mp PawnEventHandler ABI;
            this AMX remains on the reference interpreter for its current
            load lifetime.
        */
    }
}

void PawnJITRuntime::OnUnload(AMX* amx)
{
    auto it = states_.find(amx);

    if (it == states_.end())
        return;

    /*
        a script can synchronously unload itself from inside a native call
        (for example SendRconCommand("unloadfs ...")). AmxUnload then runs
        while generated code for this exact AMX is still on the native
        call stack. releasing it here would make the native return into
        freed executable memory. keep the state alive until the outermost
        Execute unwinds.
    */
    if (it->second.activeExecutions != 0)
    {
        it->second.unloadPending = true;
        for (JitContext* context = it->second.activeContext; context != nullptr; context = context->parentActive)
        {
            context->unloadRequested = 1;
        }
        return;
    }

    if (it->second.program != nullptr)
        ReleaseProgram(*it->second.program);

    states_.erase(it);
}

void PawnJITRuntime::Shutdown()
{
    /*
        do not free generated code underneath an active execution. this
        can happen during GMX/reset and through re-entrant
        component/script teardown initiated by a native. active states
        are retired exactly like OnUnload(): generated frames observe
        unloadRequested, and the outermost Execute() releases the program
        after it has returned from native code.
    */
    for (auto it = states_.begin(); it != states_.end();)
    {
        State& state = it->second;
        if (state.activeExecutions != 0)
        {
            state.unloadPending = true;
            for (JitContext* context = state.activeContext; context != nullptr; context = context->parentActive)
            {
                context->unloadRequested = 1;
            }
            ++it;
            continue;
        }

        if (state.program != nullptr)
            ReleaseProgram(*state.program);

        it = states_.erase(it);
    }
}

PawnJITRuntime::State* PawnJITRuntime::GetState(AMX* amx) noexcept
{
    const auto it = states_.find(amx);
    return it != states_.end() ? &it->second : nullptr;
}

bool PawnJITRuntime::CanExecuteJit(AMX* amx, const State& state) const
{
    if (amx == nullptr || amx->base == nullptr || amx->callback == nullptr || !state.headerCaptured || amx->base != state.loadedBase || amx->data != state.loadedData)
    {
        return false;
    }

    if (std::memcmp(amx->base, &state.loadedHeader, sizeof(state.loadedHeader)) != 0)
        return false;

    if ((amx->flags & AMX_FLAG_BROWSE) != 0)
        return false;

    if ((amx->flags & AMX_FLAG_JITC) != 0)
        return false;

    if ((amx->flags & AMX_FLAG_RELOC) == 0)
        return false;

    /*
        PawnJIT emits 32-bit absolute addresses and therefore only
        supports the normal x86 open.mp relocation model.
        AMX_FLAG_NO_RELOC is the wide-pointer fallback used when a native
        pointer cannot fit in a Pawn cell.
    */
    if ((amx->flags & AMX_FLAG_NO_RELOC) != 0)
        return false;

    /*
        openmultiplayer/compiler/open.mp sets this during relocation and
        never uses the legacy SYSREQ.D self-patching ABI. refuse foreign
        AMX runtimes instead of silently selecting historical
        compatibility semantics.
    */
    if ((amx->flags & AMX_FLAG_NO_SYSREQD) == 0)
        return false;

    if ((amx->flags & AMX_FLAG_NTVREG) == 0)
        return false;

    AmxImageInfo image;
    if (!ValidateAmxImage(amx, &image))
        return false;

    /*
        amx_Init derives these structural live-register limits directly
        from the immutable image header. if another component mutates
        them, interpreting JIT absolute addresses against the altered
        allocation would be unsafe.
    */
    const int64_t expectedHlw64 = static_cast<int64_t>(image.header->hea) - image.header->dat;
    const int64_t expectedStp64 = static_cast<int64_t>(image.header->stp) - image.header->dat - static_cast<int64_t>(sizeof(cell));
    if (expectedHlw64 < 0 || expectedStp64 < expectedHlw64 || expectedHlw64 > std::numeric_limits<cell>::max() || expectedStp64 > std::numeric_limits<cell>::max())
    {
        return false;
    }

    return amx->hlw == static_cast<cell>(expectedHlw64) && amx->stp == static_cast<cell>(expectedStp64);
}

bool PawnJITRuntime::ProgramMatchesAmx(AMX* amx, const CompiledProgram& program) const
{
    if (amx == nullptr || amx->base == nullptr || program.base != amx->base || program.data != GetData(amx) || program.debugHook != amx->debug)
    {
        return false;
    }

    AmxImageInfo image;
    if (!ValidateAmxImage(amx, &image))
        return false;

    const AMX_HEADER* hdr = image.header;
    return program.code == amx->base + hdr->cod && program.codeSize == image.codeSize && program.hlw == amx->hlw && program.stp == amx->stp;
}

bool PawnJITRuntime::EnsureCompiled(AMX* amx, State& state)
{
    /*
        open.mp keeps the loaded P-code structurally stable for our
        purposes. recompile only when the AMX backing storage or
        structural metadata changes.
    */
    if (state.program != nullptr && !ProgramMatchesAmx(amx, *state.program))
    {
        /*
            a native may change AMX metadata and immediately perform a
            nested amx_Exec. the outer execution is still returning into
            this program's generated code, so releasing it here would be
            a use-after-free. let the nested call use the interpreter;
            the next top-level call will safely invalidate and rebuild
            the program after the outermost JIT activation has unwound.
        */
        if (state.activeExecutions != 0)
            return false;

        ReleaseProgram(*state.program);
        state.program.reset();
        state.compileAttempted = false;
    }

    if (state.compileAttempted)
        return state.program != nullptr;

    state.compileAttempted = true;
    std::unique_ptr<CompiledProgram> program(new (std::nothrow) CompiledProgram());
    if (program == nullptr)
        return false;

    if (!CompileProgram(amx, *program))
        return false;

    state.program = std::move(program);
    return true;
}

bool PawnJITRuntime::GetEntryPoint(AMX* amx, int index, cell& cip) const
{
    if (amx == nullptr || amx->base == nullptr)
        return false;

    auto* hdr = reinterpret_cast<AMX_HEADER*>(amx->base);

    if (index == AMX_EXEC_MAIN)
    {
        if (hdr->cip < 0)
            return false;

        cip = static_cast<cell>(hdr->cip);
        return true;
    }

    if (index == AMX_EXEC_CONT)
    {
        cip = amx->cip;
        return true;
    }

    if (index < 0 || hdr->defsize < static_cast<int>(sizeof(uint32_t)) || hdr->publics < 0 || hdr->natives < hdr->publics || hdr->natives > hdr->cod ||
        ((hdr->natives - hdr->publics) % hdr->defsize) != 0)
    {
        return false;
    }

    const unsigned int publicCount = static_cast<unsigned int>((hdr->natives - hdr->publics) / hdr->defsize);

    if (static_cast<unsigned int>(index) >= publicCount)
        return false;

    auto* entry = reinterpret_cast<AMX_FUNCSTUB*>(amx->base + hdr->publics + static_cast<std::size_t>(index) * static_cast<std::size_t>(hdr->defsize));

    cip = static_cast<cell>(entry->address);
    return true;
}

bool PawnJITRuntime::PrepareContext(AMX* amx, CompiledProgram& program, int index, cell entryCip, JitContext& context) const
{
    context = {};
    context.amx = amx;
    context.data = GetData(amx);
    context.dispatch = program.dispatch.data();
    context.dispatchCount = static_cast<uint32_t>(program.dispatch.size());
    context.stp = amx->stp;
    context.error = AMX_ERR_NONE;

    if (context.data == nullptr)
        return false;

    /*
        treat external AMX state as untrusted, especially AMX_EXEC_CONT.
        the JIT maps STK to the physical x86 ESP, so malformed offsets
        must fall back to the interpreter instead of becoming native
        pointers.
    */
    if (amx->hlw < 0 || amx->stp < 0 || amx->hlw > amx->stp || amx->stk < 0 || amx->stk > amx->stp || amx->hea < amx->hlw || amx->hea > amx->stp || amx->hea > amx->stk ||
        !IsCellAligned(amx->hlw) || !IsCellAligned(amx->stp) || !IsCellAligned(amx->stk) || !IsCellAligned(amx->hea))
    {
        return false;
    }

    if (entryCip < 0 || (entryCip % sizeof(cell)) != 0)
        return false;

    const std::size_t slot = static_cast<std::size_t>(entryCip / sizeof(cell));

    if (slot >= program.dispatch.size() || program.dispatch[slot] == nullptr)
        return false;

    if (index == AMX_EXEC_CONT)
    {
        if (amx->frm < 0 || amx->frm > amx->stp || !IsCellAligned(amx->frm) || amx->reset_stk < 0 || amx->reset_stk > amx->stp || !IsCellAligned(amx->reset_stk) ||
            amx->reset_hea < amx->hlw || amx->reset_hea > amx->stp || !IsCellAligned(amx->reset_hea))
        {
            return false;
        }

        context.pri = amx->pri;
        context.alt = amx->alt;
        context.cip = amx->cip;
        context.frm = amx->frm;
        context.stk = amx->stk;
        context.hea = amx->hea;
        context.resetStk = amx->reset_stk;
        context.resetHea = amx->reset_hea;
        return true;
    }

    const int64_t parameterBytes64 = static_cast<int64_t>(amx->paramcount) * static_cast<int64_t>(sizeof(cell));

    if (parameterBytes64 < 0 || parameterBytes64 > std::numeric_limits<cell>::max())
        return false;

    const cell parameterBytes = static_cast<cell>(parameterBytes64);
    const int64_t resetStk64 = static_cast<int64_t>(amx->stk) + parameterBytes;
    const int64_t entryStk64 = static_cast<int64_t>(amx->stk) - static_cast<int64_t>(2 * sizeof(cell));

    if (resetStk64 < 0 || resetStk64 > amx->stp || entryStk64 < 0 || entryStk64 > amx->stp)
    {
        return false;
    }

    /*
        do every rejectable JIT-only check before touching AMX state or
        stack memory. Hook_amx_Exec falls back to the original
        interpreter when this function returns false; leaking one of the
        synthetic entry pushes into that fallback would make the
        interpreter observe a half-entered call.

        the reference amx_Exec pushes the parameter byte count and
        top-level return address first, then applies the 16-cell
        stack/heap margin check. we only accept the call when that check
        is already known to succeed. the rejected path therefore remains
        byte-for-byte untouched for fallback.
    */
    if (static_cast<int64_t>(amx->hea) + static_cast<int64_t>(16 * sizeof(cell)) > entryStk64)
        return false;

    /*
        match reference amx_Exec: fresh main/public calls start with
        PRI=ALT=FRM=0. only AMX_EXEC_CONT restores the saved VM registers.
    */
    context.pri = 0;
    context.alt = 0;
    context.cip = entryCip;
    context.frm = 0;
    context.stk = static_cast<cell>(entryStk64);
    context.hea = amx->hea;
    context.resetStk = static_cast<cell>(resetStk64);
    context.resetHea = amx->hea;

    /*
        defer the synthetic frame writes to generated code. generated
        execution is inside the SEH/sigsetjmp CPU-fault boundary;
        PrepareContext itself is not. a corrupt external data pointer must
        therefore become a contained JIT fault instead of crashing while
        C++ prepares the invocation.
    */
    context.syntheticFrame = 1;
    context.entryParameterBytes = parameterBytes;

    amx->paramcount = 0;
    return true;
}

bool PawnJITRuntime::PrepareNestedContext(AMX* amx, CompiledProgram& program, int index, cell entryCip, JitContext& parent, JitContext& context) const
{
    if (index < 0 || parent.amx != amx || parent.data != program.data)
        return false;

    context = {};
    context.amx = amx;
    context.data = parent.data;
    context.dispatch = parent.dispatch;
    context.dispatchCount = parent.dispatchCount;
    context.stp = amx->stp;
    context.error = AMX_ERR_NONE;

    /*
        a native may push parameters or otherwise adjust live stack/heap
        state before recursively calling amx_Exec. validate only those
        mutable fields; base/data/debug/layout were already checked by
        the fast-entry guard.
    */
    if (amx->hlw < 0 || amx->stp < 0 || amx->hlw > amx->stp || amx->stk < 0 || amx->stk > amx->stp || amx->hea < amx->hlw || amx->hea > amx->stp || amx->hea > amx->stk ||
        !IsCellAligned(amx->hlw) || !IsCellAligned(amx->stp) || !IsCellAligned(amx->stk) || !IsCellAligned(amx->hea))
    {
        return false;
    }

    if (entryCip < 0 || (entryCip % sizeof(cell)) != 0)
        return false;

    const std::size_t slot = static_cast<std::size_t>(entryCip / sizeof(cell));
    if (slot >= program.dispatch.size() || program.dispatch[slot] == nullptr)
        return false;

    const int64_t parameterBytes64 = static_cast<int64_t>(amx->paramcount) * static_cast<int64_t>(sizeof(cell));
    if (parameterBytes64 < 0 || parameterBytes64 > std::numeric_limits<cell>::max())
        return false;

    const cell parameterBytes = static_cast<cell>(parameterBytes64);
    const int64_t resetStk64 = static_cast<int64_t>(amx->stk) + parameterBytes;
    const int64_t entryStk64 = static_cast<int64_t>(amx->stk) - static_cast<int64_t>(2 * sizeof(cell));

    if (resetStk64 < 0 || resetStk64 > amx->stp || entryStk64 < 0 || entryStk64 > amx->stp || static_cast<int64_t>(amx->hea) + static_cast<int64_t>(16 * sizeof(cell)) > entryStk64)
    {
        return false;
    }

    context.pri = 0;
    context.alt = 0;
    context.cip = entryCip;
    context.frm = 0;
    context.stk = static_cast<cell>(entryStk64);
    context.hea = amx->hea;
    context.resetStk = static_cast<cell>(resetStk64);
    context.resetHea = amx->hea;

    context.syntheticFrame = 1;
    context.entryParameterBytes = parameterBytes;

    amx->paramcount = 0;
    return true;
}

bool PawnJITRuntime::RunPreparedContext(AMX* amx, State& state, JitContext& context, cell* retval, int& error)
{
    if (state.program == nullptr)
        return false;

    context.parentActive = state.activeContext;
    state.activeContext = &context;
    ++state.activeExecutions;

    AMX* const previousAmx = g_activeAmx;
    void* const previousState = g_activeState;
    JitContext* const previousContext = g_activeContext;
    g_activeAmx = amx;
    g_activeState = &state;
    g_activeContext = &context;

    const AmxExecutionSnapshot amxSnapshot = CaptureAmxState(amx);
    const GuardedEntryResult guarded = RunGeneratedCodeGuarded(*state.program, &context);
    error = guarded.error;

    if (guarded.faulted)
    {
        /*
            a hardware exception originating inside generated x86 is a
            JIT integrity failure, not a normal Pawn program error.
            restore the externally visible VM control state, abort this
            invocation with the closest AMX error and permanently
            quarantine this AMX from JIT use. Pawn data writes performed
            before the fault are intentionally not copied/rolled back:
            doing so would make every successful amx_Exec pay an
            O(memory) transactional cost. future calls use the reference
            VM.
        */
        RestoreAmxState(amx, amxSnapshot);
        context.error = error;
        context.exitKind = static_cast<int>(JitExitKind::CpuFault);
        context.abortRequested = 1;
        state.jitQuarantined = true;
        state.lastCpuFault = guarded.faultCode;

        for (JitContext* parent = context.parentActive; parent != nullptr; parent = parent->parentActive)
        {
            parent->error = error;
            parent->abortRequested = 1;
            parent->exitKind = static_cast<int>(JitExitKind::CpuFault);
        }
    }

    const bool unloadedDuringRun = error == kInternalUnload;
    if (unloadedDuringRun)
    {
        error = AMX_ERR_NONE;
        context.error = AMX_ERR_NONE;
    }

    if (!unloadedDuringRun && !guarded.faulted)
        SyncAfterRun(context, retval, error);

    state.activeContext = context.parentActive;
    g_activeAmx = previousAmx;
    g_activeState = previousState;
    g_activeContext = previousContext;

    const bool outermostReturned = --state.activeExecutions == 0;
    const bool releaseAfterReturn = outermostReturned && state.unloadPending;
    if (releaseAfterReturn)
    {
        auto it = states_.find(amx);
        if (it != states_.end())
        {
            if (it->second.program != nullptr)
                ReleaseProgram(*it->second.program);
            states_.erase(it);
        }
    }
    else if (outermostReturned && state.jitQuarantined && state.program != nullptr)
    {
        /*
            no generated frame can return into this program anymore.
            reclaim it immediately while retaining the State as a
            permanent interpreter fallback marker for this AMX lifetime.
        */
        ReleaseProgram(*state.program);
        state.program.reset();
    }

    return true;
}

void PawnJITRuntime::SyncAfterRun(JitContext& context, cell* retval, int error) const
{
    AMX* amx = context.amx;
    const JitExitKind exitKind = static_cast<JitExitKind>(context.exitKind);

    /* exact portable/current open.mp C-interpreter publication semantics */
    if (context.hasRetval != 0)
    {
        /* HALT publishes the complete register set before returning */
        amx->pri = context.pri;
        amx->alt = context.alt;
        amx->frm = context.frm;
        amx->cip = context.cip;
    }
    else if (error == AMX_ERR_SLEEP)
    {
        /*
            native/debug sleep publishes PRI/ALT; CIP/FRM/STK/HEA were
            already checkpointed immediately before the external
            callback.
        */
        amx->pri = context.pri;
        amx->alt = context.alt;
    }

    if (error == AMX_ERR_SLEEP)
    {
        /*
            HALT sleep publishes the live VM stack/heap itself.
            native/debug sleep leaves callback-visible AMX::stk/hea edits
            intact.
        */
        if (exitKind == JitExitKind::Halt)
        {
            amx->stk = context.stk;
            amx->hea = context.hea;
        }
        amx->reset_stk = context.resetStk;
        amx->reset_hea = context.resetHea;
    }
    else if (exitKind != JitExitKind::DirectCheck)
    {
        /*
            ABORT() paths reset STK/HEA. CHKMARGIN/CHKSTACK/CHKHEAP are
            the exception in the C VM and leave the last externally
            published state.
        */
        amx->stk = context.resetStk;
        amx->hea = context.resetHea;
    }

    /* only HALT writes *retval in the portable reference interpreter */
    if (retval != nullptr && context.hasRetval != 0)
        *retval = context.pri;
}

bool PawnJITRuntime::Execute(AMX* amx, cell* retval, int index, int& error)
{
    /*
        fast path for synchronous recursive public execution of the AMX
        that is already running on this thread. this is the shape used
        by nested Pawn-call helpers. avoid the global state map,
        compile/invalidation machinery, public-table lookup and generic
        target dispatch; retain live-stack validation and structural AMX
        checks.
    */
    if (index >= 0 && g_activeAmx == amx && g_activeState != nullptr && g_activeContext != nullptr)
    {
        State& state = *static_cast<State*>(g_activeState);
        CompiledProgram* const program = state.program.get();
        const std::size_t publicIndex = static_cast<std::size_t>(index);

        if (!state.unloadPending && !state.jitQuarantined && program != nullptr && CanExecuteJit(amx, state) && ProgramMatchesAmx(amx, *program) &&
            publicIndex < program->publicCips.size() && program->publicCips[publicIndex] >= 0)
        {
            const cell entryCip = program->publicCips[publicIndex];
            JitContext context;
            if (PrepareNestedContext(amx, *program, index, entryCip, *g_activeContext, context))
            {
                return RunPreparedContext(amx, state, context, retval, error);
            }
        }
    }

    State* const statePtr = GetState(amx);
    if (statePtr == nullptr)
        return false;
    State& state = *statePtr;

    if (!CanExecuteJit(amx, state))
        return false;

    if (state.jitQuarantined)
        return false;

    if (!EnsureCompiled(amx, state))
        return false;

    cell entryCip = 0;
    if (index >= 0)
    {
        const std::size_t publicIndex = static_cast<std::size_t>(index);
        if (state.program == nullptr || publicIndex >= state.program->publicCips.size() || state.program->publicCips[publicIndex] < 0)
        {
            return false;
        }
        entryCip = state.program->publicCips[publicIndex];
    }
    else if (!GetEntryPoint(amx, index, entryCip))
    {
        return false;
    }

    JitContext context;
    if (!PrepareContext(amx, *state.program, index, entryCip, context))
        return false;

    return RunPreparedContext(amx, state, context, retval, error);
}
