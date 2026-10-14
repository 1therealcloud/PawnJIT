/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The original code is copyright (c) 2026, 1therealcloud.
 */

#include <sdk.hpp>
#include <Server/Components/Pawn/pawn.hpp>
#include <subhook.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

#if defined(_WIN32)
#   include <windows.h>
#else
#   include <sys/mman.h>
#   include <unistd.h>
#endif

#include "runtime.hpp"

using amx_Exec_t = int(AMXAPI*)(AMX* amx, cell* retval, int index);

namespace
{
    /* open.mp SDK AMX_FUNC_Exec */
    constexpr std::size_t kAmxExecIndex = 7;
    constexpr std::size_t kExecRelayCodeSize = 8;

    ICore* g_core = nullptr;
    IPawnComponent* g_pawn = nullptr;
    bool g_loaded = false;

    subhook_t g_execHook = nullptr;
    void* g_execTarget = nullptr;
    void* g_execRelay = nullptr;
    std::size_t g_execRelaySize = 0;
    std::size_t g_execRelayCodePageSize = 0;
    volatile uint32_t* g_execRelayTargetSlot = nullptr;
    amx_Exec_t g_originalExec = nullptr;

    int AMXAPI Hook_amx_Exec(AMX* amx, cell* retval, int index);

    void Log(LogLevel level, const char* message) noexcept
    {
        if (g_core == nullptr || message == nullptr)
            return;

        try
        {
            g_core->logLn(level, "%s", message);
        }
        catch (...)
        {
            /*
                logging must never make component lifecycle/error handling
                fail.
            */
        }
    }

    void LogUnloadOnce() noexcept
    {
        if (!g_loaded)
            return;

        Log(Message, "[PawnJIT] unloaded");
        g_loaded = false;
    }

    void RemovePawnEventHandlerNoThrow(IPawnComponent* pawn, PawnEventHandler* handler) noexcept
    {
        if (pawn == nullptr || handler == nullptr)
            return;

        try
        {
            pawn->getEventDispatcher().removeEventHandler(handler);
        }
        catch (...)
        {
            /*
                the component is already tearing down. Continue releasing our
                hook/runtime state even if a foreign dispatcher implementation
                violates the normal noexcept lifecycle expectation.
            */
        }
    }

    bool RetargetExecRelay(void* relay, void* destination)
    {
        if (relay == nullptr || destination == nullptr || g_execRelayTargetSlot == nullptr)
            return false;

        const uint32_t target = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(destination));

#if defined(_WIN32)
        InterlockedExchange(reinterpret_cast<volatile LONG*>(g_execRelayTargetSlot), static_cast<LONG>(target));
#else
        __atomic_store_n(g_execRelayTargetSlot, target, __ATOMIC_RELEASE);
#endif
        return true;
    }

    bool InitializeExecRelay(void* relay, void* destination)
    {
        if (relay == nullptr || destination == nullptr || g_execRelayTargetSlot == nullptr || g_execRelayCodePageSize == 0)
        {
            return false;
        }

        /*
            x86: FF 25 <abs32> = JMP dword ptr [absolute-address]. Keep the
            executable relay immutable (RX) and the destination slot RW.
        */
        auto* code = static_cast<unsigned char*>(relay);
        code[0] = 0xFF;
        code[1] = 0x25;
        const uint32_t slotAddress = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_execRelayTargetSlot));
        std::memcpy(code + 2, &slotAddress, sizeof(slotAddress));
        code[6] = 0x90;
        code[7] = 0x90;

        if (!RetargetExecRelay(relay, destination))
            return false;

#if defined(_WIN32)
        DWORD oldProtect = 0;
        if (VirtualProtect(relay, g_execRelayCodePageSize, PAGE_EXECUTE_READ, &oldProtect) == 0)
        {
            return false;
        }
        return FlushInstructionCache(GetCurrentProcess(), relay, kExecRelayCodeSize) != 0;
#else
        if (mprotect(relay, g_execRelayCodePageSize, PROT_READ | PROT_EXEC) != 0)
            return false;
        __builtin___clear_cache(reinterpret_cast<char*>(relay), reinterpret_cast<char*>(relay) + kExecRelayCodeSize);
        return true;
#endif
    }

    void* AllocateExecRelay()
    {
        std::size_t pageSize = 4096;
#if defined(_WIN32)
        SYSTEM_INFO systemInfo = {};
        GetSystemInfo(&systemInfo);
        if (systemInfo.dwPageSize != 0)
            pageSize = static_cast<std::size_t>(systemInfo.dwPageSize);
#else
        const long queriedPageSize = sysconf(_SC_PAGESIZE);
        if (queriedPageSize > 0)
            pageSize = static_cast<std::size_t>(queriedPageSize);
#endif

        if (pageSize > std::numeric_limits<std::size_t>::max() / 2)
            return nullptr;

        g_execRelayCodePageSize = pageSize;
        g_execRelaySize = pageSize * 2;

#if defined(_WIN32)
        void* relay = VirtualAlloc(nullptr, g_execRelaySize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
        void* relay = mmap(nullptr, g_execRelaySize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (relay == MAP_FAILED)
            relay = nullptr;
#endif

        if (relay == nullptr)
        {
            g_execRelaySize = 0;
            g_execRelayCodePageSize = 0;
            g_execRelayTargetSlot = nullptr;
            return nullptr;
        }

        g_execRelayTargetSlot = reinterpret_cast<volatile uint32_t*>(static_cast<unsigned char*>(relay) + pageSize);
        return relay;
    }

    void FreeExecRelay(void* relay)
    {
        if (relay != nullptr)
        {
#if defined(_WIN32)
            VirtualFree(relay, 0, MEM_RELEASE);
#else
            if (g_execRelaySize != 0)
                munmap(relay, g_execRelaySize);
#endif
        }

        g_execRelayTargetSlot = nullptr;
        g_execRelayCodePageSize = 0;
        g_execRelaySize = 0;
    }

    void ProbeHostOpcodeTable(AMX* amx) noexcept
    {
        if (amx == nullptr || g_originalExec == nullptr)
            return;

        AmxImageInfo image;
        if (!ValidateAmxImage(amx, &image) || image.codeSize < static_cast<cell>(sizeof(cell)))
        {
            return;
        }

        auto* hdr = const_cast<AMX_HEADER*>(image.header);

        const cell firstOpcode = *reinterpret_cast<const cell*>(amx->base + hdr->cod);
        if (firstOpcode >= 0 && firstOpcode <= 157)
        {
            MarkHostOpcodeStreamNumeric();
            return;
        }

        /*
            GNU/ICC builds of the Pawn C VM relocate numeric opcodes to
            handler addresses. Ask the host VM for that table through its
            BROWSE path.
        */
        struct FlagsRestore
        {
            AMX* amx = nullptr;
            int flags = 0;
            ~FlagsRestore() noexcept
            {
                if (amx != nullptr)
                    amx->flags = flags;
            }
        } restore{amx, amx->flags};

        cell tableAddress = 0;
        amx->flags = restore.flags | AMX_FLAG_BROWSE;
        int error = AMX_ERR_GENERAL;
        try
        {
            error = g_originalExec(amx, &tableAddress, 0);
        }
        catch (...)
        {
            return;
        }

        if (error != AMX_ERR_NONE || tableAddress == 0)
            return;

        /* indexes 0..157 */
        constexpr std::size_t kOpenMpFileOpcodeCount = 158;
        const auto* table = reinterpret_cast<const cell*>(static_cast<uintptr_t>(static_cast<ucell>(tableAddress)));
        ConfigureHostOpcodeTable(table, kOpenMpFileOpcodeCount);
    }

    int AMXAPI Hook_amx_Exec(AMX* amx, cell* retval, int index)
    {
        try
        {
            int error = AMX_ERR_NONE;

            /*
                generated code temporarily maps the logical Pawn STK to x86
                ESP. Linux signal delivery therefore requires a per-thread
                alternate signal stack before we can safely enter the JIT
                fault boundary.
            */
            if (PreparePawnJITThreadFaultContainment() && g_pawnJITRuntime.Execute(amx, retval, index, error))
            {
                return error;
            }

            return g_originalExec != nullptr ? g_originalExec(amx, retval, index) : AMX_ERR_INIT;
        }
        catch (const std::bad_alloc&)
        {
            return AMX_ERR_MEMORY;
        }
        catch (...)
        {
            /*
                no C++ exception may cross the C AMX ABI. Do not re-run the
                interpreter here: an unexpected exception may have happened
                after generated Pawn code already produced side effects.
            */
            return AMX_ERR_GENERAL;
        }
    }

    bool InstallExecHook(IPawnComponent& pawn)
    {
        if (g_execHook != nullptr)
            return true;

        static_assert(NUM_AMX_FUNCS > kAmxExecIndex, "open.mp SDK AMX table is too small");
        const auto& functions = pawn.getAmxFunctions();
        void* target = functions[kAmxExecIndex];
        if (target == nullptr)
            return false;

        g_execTarget = target;
        g_execRelay = AllocateExecRelay();
        if (g_execRelay == nullptr || !InitializeExecRelay(g_execRelay, reinterpret_cast<void*>(&Hook_amx_Exec)))
        {
            FreeExecRelay(g_execRelay);
            g_execRelay = nullptr;
            g_execTarget = nullptr;
            return false;
        }

        g_execHook = subhook_new(target, g_execRelay);
        if (g_execHook == nullptr)
        {
            FreeExecRelay(g_execRelay);
            g_execRelay = nullptr;
            g_execTarget = nullptr;
            return false;
        }

        g_originalExec = reinterpret_cast<amx_Exec_t>(subhook_get_trampoline(g_execHook));
        if (g_originalExec == nullptr || subhook_install(g_execHook) < 0)
        {
            subhook_free(g_execHook);
            g_execHook = nullptr;
            g_originalExec = nullptr;
            FreeExecRelay(g_execRelay);
            g_execRelay = nullptr;
            g_execTarget = nullptr;
            return false;
        }

        return true;
    }

    void RemoveExecHook()
    {
        if (g_execHook != nullptr)
        {
            const void* currentDestination = g_execTarget != nullptr ? subhook_read_dst(g_execTarget) : nullptr;
            const bool ownsEntryPatch = currentDestination == g_execRelay;

            if (subhook_is_installed(g_execHook) && ownsEntryPatch)
            {
                /*
                    first make any concurrent/in-flight relay entry harmless,
                    then detach the function-entry patch, and only then free
                    the relay. A thread that has already reached the relay
                    will jump to the original trampoline instead of
                    Hook_amx_Exec.
                */
                if (g_execRelay != nullptr && g_originalExec != nullptr)
                    RetargetExecRelay(g_execRelay, reinterpret_cast<void*>(g_originalExec));
                subhook_remove(g_execHook);
                subhook_free(g_execHook);
                g_execHook = nullptr;
                FreeExecRelay(g_execRelay);
                g_execRelay = nullptr;
            }
            else if (subhook_is_installed(g_execHook))
            {
                /*
                    A later component owns the function entry. Retire our
                    relay by forwarding it to the pre-PawnJIT trampoline.
                    Those bridge allocations intentionally remain alive for
                    process lifetime.
                */
                if (g_execRelay != nullptr && g_originalExec != nullptr && RetargetExecRelay(g_execRelay, reinterpret_cast<void*>(g_originalExec)))
                {
                    g_execHook = nullptr;
                    g_execRelay = nullptr;
                    g_execRelayTargetSlot = nullptr;
                    g_execRelayCodePageSize = 0;
                    g_execRelaySize = 0;
                }
            }
            else
            {
                subhook_free(g_execHook);
                g_execHook = nullptr;
                FreeExecRelay(g_execRelay);
                g_execRelay = nullptr;
            }
        }

        g_execTarget = nullptr;
        g_originalExec = nullptr;
        ClearHostOpcodeTable();
    }

    void RegisterLoadedScript(IPawnScript* script) noexcept
    {
        if (script == nullptr)
            return;

        try
        {
            if (!script->IsLoaded())
                return;

            AMX* amx = script->GetAMX();
            ProbeHostOpcodeTable(amx);
            g_pawnJITRuntime.OnLoad(amx);
        }
        catch (...)
        {
            /*
                A script that cannot be inspected safely simply stays on the
                reference interpreter. Never propagate across
                PawnEventHandler.
            */
        }
    }
}

class PawnJITComponent final : public IComponent, public PawnEventHandler
{
public:
    PROVIDE_UID(0x5B5ADDD7D2C56A11)

    StringView componentName() const override
    {
        return "PawnJIT";
    }

    SemanticVersion componentVersion() const override
    {
        return SemanticVersion(0, 54, 3, 0);
    }

    void onLoad(ICore* core) override
    {
        g_core = core;
    }

    void onInit(IComponentList* components) override
    {
        if (components == nullptr)
            return;

        try
        {
            g_pawn = components->queryComponent<IPawnComponent>();
            if (g_pawn == nullptr)
                return;

            g_pawn->getEventDispatcher().addEventHandler(this, EventPriority_Highest);

            if (!InitializePawnJITFaultContainment())
            {
                RemovePawnEventHandlerNoThrow(g_pawn, this);
                g_pawn = nullptr;
                return;
            }

            if (!InstallExecHook(*g_pawn))
            {
                ShutdownPawnJITFaultContainment();
                RemovePawnEventHandlerNoThrow(g_pawn, this);
                g_pawn = nullptr;
                return;
            }

            g_loaded = true;
            Log(Message, "[PawnJIT] loaded");
        }
        catch (...)
        {
            /*
                treat any SDK/allocator failure during component startup as a
                clean disabled-JIT state, while keeping the server itself
                alive.
            */
            g_pawnJITRuntime.Shutdown();
            RemoveExecHook();
            ShutdownPawnJITFaultContainment();
            RemovePawnEventHandlerNoThrow(g_pawn, this);
            g_loaded = false;
            g_pawn = nullptr;
        }
    }

    void onReady() override
    {
        if (g_pawn == nullptr)
            return;

        try
        {
            /*
                normally scripts load after component initialisation, but this
                also makes hot-loading/reordered component startup
                deterministic.
            */
            RegisterLoadedScript(g_pawn->mainScript());
            for (IPawnScript* script : g_pawn->sideScripts())
                RegisterLoadedScript(script);
        }
        catch (...)
        {
            /*
                discovery is optional for correctness; later onAmxLoad events
                or interpreter fallback remain valid if SDK enumeration fails.
            */
        }
    }

    void onFree(IComponent* component) override
    {
        if (component != g_pawn)
            return;

        RemovePawnEventHandlerNoThrow(g_pawn, this);
        g_pawnJITRuntime.Shutdown();
        RemoveExecHook();
        ShutdownPawnJITFaultContainment();

        /*
            open.mp may tear the Pawn dependency down before calling our own
            free(). Emit the lifecycle notice here while ICore is still valid.
        */
        LogUnloadOnce();
        g_pawn = nullptr;
    }

    void free() override
    {
        RemovePawnEventHandlerNoThrow(g_pawn, this);

        g_pawnJITRuntime.Shutdown();
        RemoveExecHook();
        ShutdownPawnJITFaultContainment();

        LogUnloadOnce();
        g_pawn = nullptr;
        g_core = nullptr;
        delete this;
    }

    void reset() override
    {
        /*
            GMX unload/load events normally clean the main script. Side
            scripts, however, may survive a GMX without receiving a fresh
            OnAmxLoad. Drop every cached program first so no stale main-AMX
            entry survives, then immediately re-arm every still-loaded side
            script.
        */
        g_pawnJITRuntime.Shutdown();
        ClearHostOpcodeTable();

        if (g_pawn == nullptr)
            return;

        try
        {
            for (IPawnScript* script : g_pawn->sideScripts())
                RegisterLoadedScript(script);
        }
        catch (...)
        {
            /*
                A side script that cannot be rediscovered simply continues on
                the reference interpreter until its next explicit load event.
            */
        }
    }

    void onAmxLoad(IPawnScript& script) override
    {
        RegisterLoadedScript(&script);
    }

    void onAmxUnload(IPawnScript& script) override
    {
        try
        {
            g_pawnJITRuntime.OnUnload(script.GetAMX());
        }
        catch (...)
        {
            /*
                no exception may cross the open.mp PawnEventHandler ABI.
            */
        }
    }
};

COMPONENT_ENTRY_POINT()
{
    return new (std::nothrow) PawnJITComponent();
}
