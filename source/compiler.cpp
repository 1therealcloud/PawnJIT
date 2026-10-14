/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The original code is copyright (c) 2026, 1therealcloud.
 */

#include "runtime.hpp"

#include <asmjit/x86.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    asmjit::JitRuntime g_jitRuntime;

    struct JitEntryAllocationGuard
    {
        JitEntry entry = nullptr;

        ~JitEntryAllocationGuard()
        {
            if (entry != nullptr)
                g_jitRuntime.release(entry);
        }

        void Commit() noexcept
        {
            entry = nullptr;
        }
    };

    constexpr int kJumpThreadMaxHops = 32;
    constexpr std::size_t kReturnPicMaxTargets = 4;
    constexpr std::size_t kInlineMaxOps = 24;
    constexpr std::size_t kInlineMaxDepth = 4;
    constexpr std::size_t kInlineMaxBranches = 8;
    constexpr std::size_t kLoopEffectMaxDepth = 8;
    constexpr std::size_t kLoopEffectMaxOps = 2048;
    constexpr std::size_t kLoopRangeMaxInitScan = 12;
    constexpr std::size_t kRedundantStoreTraceMaxOps = 128;
    constexpr std::size_t kConstTraceMaxOps = 64;
    constexpr std::size_t kConstPropMinOps = 3;
    constexpr std::size_t kValueTraceMaxOps = 64;
    constexpr std::size_t kValuePropMinOps = 3;
    constexpr std::size_t kStoreForwardTraceMaxOps = 48;
    constexpr std::size_t kMemoryValueTraceMaxOps = 96;
    constexpr std::size_t kMemoryValueMaxSlots = 8;
    constexpr std::size_t kMemoryMergeMaxDepth = 3;
    constexpr std::size_t kMemoryMergeArmMaxOps = 32;
    constexpr std::size_t kMemoryMergeTraceMaxOps = 64;

    template <typename T>
    uint32_t Pointer32(T pointer)
    {
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointer));
    }

    asmjit::x86::Mem Ctx(std::size_t offset)
    {
        return asmjit::x86::dword_ptr(asmjit::x86::esi, static_cast<int32_t>(offset));
    }

    uint8_t MaskedShiftCount(cell value) noexcept
    {
        return static_cast<uint8_t>(static_cast<ucell>(value) & 31u);
    }

    bool IsPositivePowerOfTwo(cell value) noexcept
    {
        if (value <= 0)
            return false;
        const ucell bits = static_cast<ucell>(value);
        return (bits & (bits - 1u)) == 0;
    }

    uint8_t PowerOfTwoShift(cell value) noexcept
    {
        ucell bits = static_cast<ucell>(value);
        uint8_t shift = 0;
        while (bits > 1u)
        {
            bits >>= 1u;
            ++shift;
        }
        return shift;
    }

    unsigned char* ResolveDataBase(AMX* amx)
    {
        if (amx == nullptr || amx->base == nullptr)
            return nullptr;

        if (amx->data != nullptr)
            return amx->data;

        auto* hdr = reinterpret_cast<AMX_HEADER*>(amx->base);
        return amx->base + hdr->dat;
    }

    cell EncodeReturnAddress(cell cip)
    {
        /*
            open.mp's portable Pawn VM stores return CIPs relative to COD.
        */
        return cip;
    }

    int32_t DataDisp(uint32_t dataBase, cell offset = 0)
    {
        return static_cast<int32_t>(dataBase + static_cast<uint32_t>(offset));
    }

    int32_t NegDataDisp(uint32_t dataBase)
    {
        return static_cast<int32_t>(0u - dataBase);
    }

    asmjit::x86::Mem DataAbs(uint32_t dataBase, cell offset = 0)
    {
        return asmjit::x86::dword_ptr(static_cast<uint64_t>(dataBase + static_cast<uint32_t>(offset)));
    }

    asmjit::x86::Mem AmxAbs(uint32_t amxAddress, std::size_t offset)
    {
        return asmjit::x86::dword_ptr(static_cast<uint64_t>(amxAddress + static_cast<uint32_t>(offset)));
    }

    asmjit::x86::Mem DataAt(const asmjit::x86::Gp& address, uint32_t dataBase)
    {
        return asmjit::x86::dword_ptr(address, DataDisp(dataBase));
    }

    /*
        keep the hottest Pawn VM registers in callee-saved x86 registers. EDI
        = PRI, EBX = ALT, EBP = FRM. The physical x86 ESP is the Pawn stack
        pointer while generated Pawn code is running. JitContext::stk is only
        synchronized before external calls / exits.
    */
    void EmitFlushPriAlt(asmjit::x86::Assembler& a)
    {
        using namespace asmjit::x86;
        a.mov(Ctx(offsetof(JitContext, pri)), edi);
        a.mov(Ctx(offsetof(JitContext, alt)), ebx);
    }

    void EmitReloadPriAlt(asmjit::x86::Assembler& a)
    {
        using namespace asmjit::x86;
        a.mov(edi, Ctx(offsetof(JitContext, pri)));
        a.mov(ebx, Ctx(offsetof(JitContext, alt)));
    }

    void EmitSyncStack(asmjit::x86::Assembler& a, uint32_t dataBase)
    {
        using namespace asmjit::x86;
        a.lea(eax, ptr(esp, NegDataDisp(dataBase)));
        a.mov(Ctx(offsetof(JitContext, stk)), eax);
    }

    void EmitReloadStack(asmjit::x86::Assembler& a, uint32_t dataBase)
    {
        using namespace asmjit::x86;
        a.mov(esp, Ctx(offsetof(JitContext, stk)));
        a.add(esp, static_cast<int32_t>(dataBase));
    }

    void EmitPrepareHostCallStack(asmjit::x86::Assembler& a, int argumentBytes)
    {
        using namespace asmjit::x86;

        /*
            external-call stack positions for the only argument sizes emitted
            by PawnJIT are precomputed once in the generated entry prologue.
            Hot SYSREQ/helper paths therefore need only one load instead of
            repeating alignment arithmetic before every host CALL.
        */
        switch (argumentBytes)
        {
            case 4:
                a.mov(esp, Ctx(offsetof(JitContext, hostCallEsp4)));
                return;
            case 8:
                a.mov(esp, Ctx(offsetof(JitContext, hostCallEsp8)));
                return;
            case 16:
                a.mov(esp, Ctx(offsetof(JitContext, hostCallEsp16)));
                return;
            default:
                break;
        }

        a.mov(esp, Ctx(offsetof(JitContext, hostEsp)));
        a.and_(esp, -16);
        const int padding = (16 - (argumentBytes & 15)) & 15;
        if (padding != 0)
            a.sub(esp, padding);
    }

    void EmitEnterHostStack(asmjit::x86::Assembler& a, uint32_t dataBase, int argumentBytes)
    {
        EmitSyncStack(a, dataBase);
        EmitPrepareHostCallStack(a, argumentBytes);
    }

    void EmitLeaveHostStack(asmjit::x86::Assembler& a, uint32_t dataBase)
    {
        EmitReloadStack(a, dataBase);
    }

    void EmitFlushVmRegisters(asmjit::x86::Assembler& a, uint32_t dataBase)
    {
        using namespace asmjit::x86;
        EmitFlushPriAlt(a);
        a.mov(Ctx(offsetof(JitContext, frm)), ebp);
        EmitSyncStack(a, dataBase);
    }

    void EmitReloadVmRegisters(asmjit::x86::Assembler& a, uint32_t dataBase)
    {
        using namespace asmjit::x86;
        EmitReloadPriAlt(a);
        a.mov(ebp, Ctx(offsetof(JitContext, frm)));
        EmitReloadStack(a, dataBase);
    }

    void EmitCheckAddress(asmjit::x86::Assembler& a, uint32_t dataBase, const asmjit::x86::Gp& address, int size, cell stp, const asmjit::Label& memoryError,
                          bool fullRange = false);

    void EmitCheckRawDataRange(asmjit::x86::Assembler& a, const asmjit::x86::Gp& address, int size, cell stp, const asmjit::Label& memoryError)
    {
        using namespace asmjit::x86;

        /*
            LREF/SREF and INC.I/DEC.I in the reference VM are raw data-block
            dereferences: unlike LOAD.I/STOR.I/LIDX they do not reject the
            HEA..STK gap. Keep that observable behavior, while still
            preventing an out-of-allocation host read/write that the original
            VM would turn into undefined process memory access. STP is
            structural for a compiled program, so keep it as an immediate in
            generated code.
        */
        a.test(address, address);
        a.js(memoryError);

        const uint32_t stpBits = static_cast<uint32_t>(static_cast<ucell>(stp));
        /*
            AMX::stp names the start of the sentinel cell. Raw reference-VM
            dereferences may legally consume that cell, so the actual
            allocation ends one cell later at STP + sizeof(cell). Preserve
            those edge cases while still preventing reads/writes beyond the
            AMX allocation.
        */
        const uint64_t allocationEnd64 = static_cast<uint64_t>(stpBits) + static_cast<uint64_t>(sizeof(cell));
        if (size > 0)
        {
            const uint32_t accessSize = static_cast<uint32_t>(size);
            if (allocationEnd64 < accessSize || allocationEnd64 - accessSize > std::numeric_limits<uint32_t>::max())
            {
                a.jmp(memoryError);
                return;
            }
            const uint32_t maxStart = static_cast<uint32_t>(allocationEnd64 - accessSize);
            a.cmp(address, static_cast<int32_t>(maxStart));
        }
        else
        {
            a.cmp(address, static_cast<int32_t>(stpBits));
        }
        a.ja(memoryError);
    }

    void EmitCheckNativeParams(asmjit::x86::Assembler& a, uint32_t dataBase, cell stp, const asmjit::Label& memoryError)
    {
        using namespace asmjit::x86;

        /*
            native params start exactly at the current Pawn STK. In addition
            to validating params[0] itself, harden the external-call boundary
            by validating the full declared parameter envelope before any host
            native sees it. This costs only a handful of instructions at
            SYSREQ boundaries and keeps ordinary Pawn opcode hot paths
            untouched.

            Layout: [params-byte-count][param1]...[paramN]. Reject negative or
            overflowing byte counts that would otherwise let a native walk
            past STP and turn malformed Pawn state into a process memory
            fault.
        */
        a.lea(edx, ptr(esp, NegDataDisp(dataBase)));
        a.test(edx, edx);
        a.js(memoryError);

        const uint32_t stpBits = static_cast<uint32_t>(static_cast<ucell>(stp));

        /*
            STP is the sentinel-cell address, so params[0] itself may start at
            STP. The payload must end no later than the end of that sentinel
            cell, which simplifies to parameterBytes <= STP - STK.
        */
        a.cmp(edx, static_cast<int32_t>(stpBits));
        a.ja(memoryError);

        a.mov(ecx, dword_ptr(edx, DataDisp(dataBase, 0)));
        a.test(ecx, ecx);
        a.js(memoryError);

        a.mov(eax, static_cast<int32_t>(stpBits));
        a.sub(eax, edx);
        a.cmp(ecx, eax);
        a.ja(memoryError);
    }

    void EmitCallbackNativeCall(asmjit::x86::Assembler& a, uint32_t dataBase, uint32_t amxAddress, cell stp, bool indexFromPri, cell immediateIndex, cell nextCip,
                                bool restoreAltBeforeError, bool priLive, const asmjit::Label& memoryError, const asmjit::Label& exitLabel)
    {
        using namespace asmjit::x86;
        (void)amxAddress;
        (void)restoreAltBeforeError;
        (void)priLive;

        /*
            keep malformed native parameter blocks out of external code before
            switching stacks. The helper deliberately does not re-read the
            untrusted params header after leaving generated code.
        */
        EmitCheckNativeParams(a, dataBase, stp, memoryError);
        EmitFlushVmRegisters(a, dataBase);

        /*
            route every native callback through one C++ containment boundary.
            This prevents a C++ exception from a host callback/plugin from
            unwinding through raw generated x86 frames.
        */
        EmitPrepareHostCallStack(a, 12);
        a.push(nextCip);
        if (indexFromPri)
            a.push(edi);
        else
            a.push(immediateIndex);
        a.push(esi);
        a.mov(eax, Pointer32(&JitCallNative));
        a.call(eax);
        a.add(esp, 12);
        EmitReloadVmRegisters(a, dataBase);

        a.test(eax, eax);
        a.jnz(exitLabel);
    }

    void EmitDebugBreakCall(asmjit::x86::Assembler& a, uint32_t dataBase, uint32_t amxAddress, cell nextCip, const asmjit::Label& exitLabel)
    {
        using namespace asmjit::x86;
        (void)amxAddress;

        EmitFlushVmRegisters(a, dataBase);

        /*
            debug hooks are external host code too. Use the same protected
            helper pattern as natives instead of calling AMX::debug from
            generated x86.
        */
        EmitPrepareHostCallStack(a, 8);
        a.push(nextCip);
        a.push(esi);
        a.mov(eax, Pointer32(&JitDebugBreak));
        a.call(eax);
        a.add(esp, 8);
        EmitReloadVmRegisters(a, dataBase);

        a.test(eax, eax);
        a.jnz(exitLabel);
    }

    bool HasInstruction(const DecodedProgram& program, cell cip)
    {
        return program.byCip.find(cip) != program.byCip.end();
    }

    const Instruction* FindInstruction(const DecodedProgram& program, cell cip)
    {
        auto it = program.byCip.find(cip);

        if (it == program.byCip.end())
            return nullptr;

        return &program.instructions[it->second];
    }

    bool StaticAddressValid(AMX* amx, cell address, cell bytes)
    {
        if (amx == nullptr || address < 0 || bytes < 0 || amx->stp < 0)
            return false;

        const uint64_t begin = static_cast<uint64_t>(static_cast<ucell>(address));
        const uint64_t end = begin + static_cast<uint64_t>(static_cast<ucell>(bytes));
        const uint64_t allocationEnd = static_cast<uint64_t>(static_cast<ucell>(amx->stp)) + sizeof(cell);
        return end >= begin && end <= allocationEnd;
    }

    bool FrameOperandValid(AMX* amx, cell offset)
    {
        if (amx == nullptr || amx->base == nullptr)
            return false;

        const int64_t value = static_cast<int64_t>(offset);
        /*
            the reference VM accepts the full cell-sized displacement and only
            combines it with FRM when the instruction executes. Large locals
            can legitimately extend beyond the nominal initial stack
            reservation.
        */
        return value >= std::numeric_limits<cell>::min() && value <= std::numeric_limits<cell>::max();
    }

    cell ResolveBranchTarget(AMX* amx, const Instruction& instruction)
    {
        if (instruction.opcode == Opcode::JREL)
        {
            const int64_t target = static_cast<int64_t>(instruction.Next()) + static_cast<int64_t>(instruction.operands[0]);

            if (target < 0 || target > std::numeric_limits<cell>::max())
                return -1;

            return static_cast<cell>(target);
        }

        return NormalizeCodeTarget(amx, instruction.operands[0]);
    }

    struct ThreadedDirectTarget
    {
        cell target = -1;
    };

    ThreadedDirectTarget ResolveThreadedDirectTarget(AMX* amx, const DecodedProgram& decoded, cell initialTarget)
    {
        ThreadedDirectTarget result;
        result.target = initialTarget;

        if (initialTarget < 0)
            return result;

        std::array<cell, static_cast<std::size_t>(kJumpThreadMaxHops) + 1u> visited{};
        std::size_t visitedCount = 1;
        visited[0] = initialTarget;

        cell current = initialTarget;
        for (int hop = 0; hop < kJumpThreadMaxHops; ++hop)
        {
            const Instruction* instruction = FindInstruction(decoded, current);
            if (instruction == nullptr || (instruction->opcode != Opcode::JUMP && instruction->opcode != Opcode::JREL))
            {
                break;
            }

            const cell next = ResolveBranchTarget(amx, *instruction);
            if (next < 0 || FindInstruction(decoded, next) == nullptr)
                break;

            if (std::find(visited.begin(), visited.begin() + visitedCount, next) != visited.begin() + visitedCount)
            {
                /*
                    never canonicalize a jump cycle to an arbitrary member.
                    Keeping the original edge preserves exact bytecode/CIP
                    loop semantics.
                */
                result.target = initialTarget;
                return result;
            }

            visited[visitedCount++] = next;
            current = next;
            result.target = current;
        }

        return result;
    }

    constexpr std::size_t ReturnPicTargetLimit() noexcept
    {
        return kReturnPicMaxTargets > 0 ? static_cast<std::size_t>(kReturnPicMaxTargets) : 0u;
    }

    struct CallGraphSummary
    {
        std::vector<cell> functionEntryByInstruction;
        std::unordered_map<cell, std::vector<cell>> directReturnCipsByFunction;
        /*
            start-index map used by the fused CONST.pri + CALL.pri codegen
            path.
        */
        std::vector<cell> devirtualizedCallTargetByInstruction;
        /*
            transfer-index map shared with inlining/return specialization.
            This also contains generalized constant-flow CALL.pri sites
            discovered after the full constant-transfer matcher is available.
        */
        std::vector<cell> devirtualizedCallTargetByTransferInstruction;
    };

    void AddUniqueReturnCip(std::vector<cell>& targets, cell cip)
    {
        if (cip < 0)
            return;
        if (std::find(targets.begin(), targets.end(), cip) == targets.end())
            targets.push_back(cip);
    }

    cell ResolveCallGraphFunctionEntry(const DecodedProgram& decoded, const CallGraphSummary& graph, cell target)
    {
        const auto it = decoded.byCip.find(target);
        if (it == decoded.byCip.end() || it->second >= graph.functionEntryByInstruction.size())
            return -1;
        return graph.functionEntryByInstruction[it->second];
    }

    CallGraphSummary BuildCallGraphSummary(AMX* amx, const DecodedProgram& decoded)
    {
        CallGraphSummary graph;
        graph.functionEntryByInstruction.assign(decoded.instructions.size(), -1);
        graph.devirtualizedCallTargetByInstruction.assign(decoded.instructions.size(), -1);
        graph.devirtualizedCallTargetByTransferInstruction.assign(decoded.instructions.size(), -1);

        cell currentFunction = -1;
        for (std::size_t index = 0; index < decoded.instructions.size(); ++index)
        {
            const Instruction& instruction = decoded.instructions[index];
            if (instruction.opcode == Opcode::PROC)
            {
                currentFunction = instruction.cip;
            }
            graph.functionEntryByInstruction[index] = currentFunction;
        }

        auto addCall = [&](cell target, cell returnCip) {
            if (target < 0)
                return;
            const ThreadedDirectTarget threaded = ResolveThreadedDirectTarget(amx, decoded, target);
            const cell functionEntry = ResolveCallGraphFunctionEntry(decoded, graph, threaded.target);
            if (functionEntry < 0)
                return;
            AddUniqueReturnCip(graph.directReturnCipsByFunction[functionEntry], returnCip);
        };

        for (std::size_t index = 0; index < decoded.instructions.size(); ++index)
        {
            const Instruction& instruction = decoded.instructions[index];
            if (instruction.opcode == Opcode::CALL)
            {
                addCall(ResolveBranchTarget(amx, instruction), instruction.Next());
                continue;
            }

            if (instruction.opcode == Opcode::CALL_PRI && index != 0)
            {
                const Instruction& previous = decoded.instructions[index - 1];
                if (previous.Next() == instruction.cip && previous.opcode == Opcode::CONST_PRI && !previous.operands.empty())
                {
                    const cell target = NormalizeCodeTarget(amx, previous.operands[0]);
                    if (target >= 0 && FindInstruction(decoded, target) != nullptr)
                    {
                        const ThreadedDirectTarget threaded = ResolveThreadedDirectTarget(amx, decoded, target);
                        if (threaded.target >= 0 && FindInstruction(decoded, threaded.target) != nullptr)
                        {
                            graph.devirtualizedCallTargetByInstruction[index - 1] = threaded.target;
                            graph.devirtualizedCallTargetByTransferInstruction[index] = threaded.target;
                            addCall(threaded.target, instruction.Next());
                        }
                    }
                }
            }
        }

        return graph;
    }

    enum class InlineRejectReason : uint8_t
    {
        None,
        Shape,
        Opcode,
        ControlFlow,
        Recursive,
        Depth,
        Size
    };

    struct InlineFunctionInfo
    {
        cell entry = -1;
        std::size_t beginIndex = 0;
        std::size_t endIndex = 0;
        std::size_t ownInstructions = 0;
        std::size_t branchInstructions = 0;
        std::size_t expandedInstructions = 0;
        std::size_t expandedDepth = 0;
        std::vector<cell> callees;
        InlineRejectReason reject = InlineRejectReason::None;
        bool localEligible = false;
        bool inlineable = false;
    };

    struct FunctionInliningSummary
    {
        std::unordered_map<cell, InlineFunctionInfo> functions;
        std::vector<cell> targetByCallInstruction;
    };

    constexpr std::size_t InlineMaxOps() noexcept
    {
        return kInlineMaxOps > 0 ? static_cast<std::size_t>(kInlineMaxOps) : 0u;
    }

    constexpr std::size_t InlineMaxDepth() noexcept
    {
        return kInlineMaxDepth > 0 ? static_cast<std::size_t>(kInlineMaxDepth) : 0u;
    }

    constexpr std::size_t InlineMaxBranches() noexcept
    {
        return kInlineMaxBranches > 0 ? static_cast<std::size_t>(kInlineMaxBranches) : 0u;
    }

    bool IsInlineMetadataOpcode(Opcode opcode)
    {
        switch (opcode)
        {
            case Opcode::LINE:
            case Opcode::SYMBOL:
            case Opcode::SRANGE:
            case Opcode::SYMTAG:
            case Opcode::NOP:
                return true;
            default:
                return false;
        }
    }

    bool IsInlineCloneOpcode(Opcode opcode)
    {
        switch (opcode)
        {
            case Opcode::PROC:
            case Opcode::RETN:
            case Opcode::CALL:
            case Opcode::LOAD_PRI:
            case Opcode::LOAD_ALT:
            case Opcode::LOAD_S_PRI:
            case Opcode::LOAD_S_ALT:
            case Opcode::LREF_PRI:
            case Opcode::LREF_ALT:
            case Opcode::LREF_S_PRI:
            case Opcode::LREF_S_ALT:
            case Opcode::LOAD_I:
            case Opcode::CONST_PRI:
            case Opcode::CONST_ALT:
            case Opcode::ADDR_PRI:
            case Opcode::ADDR_ALT:
            case Opcode::STOR_PRI:
            case Opcode::STOR_ALT:
            case Opcode::STOR_S_PRI:
            case Opcode::STOR_S_ALT:
            case Opcode::SREF_PRI:
            case Opcode::SREF_ALT:
            case Opcode::SREF_S_PRI:
            case Opcode::SREF_S_ALT:
            case Opcode::STOR_I:
            case Opcode::LIDX:
            case Opcode::LIDX_B:
            case Opcode::IDXADDR:
            case Opcode::IDXADDR_B:
            case Opcode::MOVE_PRI:
            case Opcode::MOVE_ALT:
            case Opcode::XCHG:
            case Opcode::PUSH_PRI:
            case Opcode::PUSH_ALT:
            case Opcode::PUSH_R:
            case Opcode::PUSH_C:
            case Opcode::PUSH:
            case Opcode::PUSH_S:
            case Opcode::POP_PRI:
            case Opcode::POP_ALT:
            case Opcode::STACK:
            case Opcode::HEAP:
            case Opcode::JUMP:
            case Opcode::JREL:
            case Opcode::JZER:
            case Opcode::JNZ:
            case Opcode::JEQ:
            case Opcode::JNEQ:
            case Opcode::JLESS:
            case Opcode::JLEQ:
            case Opcode::JGRTR:
            case Opcode::JGEQ:
            case Opcode::JSLESS:
            case Opcode::JSLEQ:
            case Opcode::JSGRTR:
            case Opcode::JSGEQ:
            case Opcode::SHL:
            case Opcode::SHR:
            case Opcode::SSHR:
            case Opcode::SHL_C_PRI:
            case Opcode::SHL_C_ALT:
            case Opcode::SHR_C_PRI:
            case Opcode::SHR_C_ALT:
            case Opcode::SMUL:
            case Opcode::SDIV:
            case Opcode::SDIV_ALT:
            case Opcode::UMUL:
            case Opcode::UDIV:
            case Opcode::UDIV_ALT:
            case Opcode::ADD:
            case Opcode::SUB:
            case Opcode::SUB_ALT:
            case Opcode::AND:
            case Opcode::OR:
            case Opcode::XOR:
            case Opcode::NOT:
            case Opcode::NEG:
            case Opcode::INVERT:
            case Opcode::ADD_C:
            case Opcode::SMUL_C:
            case Opcode::ZERO_PRI:
            case Opcode::ZERO_ALT:
            case Opcode::ZERO:
            case Opcode::ZERO_S:
            case Opcode::SIGN_PRI:
            case Opcode::SIGN_ALT:
            case Opcode::EQ:
            case Opcode::NEQ:
            case Opcode::LESS:
            case Opcode::LEQ:
            case Opcode::GRTR:
            case Opcode::GEQ:
            case Opcode::SLESS:
            case Opcode::SLEQ:
            case Opcode::SGRTR:
            case Opcode::SGEQ:
            case Opcode::EQ_C_PRI:
            case Opcode::EQ_C_ALT:
            case Opcode::INC_PRI:
            case Opcode::INC_ALT:
            case Opcode::INC:
            case Opcode::INC_S:
            case Opcode::INC_I:
            case Opcode::DEC_PRI:
            case Opcode::DEC_ALT:
            case Opcode::DEC:
            case Opcode::DEC_S:
            case Opcode::DEC_I:
            case Opcode::BOUNDS:
            case Opcode::BREAK:
            case Opcode::SWAP_PRI:
            case Opcode::SWAP_ALT:
            case Opcode::PUSH_ADR:
                return true;
            default:
                return IsInlineMetadataOpcode(opcode);
        }
    }

    bool IsConditionalDirectBranch(Opcode opcode);
    bool IsUnconditionalDirectBranch(Opcode opcode);

    FunctionInliningSummary BuildFunctionInliningSummary(AMX* amx, const DecodedProgram& decoded, const CallGraphSummary& callGraph)
    {
        FunctionInliningSummary summary;
        summary.targetByCallInstruction.assign(decoded.instructions.size(), -1);

        if (InlineMaxOps() == 0 || InlineMaxDepth() == 0)
            return summary;

        /*
            discover the contiguous bytecode range owned by every PROC. This
            implementation accepts only forward direct control-flow edges.
            This covers the normal if/else diamonds emitted by pawncc while
            still rejecting loops, switch tables, indirect jumps and branches
            that escape the function clone.
        */
        for (std::size_t index = 0; index < decoded.instructions.size(); ++index)
        {
            const Instruction& instruction = decoded.instructions[index];
            if (instruction.opcode != Opcode::PROC)
                continue;

            InlineFunctionInfo info;
            info.entry = instruction.cip;
            info.beginIndex = index;
            info.endIndex = index;

            /*
                first determine the exact instruction range owned by this
                function according to the whole-AMX call graph.
            */
            for (std::size_t cursor = index; cursor < decoded.instructions.size(); ++cursor)
            {
                if (cursor != index && decoded.instructions[cursor].opcode == Opcode::PROC)
                    break;
                if (cursor < callGraph.functionEntryByInstruction.size() && callGraph.functionEntryByInstruction[cursor] != info.entry)
                {
                    break;
                }
                info.endIndex = cursor;
            }

            const std::size_t rangeSize = info.endIndex - info.beginIndex + 1;
            std::vector<std::vector<std::size_t>> successors(rangeSize);
            bool sawRetn = false;

            for (std::size_t cursor = info.beginIndex; cursor <= info.endIndex && info.reject == InlineRejectReason::None; ++cursor)
            {
                const Instruction& current = decoded.instructions[cursor];

                if (!IsInlineCloneOpcode(current.opcode) || current.opcode == Opcode::RET)
                {
                    info.reject = InlineRejectReason::Opcode;
                    break;
                }

                if (current.opcode == Opcode::PROC && cursor != info.beginIndex)
                {
                    info.reject = InlineRejectReason::Shape;
                    break;
                }

                if (current.opcode == Opcode::RETN)
                {
                    sawRetn = true;
                    continue;
                }

                if (current.opcode == Opcode::CALL)
                {
                    const cell rawTarget = ResolveBranchTarget(amx, current);
                    const ThreadedDirectTarget threaded = ResolveThreadedDirectTarget(amx, decoded, rawTarget);
                    const cell callee = ResolveCallGraphFunctionEntry(decoded, callGraph, threaded.target);

                    /*
                        A CALL into the middle of a function is legal bytecode
                        but not safe for this frame-preserving clone.
                    */
                    if (callee < 0 || callee != threaded.target)
                    {
                        info.reject = InlineRejectReason::Shape;
                        break;
                    }
                    info.callees.push_back(callee);
                }

                if (IsUnconditionalDirectBranch(current.opcode) || IsConditionalDirectBranch(current.opcode))
                {
                    const cell targetCip = ResolveBranchTarget(amx, current);
                    const auto targetIt = decoded.byCip.find(targetCip);
                    if (targetIt == decoded.byCip.end() || targetIt->second < info.beginIndex || targetIt->second > info.endIndex)
                    {
                        info.reject = InlineRejectReason::ControlFlow;
                        break;
                    }

                    const std::size_t targetIndex = targetIt->second;
                    if (targetIndex <= cursor)
                    {
                        /*
                            keep loop/SCC cloning out. A strictly forward
                            target plus normal fallthrough guarantees an
                            acyclic clone without needing a second loop
                            analysis here.
                        */
                        info.reject = InlineRejectReason::ControlFlow;
                        break;
                    }

                    if (targetIndex < callGraph.functionEntryByInstruction.size() && callGraph.functionEntryByInstruction[targetIndex] != info.entry)
                    {
                        info.reject = InlineRejectReason::ControlFlow;
                        break;
                    }

                    ++info.branchInstructions;
                    successors[cursor - info.beginIndex].push_back(targetIndex - info.beginIndex);

                    if (IsConditionalDirectBranch(current.opcode))
                    {
                        if (cursor + 1 > info.endIndex)
                        {
                            info.reject = InlineRejectReason::Shape;
                            break;
                        }
                        successors[cursor - info.beginIndex].push_back(cursor + 1 - info.beginIndex);
                    }
                }
                else if (cursor + 1 <= info.endIndex)
                {
                    successors[cursor - info.beginIndex].push_back(cursor + 1 - info.beginIndex);
                }

                if (current.opcode != Opcode::PROC && !IsInlineMetadataOpcode(current.opcode))
                    ++info.ownInstructions;
            }

            if (info.reject == InlineRejectReason::None && !sawRetn)
                info.reject = InlineRejectReason::Shape;

            if (info.reject == InlineRejectReason::None && info.branchInstructions > InlineMaxBranches())
            {
                info.reject = InlineRejectReason::ControlFlow;
            }

            /*
                every path reachable from PROC must terminate in RETN.  Dead
                bytes are harmless because clone entry is always the PROC and
                all accepted direct edges stay inside this range.
            */
            if (info.reject == InlineRejectReason::None)
            {
                std::vector<uint8_t> reachable(rangeSize, 0);
                std::vector<std::size_t> worklist;
                worklist.push_back(0);
                reachable[0] = 1;
                bool reachedRetn = false;

                while (!worklist.empty() && info.reject == InlineRejectReason::None)
                {
                    const std::size_t local = worklist.back();
                    worklist.pop_back();
                    const std::size_t absolute = info.beginIndex + local;
                    const Instruction& current = decoded.instructions[absolute];

                    if (current.opcode == Opcode::RETN)
                    {
                        reachedRetn = true;
                        continue;
                    }

                    if (successors[local].empty())
                    {
                        info.reject = InlineRejectReason::Shape;
                        break;
                    }

                    for (std::size_t successor : successors[local])
                    {
                        if (successor >= rangeSize)
                        {
                            info.reject = InlineRejectReason::ControlFlow;
                            break;
                        }
                        if (!reachable[successor])
                        {
                            reachable[successor] = 1;
                            worklist.push_back(successor);
                        }
                    }
                }

                if (info.reject == InlineRejectReason::None && !reachedRetn)
                    info.reject = InlineRejectReason::Shape;
            }

            info.localEligible = info.reject == InlineRejectReason::None;
            summary.functions.emplace(info.entry, std::move(info));
        }

        enum class VisitState : uint8_t
        {
            None,
            Visiting,
            Done
        };
        std::unordered_map<cell, VisitState> states;

        auto evaluate = [&](auto&& self, cell entry) -> bool {
            auto it = summary.functions.find(entry);
            if (it == summary.functions.end())
                return false;

            InlineFunctionInfo& info = it->second;
            const VisitState state = states[entry];
            if (state == VisitState::Done)
                return info.inlineable;
            if (state == VisitState::Visiting)
            {
                info.reject = InlineRejectReason::Recursive;
                info.inlineable = false;
                return false;
            }

            if (!info.localEligible)
            {
                states[entry] = VisitState::Done;
                return false;
            }

            states[entry] = VisitState::Visiting;
            std::size_t expanded = info.ownInstructions;
            std::size_t expandedDepth = 1;

            for (cell callee : info.callees)
            {
                if (!self(self, callee))
                {
                    if (info.reject == InlineRejectReason::None)
                    {
                        const auto child = summary.functions.find(callee);
                        if (child != summary.functions.end() && child->second.reject != InlineRejectReason::None)
                        {
                            info.reject = child->second.reject;
                        }
                        else
                        {
                            info.reject = InlineRejectReason::Shape;
                        }
                    }
                    states[entry] = VisitState::Done;
                    info.inlineable = false;
                    return false;
                }

                const InlineFunctionInfo& child = summary.functions.find(callee)->second;
                if (child.expandedInstructions > InlineMaxOps() || expanded > InlineMaxOps() - child.expandedInstructions)
                {
                    info.reject = InlineRejectReason::Size;
                    states[entry] = VisitState::Done;
                    info.inlineable = false;
                    return false;
                }
                expanded += child.expandedInstructions;
                expandedDepth = std::max(expandedDepth, child.expandedDepth + 1);
            }

            if (expanded > InlineMaxOps())
            {
                info.reject = InlineRejectReason::Size;
                states[entry] = VisitState::Done;
                info.inlineable = false;
                return false;
            }

            if (expandedDepth > InlineMaxDepth())
            {
                info.reject = InlineRejectReason::Depth;
                states[entry] = VisitState::Done;
                info.inlineable = false;
                return false;
            }

            info.expandedInstructions = expanded;
            info.expandedDepth = expandedDepth;
            info.inlineable = true;
            states[entry] = VisitState::Done;
            return true;
        };

        for (auto& item : summary.functions)
            evaluate(evaluate, item.first);

        /*
            A cycle can be discovered through one node first.  Make the whole
            strongly recursive chain non-inlineable instead of leaving an
            ancestor with a stale successful result.
        */
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (auto& item : summary.functions)
            {
                InlineFunctionInfo& info = item.second;
                if (!info.inlineable)
                    continue;
                for (cell callee : info.callees)
                {
                    const auto child = summary.functions.find(callee);
                    if (child == summary.functions.end() || !child->second.inlineable)
                    {
                        info.inlineable = false;
                        if (info.reject == InlineRejectReason::None)
                        {
                            if (child != summary.functions.end())
                            {
                                info.reject = child->second.reject;
                            }
                            else
                            {
                                info.reject = InlineRejectReason::Shape;
                            }
                        }
                        changed = true;
                        break;
                    }
                }
            }
        }

        for (std::size_t index = 0; index < decoded.instructions.size(); ++index)
        {
            const Instruction& instruction = decoded.instructions[index];
            cell target = -1;

            if (instruction.opcode == Opcode::CALL)
            {
                const cell rawTarget = ResolveBranchTarget(amx, instruction);
                target = ResolveThreadedDirectTarget(amx, decoded, rawTarget).target;
            }
            else if (instruction.opcode == Opcode::CALL_PRI && index < callGraph.devirtualizedCallTargetByTransferInstruction.size())
            {
                target = callGraph.devirtualizedCallTargetByTransferInstruction[index];
            }
            else
            {
                continue;
            }

            const auto it = summary.functions.find(target);
            if (it == summary.functions.end() || !it->second.inlineable)
                continue;

            summary.targetByCallInstruction[index] = target;
        }

        return summary;
    }

    const std::vector<cell>* GetReturnSpecializationTargets(const CallGraphSummary& graph, std::size_t instructionIndex)
    {
        if (instructionIndex >= graph.functionEntryByInstruction.size())
            return nullptr;
        const cell functionEntry = graph.functionEntryByInstruction[instructionIndex];
        if (functionEntry < 0)
            return nullptr;
        const auto it = graph.directReturnCipsByFunction.find(functionEntry);
        if (it == graph.directReturnCipsByFunction.end() || it->second.empty())
            return nullptr;
        /*
            A function with more callers than the PIC budget can still benefit
            from specializing its hottest known return sites.  The generic
            resolver remains the exact fallback for every omitted/indirect
            caller, so limiting the emitted PIC does not narrow semantics.
        */
        return &it->second;
    }

    void EmitCheckAddress(asmjit::x86::Assembler& a, uint32_t dataBase, const asmjit::x86::Gp& address, int size, cell stp, const asmjit::Label& memoryError, bool fullRange)
    {
        using namespace asmjit::x86;

        asmjit::Label stackRegion = a.new_label();
        asmjit::Label valid = a.new_label();

        a.test(address, address);
        a.js(memoryError);

        if (!fullRange)
        {
            /*
                scalar accesses validate only their starting address in the
                reference C VM: address must be below STP and outside the
                HEA..STK gap. A cell may therefore straddle HEA.
            */
            a.cmp(address, stp);
            a.jae(memoryError);
            a.cmp(address, Ctx(offsetof(JitContext, hea)));
            a.jb(valid);
            a.lea(ecx, ptr(esp, NegDataDisp(dataBase)));
            a.cmp(address, ecx);
            a.jb(memoryError);
            a.bind(valid);
            return;
        }

        /*
            first make sure [address, address + size) stays below STP. The
            reference x86 VM compares STP as a raw 32-bit cell (unsigned), so
            preserve the exact bit-pattern semantics even for malformed AMX
            metadata instead of applying a signed compile-time test.
        */
        const uint32_t stpBits = static_cast<uint32_t>(static_cast<ucell>(stp));
        const uint32_t accessSize = static_cast<uint32_t>(size);
        if (stpBits < accessSize)
        {
            a.jmp(memoryError);
            return;
        }
        a.cmp(address, static_cast<int32_t>(stpBits - accessSize));
        a.ja(memoryError);

        /*
            heap-side accesses must end at or before HEA; stack-side accesses
            must start at or above STK. This rejects ranges crossing the
            heap/stack gap.
        */
        a.cmp(address, Ctx(offsetof(JitContext, hea)));
        a.jae(stackRegion);
        a.mov(ecx, Ctx(offsetof(JitContext, hea)));
        a.cmp(ecx, size);
        a.jb(memoryError);
        a.sub(ecx, size);
        a.cmp(address, ecx);
        a.ja(memoryError);
        a.jmp(valid);

        a.bind(stackRegion);
        a.lea(ecx, ptr(esp, NegDataDisp(dataBase)));
        a.cmp(address, ecx);
        a.jb(memoryError);

        a.bind(valid);
    }

    void EmitCheckPushSpace(asmjit::x86::Assembler& a, uint32_t dataBase, int bytes, const asmjit::Label& stackError)
    {
        using namespace asmjit::x86;
        a.lea(edx, ptr(esp, -bytes));
        a.cmp(edx, static_cast<int32_t>(dataBase));
        a.jb(stackError);
    }

    void EmitCheckHeapStackMargin(asmjit::x86::Assembler& a, const asmjit::x86::Gp& heap, const asmjit::x86::Gp& stack, const asmjit::Label& stackError)
    {
        using namespace asmjit::x86;

        /*
            pawn checks hea + STKMARGIN > stk with signed cell arithmetic.
            Rewrite it as hea > stk - STKMARGIN so the addition cannot wrap.
            Every caller has already established a non-negative logical STK,
            therefore subtracting the 64-byte margin cannot overflow int32.
        */
        a.lea(ecx, ptr(stack, -static_cast<int>(16 * sizeof(cell))));
        a.cmp(heap, ecx);
        a.jg(stackError);
    }

    void EmitPushEax(asmjit::x86::Assembler& a, uint32_t dataBase, const asmjit::Label& stackError)
    {
        using namespace asmjit::x86;
        EmitCheckPushSpace(a, dataBase, static_cast<int>(sizeof(cell)), stackError);
        a.push(eax);
    }

    /*
        raw stack-cell access used by POP/SWAP/RETN. The reference VMs access
        [STK] directly here instead of applying LOAD.I's heap/stack-gap rules.
        Keep the host-safe outer data bounds, including the historical STP
        sentinel cell, but deliberately do not reject the HEA..STK gap.
    */
    void EmitCheckRawStackCell(asmjit::x86::Assembler& a, uint32_t dataBase, cell stp, const asmjit::Label& errorLabel)
    {
        using namespace asmjit::x86;

        a.cmp(esp, static_cast<int32_t>(dataBase));
        a.jb(errorLabel);
        a.cmp(esp, static_cast<uint32_t>(dataBase + static_cast<uint32_t>(stp)));
        a.ja(errorLabel);
    }

    /*
        RET/RETN always consume FRM and return-CIP as one adjacent pair.
        Validate both cells with a single host-safety check, then load them
        directly. This preserves the raw-stack semantics while removing the
        duplicated bounds sequence from every function return.
    */
    void EmitPopFrameAndReturn(asmjit::x86::Assembler& a, uint32_t dataBase, cell stp, const asmjit::Label& stackLow)
    {
        using namespace asmjit::x86;

        a.cmp(esp, static_cast<int32_t>(dataBase));
        a.jb(stackLow);

        /*
            match the original 32-bit ADD/SUB sequence exactly.  In
            particular, STP is a raw cell here and the physical upper bound
            wraps modulo 2^32 for malformed metadata rather than being
            interpreted as signed C++.
        */
        const uint32_t upper = dataBase + static_cast<uint32_t>(static_cast<ucell>(stp)) - static_cast<uint32_t>(sizeof(cell));
        a.cmp(esp, upper);
        a.ja(stackLow);

        a.mov(ebp, dword_ptr(esp));
        a.mov(eax, dword_ptr(esp, static_cast<int32_t>(sizeof(cell))));
        a.add(esp, static_cast<int>(2 * sizeof(cell)));
    }

    bool PortableRetnPublishesStack(const DecodedProgram& decoded) noexcept
    {
        /*
            the classic source differs here: ANSI/switch stores amx->stk after
            RETN cleanup, while GNU/ICC indirect-threaded only updates local
            stk.
        */
        return decoded.portableFlavor == PortableVmFlavor::AnsiSwitch;
    }

    bool AlignApplies(const DecodedProgram& decoded, cell operand) noexcept
    {
        /*
            GNU threaded compares signed `offs`; ANSI/switch casts to size_t.
            On the 32-bit Pawn target this differs for negative operands.
        */
        if (decoded.portableFlavor == PortableVmFlavor::GnuThreaded)
            return operand < static_cast<cell>(sizeof(cell));

        return static_cast<ucell>(operand) < static_cast<ucell>(sizeof(cell));
    }

    /*
        ANSI/switch portable amx.c writes the post-cleanup STK back to AMX
        inside RETN. This is observable if a later direct stack/heap check
        returns without ABORT()'s normal reset path. Preserve EAX (saved
        return CIP).
    */
    void EmitPublishPortableRetnStack(asmjit::x86::Assembler& a, uint32_t dataBase, uint32_t amxAddress)
    {
        using namespace asmjit::x86;

        a.lea(edx, ptr(esp, NegDataDisp(dataBase)));
        a.mov(AmxAbs(amxAddress, offsetof(AMX, stk)), edx);
    }

    /*
        RETN consumes params[0] plus the parameter payload.  The reference VM
        accepts signed and unaligned byte counts, so validate the final
        wrapped logical STK rather than imposing normal-compiler frame
        invariants.  This preserves odd/negative #emit behaviour without
        letting physical ESP leave the allocated VM data range. EAX (the
        return CIP) is preserved.
    */
    void EmitRetnParameterCleanup(asmjit::x86::Assembler& a, uint32_t dataBase, cell stp, const asmjit::Label& memoryError, const asmjit::Label& stackLow)
    {
        using namespace asmjit::x86;

        a.lea(edx, ptr(esp, NegDataDisp(dataBase)));
        EmitCheckRawStackCell(a, dataBase, stp, memoryError);

        a.mov(ecx, dword_ptr(esp));
        a.lea(edx, ptr(edx, ecx, 0, static_cast<int>(sizeof(cell))));
        a.test(edx, edx);
        a.js(stackLow);
        a.cmp(edx, stp);
        a.ja(stackLow);

        a.lea(esp, ptr(edx, static_cast<int32_t>(dataBase)));
    }

    /*
        dispatch an exact relative bytecode CIP already in EAX. Unlike the
        broader compatibility resolver used for return slots, this path
        deliberately does not accept physical P-code pointers or generated
        native addresses.
    */
    void EmitDispatchRelativeEax(asmjit::x86::Assembler& a, uint32_t dispatchCount, uint32_t dispatchAddress, const asmjit::Label& invalidAddress)
    {
        using namespace asmjit::x86;

        const uint32_t codeSize = dispatchCount * static_cast<uint32_t>(sizeof(cell));
        /*
            decoder validation guarantees a positive signed-cell code size.
            One unsigned upper-bound check therefore rejects both negative
            targets (their uint32 representation is large) and targets past
            DAT.
        */
        a.cmp(eax, codeSize);
        a.jae(invalidAddress);
        a.test(eax, static_cast<int>(sizeof(cell) - 1));
        a.jnz(invalidAddress);
        a.mov(edx, eax);
        a.shr(edx, 2);
        a.mov(ecx, dispatchAddress);
        a.mov(ecx, dword_ptr(ecx, edx, 2));
        a.test(ecx, ecx);
        a.jz(invalidAddress);
        a.jmp(ecx);
    }

    /*
        dispatch a validated relative bytecode target already in EAX without a
        context->cip store/load round-trip. Portable open.mp targets are
        always relative to COD here; physical/generated native addresses are
        unsupported.
    */
    void EmitDispatchEax(asmjit::x86::Assembler& a, uint32_t dispatchCount, uint32_t dispatchAddress, const asmjit::Label& invalidAddress)
    {
        using namespace asmjit::x86;

        const uint32_t codeSize = dispatchCount * static_cast<uint32_t>(sizeof(cell));
        a.cmp(eax, codeSize);
        a.jae(invalidAddress);
        a.test(eax, static_cast<int>(sizeof(cell) - 1));
        a.jnz(invalidAddress);
        a.mov(edx, eax);
        a.shr(edx, 2);
        a.mov(ecx, dispatchAddress);
        a.mov(ecx, dword_ptr(ecx, edx, 2));
        a.test(ecx, ecx);
        a.jz(invalidAddress);
        a.jmp(ecx);
    }

    /*
        portable-C CALL sites push relative bytecode CIPs. Return dispatch
        accepts only that representation, exactly like the reference VM.
    */
    void EmitReturnDispatchEax(asmjit::x86::Assembler& a, AMX* amx, uint32_t dispatchAddress, const asmjit::Label& invalidAddress)
    {
        using namespace asmjit::x86;

        auto* hdr = reinterpret_cast<AMX_HEADER*>(amx->base);
        const uint32_t codeSize = static_cast<uint32_t>(hdr->dat - hdr->cod);

        /*
            portable CALL/RETN stores a relative bytecode CIP. The reference
            VM rejects every other representation as AMX_ERR_MEMACCESS.
        */
        a.cmp(eax, codeSize);
        a.jae(invalidAddress);
        a.test(eax, static_cast<int>(sizeof(cell) - 1));
        a.jnz(invalidAddress);
        a.mov(edx, eax);
        a.shr(edx, 2);
        a.mov(ecx, dispatchAddress);
        a.mov(ecx, dword_ptr(ecx, edx, 2));
        a.test(ecx, ecx);
        a.jz(invalidAddress);
        a.jmp(ecx);
    }

    void EmitSpecializedReturnDispatchEax(asmjit::x86::Assembler& a, AMX* amx, uint32_t dispatchAddress, const asmjit::Label& invalidAddress, const std::vector<cell>* knownReturns,
                                          const std::vector<asmjit::Label>& labels)
    {
        if (knownReturns != nullptr)
        {
            const std::size_t limit = std::min(knownReturns->size(), ReturnPicTargetLimit());
            for (std::size_t index = 0; index < limit; ++index)
            {
                const cell returnCip = (*knownReturns)[index];
                if (returnCip < 0 || (returnCip % static_cast<cell>(sizeof(cell))) != 0)
                    continue;
                const std::size_t slot = static_cast<std::size_t>(returnCip / sizeof(cell));
                if (slot >= labels.size())
                    continue;
                a.cmp(asmjit::x86::eax, EncodeReturnAddress(returnCip));
                a.je(labels[slot]);
            }
        }
        EmitReturnDispatchEax(a, amx, dispatchAddress, invalidAddress);
    }

    void EmitDispatch(asmjit::x86::Assembler& a, uint32_t dispatchCount, uint32_t dispatchAddress, const asmjit::Label& invalidAddress)
    {
        using namespace asmjit::x86;
        a.mov(eax, Ctx(offsetof(JitContext, cip)));
        EmitDispatchEax(a, dispatchCount, dispatchAddress, invalidAddress);
    }

    void EmitHelperErrorCheck(asmjit::x86::Assembler& a, const asmjit::Label& exitLabel)
    {
        using namespace asmjit::x86;

        a.cmp(Ctx(offsetof(JitContext, abortRequested)), 0);
        a.jne(exitLabel);
        a.cmp(Ctx(offsetof(JitContext, error)), AMX_ERR_NONE);
        a.jne(exitLabel);
    }

    void EmitSignedDivide(asmjit::x86::Assembler& a, bool reverse, bool priLive, bool altLive, const asmjit::Label& divideError)
    {
        using namespace asmjit::x86;

        asmjit::Label normal = a.new_label();
        asmjit::Label remainderNegative = a.new_label();
        asmjit::Label adjust = a.new_label();
        asmjit::Label assign = a.new_label();

        if (reverse)
        {
            a.mov(eax, ebx);
            a.mov(ecx, edi);
        }
        else
        {
            a.mov(eax, edi);
            a.mov(ecx, ebx);
        }

        asmjit::Label divisorOk = a.new_label();
        a.test(ecx, ecx);
        a.jnz(divisorOk);
        a.jmp(divideError);
        a.bind(divisorOk);

        /*
            x86 IDIV traps on INT_MIN / -1. Pawn wraps the quotient to
            INT_MIN.
        */
        a.cmp(eax, std::numeric_limits<int32_t>::min());
        a.jne(normal);
        a.cmp(ecx, -1);
        a.jne(normal);
        a.mov(eax, std::numeric_limits<int32_t>::min());
        a.xor_(edx, edx);
        a.jmp(assign);

        a.bind(normal);
        a.cdq();
        a.idiv(ecx);

        if (!reverse)
        {
            /*
                match portable amx.c OP_SDIV exactly, including its historical
                TRUNC_SDIV quirk: PRI becomes the quotient before ALT is
                formed from PRI % divisor. SDIV.ALT keeps the true dividend
                remainder.
            */
            a.mov(Ctx(offsetof(JitContext, callbackResult)), eax);
            a.cdq();
            a.idiv(ecx);
            a.mov(eax, Ctx(offsetof(JitContext, callbackResult)));
        }

        a.test(edx, edx);
        a.jz(assign);
        a.js(remainderNegative);
        a.test(ecx, ecx);
        a.js(adjust);
        a.jmp(assign);

        a.bind(remainderNegative);
        a.test(ecx, ecx);
        a.js(assign);

        a.bind(adjust);
        a.dec(eax);
        a.add(edx, ecx);

        a.bind(assign);
        if (priLive)
            a.mov(edi, eax);
        if (altLive)
            a.mov(ebx, edx);
    }

    void EmitUnsignedDivide(asmjit::x86::Assembler& a, bool reverse, bool priLive, bool altLive, const asmjit::Label& divideError)
    {
        using namespace asmjit::x86;

        if (reverse)
        {
            a.mov(eax, ebx);
            a.mov(ecx, edi);
        }
        else
        {
            a.mov(eax, edi);
            a.mov(ecx, ebx);
        }

        asmjit::Label divisorOk = a.new_label();
        a.test(ecx, ecx);
        a.jnz(divisorOk);
        a.jmp(divideError);
        a.bind(divisorOk);
        a.xor_(edx, edx);
        a.div(ecx);
        if (priLive)
            a.mov(edi, eax);
        if (altLive)
            a.mov(ebx, edx);
    }

    void EmitComparisonFlags(asmjit::x86::Assembler& a, Opcode opcode, cell operand)
    {
        using namespace asmjit::x86;

        if (opcode == Opcode::EQ_C_PRI)
        {
            if (operand == 0)
                a.test(edi, edi);
            else
                a.cmp(edi, operand);
        }
        else if (opcode == Opcode::EQ_C_ALT)
        {
            if (operand == 0)
                a.test(ebx, ebx);
            else
                a.cmp(ebx, operand);
        }
        else
        {
            a.cmp(edi, ebx);
        }
    }

    void EmitComparisonJump(asmjit::x86::Assembler& a, Opcode opcode, const asmjit::Label& label, bool whenTrue)
    {
        switch (opcode)
        {
            case Opcode::EQ:
            case Opcode::EQ_C_PRI:
            case Opcode::EQ_C_ALT:
                if (whenTrue)
                    a.je(label);
                else
                    a.jne(label);
                break;

            case Opcode::NEQ:
                if (whenTrue)
                    a.jne(label);
                else
                    a.je(label);
                break;

            case Opcode::LESS:
                if (whenTrue)
                    a.jb(label);
                else
                    a.jae(label);
                break;

            case Opcode::LEQ:
                if (whenTrue)
                    a.jbe(label);
                else
                    a.ja(label);
                break;

            case Opcode::GRTR:
                if (whenTrue)
                    a.ja(label);
                else
                    a.jbe(label);
                break;

            case Opcode::GEQ:
                if (whenTrue)
                    a.jae(label);
                else
                    a.jb(label);
                break;

            case Opcode::SLESS:
                if (whenTrue)
                    a.jl(label);
                else
                    a.jge(label);
                break;

            case Opcode::SLEQ:
                if (whenTrue)
                    a.jle(label);
                else
                    a.jg(label);
                break;

            case Opcode::SGRTR:
                if (whenTrue)
                    a.jg(label);
                else
                    a.jle(label);
                break;

            case Opcode::SGEQ:
                if (whenTrue)
                    a.jge(label);
                else
                    a.jl(label);
                break;

            default:
                break;
        }
    }

    enum RegisterMask : uint8_t
    {
        RegNone = 0,
        RegPri = 1u << 0,
        RegAlt = 1u << 1,
    };

    struct RegisterEffect
    {
        uint8_t read = RegNone;
        uint8_t write = RegNone;
        bool barrier = false;
    };

    RegisterEffect GetRegisterEffect(const Instruction& instruction)
    {
        RegisterEffect effect;

        switch (instruction.opcode)
        {
            case Opcode::LOAD_PRI:
            case Opcode::LOAD_S_PRI:
            case Opcode::LREF_PRI:
            case Opcode::LREF_S_PRI:
            case Opcode::CONST_PRI:
            case Opcode::ADDR_PRI:
            case Opcode::POP_PRI:
            case Opcode::ZERO_PRI:
                effect.write = RegPri;
                break;

            case Opcode::LOAD_ALT:
            case Opcode::LOAD_S_ALT:
            case Opcode::LREF_ALT:
            case Opcode::LREF_S_ALT:
            case Opcode::CONST_ALT:
            case Opcode::ADDR_ALT:
            case Opcode::POP_ALT:
            case Opcode::ZERO_ALT:
                effect.write = RegAlt;
                break;

            case Opcode::LOAD_I:
            case Opcode::LODB_I:
            case Opcode::ALIGN_PRI:
            case Opcode::NOT:
            case Opcode::NEG:
            case Opcode::INVERT:
            case Opcode::ADD_C:
            case Opcode::SMUL_C:
            case Opcode::SIGN_PRI:
            case Opcode::EQ_C_PRI:
            case Opcode::INC_PRI:
            case Opcode::DEC_PRI:
                effect.read = RegPri;
                effect.write = RegPri;
                break;

            case Opcode::ALIGN_ALT:
            case Opcode::SIGN_ALT:
            case Opcode::INC_ALT:
            case Opcode::DEC_ALT:
            case Opcode::SHL_C_ALT:
            case Opcode::SHR_C_ALT:
                effect.read = RegAlt;
                effect.write = RegAlt;
                break;

            case Opcode::SHL_C_PRI:
            case Opcode::SHR_C_PRI:
                effect.read = RegPri;
                effect.write = RegPri;
                break;

            case Opcode::MOVE_PRI:
                effect.read = RegAlt;
                effect.write = RegPri;
                break;

            case Opcode::MOVE_ALT:
                effect.read = RegPri;
                effect.write = RegAlt;
                break;

            case Opcode::XCHG:
                effect.read = RegPri | RegAlt;
                effect.write = RegPri | RegAlt;
                break;

            case Opcode::STOR_PRI:
            case Opcode::STOR_S_PRI:
            case Opcode::SREF_PRI:
            case Opcode::SREF_S_PRI:
            case Opcode::PUSH_PRI:
            case Opcode::PUSH_R:
            case Opcode::INC_I:
            case Opcode::DEC_I:
            case Opcode::BOUNDS:
            case Opcode::FILL:
                effect.read = RegPri;
                break;

            case Opcode::STOR_ALT:
            case Opcode::STOR_S_ALT:
            case Opcode::SREF_ALT:
            case Opcode::SREF_S_ALT:
            case Opcode::PUSH_ALT:
                effect.read = RegAlt;
                break;

            case Opcode::STOR_I:
            case Opcode::STRB_I:
            case Opcode::MOVS:
                effect.read = RegPri | RegAlt;
                break;

            case Opcode::LIDX:
            case Opcode::LIDX_B:
            case Opcode::IDXADDR:
            case Opcode::IDXADDR_B:
            case Opcode::SHL:
            case Opcode::SHR:
            case Opcode::SSHR:
            case Opcode::SMUL:
            case Opcode::UMUL:
            case Opcode::ADD:
            case Opcode::SUB:
            case Opcode::SUB_ALT:
            case Opcode::AND:
            case Opcode::OR:
            case Opcode::XOR:
            case Opcode::EQ:
            case Opcode::NEQ:
            case Opcode::LESS:
            case Opcode::LEQ:
            case Opcode::GRTR:
            case Opcode::GEQ:
            case Opcode::SLESS:
            case Opcode::SLEQ:
            case Opcode::SGRTR:
            case Opcode::SGEQ:
            case Opcode::CMPS:
                effect.read = RegPri | RegAlt;
                effect.write = RegPri;
                break;

            case Opcode::EQ_C_ALT:
                effect.read = RegAlt;
                effect.write = RegPri;
                break;

            case Opcode::SDIV:
            case Opcode::SDIV_ALT:
            case Opcode::UDIV:
            case Opcode::UDIV_ALT:
                effect.read = RegPri | RegAlt;
                effect.write = RegPri | RegAlt;
                break;

            case Opcode::STACK:
            case Opcode::HEAP:
                effect.write = RegAlt;
                break;

            case Opcode::LCTRL:
                if (!instruction.operands.empty() && instruction.operands[0] >= 0 && instruction.operands[0] <= 9)
                {
                    effect.write = RegPri;
                    if (instruction.operands[0] == 8)
                    {
                        effect.read = RegPri;
                        effect.barrier = true;
                    }
                }
                break;

            case Opcode::SCTRL:
                if (!instruction.operands.empty())
                {
                    switch (instruction.operands[0])
                    {
                        case 2:
                        case 4:
                        case 5:
                        case 6:
                        case 8:
                            effect.read = RegPri;
                            break;
                        default:
                            break;
                    }
                }
                /*
                    SCTRL can replace HEA/STK/FRM/CIP. Treat every form as a
                    hard boundary for register/dataflow analysis so an unusual
                    #emit cannot let local/memory facts cross a VM-state
                    change.
                */
                effect.barrier = true;
                break;

            case Opcode::JZER:
            case Opcode::JNZ:
            case Opcode::JUMP_PRI:
            case Opcode::SWITCH:
                effect.read = RegPri;
                effect.barrier = true;
                break;

            case Opcode::JEQ:
            case Opcode::JNEQ:
            case Opcode::JLESS:
            case Opcode::JLEQ:
            case Opcode::JGRTR:
            case Opcode::JGEQ:
            case Opcode::JSLESS:
            case Opcode::JSLEQ:
            case Opcode::JSGRTR:
            case Opcode::JSGEQ:
                effect.read = RegPri | RegAlt;
                effect.barrier = true;
                break;

            case Opcode::CALL_PRI:
            case Opcode::SYSREQ_PRI:
                effect.read = RegPri | RegAlt;
                effect.write = RegPri | RegAlt;
                effect.barrier = true;
                break;

            case Opcode::CALL:
            case Opcode::SYSREQ_C:
            case Opcode::SYSREQ_N:
            case Opcode::BREAK:
                /*
                    calls/debug hooks may observe incoming VM registers and
                    may replace them on return. Keep the CFG liveness pass
                    strictly on the safe side around external or
                    interprocedural edges.
                */
                effect.read = RegPri | RegAlt;
                effect.write = RegPri | RegAlt;
                effect.barrier = true;
                break;

            case Opcode::LOAD_BOTH:
            case Opcode::LOAD_S_BOTH:
                /*
                    these macro loads are ordinary direct data reads: they
                    have no helper call, fault edge or control-flow side
                    effect. Keep them visible to liveness/loop CFG so an
                    unused half can be omitted and loops are not split at the
                    macro boundary.
                */
                effect.write = RegPri | RegAlt;
                break;

            case Opcode::PUSH2_C:
            case Opcode::PUSH2:
            case Opcode::PUSH2_S:
            case Opcode::PUSH2_ADR:
            case Opcode::PUSH3_C:
            case Opcode::PUSH3:
            case Opcode::PUSH3_S:
            case Opcode::PUSH3_ADR:
            case Opcode::PUSH4_C:
            case Opcode::PUSH4:
            case Opcode::PUSH4_S:
            case Opcode::PUSH4_ADR:
            case Opcode::PUSH5_C:
            case Opcode::PUSH5:
            case Opcode::PUSH5_S:
            case Opcode::PUSH5_ADR:
                /*
                    multi-push macros can fail on their stack-space check, so
                    they remain hard boundaries. CONST_OP/CONST_S are ordinary
                    direct memory writes and are intentionally allowed through
                    register-liveness/loop CFG below.
                */
                effect.barrier = true;
                break;

            case Opcode::CONST_OP:
            case Opcode::CONST_S:
                break;

            case Opcode::HALT:
                /*
                    HALT exposes PRI as the return value and ALT through the
                    VM register state.
                */
                effect.read = RegPri | RegAlt;
                effect.barrier = true;
                break;

            case Opcode::RET:
            case Opcode::RETN:
                /*
                    interprocedural return targets are not represented as
                    local CFG successors. Both VM registers are architectural
                    outputs of the callee and must therefore remain live at
                    RET/RETN.
                */
                effect.read = RegPri | RegAlt;
                effect.barrier = true;
                break;

            case Opcode::JUMP:
            case Opcode::JREL:
                effect.barrier = true;
                break;

            case Opcode::SWAP_PRI:
                effect.read = RegPri;
                effect.write = RegPri;
                break;

            case Opcode::SWAP_ALT:
                effect.read = RegAlt;
                effect.write = RegAlt;
                break;

            default:
                break;
        }

        /*
            error paths leave the JIT immediately and expose the current VM
            register state back through AMX. Never prove an incoming PRI/ALT
            dead across an opcode that can fail before normal fallthrough;
            otherwise a removed value could become observable only on the
            exceptional path.
        */
        switch (instruction.opcode)
        {
            case Opcode::LREF_PRI:
            case Opcode::LREF_ALT:
            case Opcode::LREF_S_PRI:
            case Opcode::LREF_S_ALT:
            case Opcode::LOAD_I:
            case Opcode::LODB_I:
            case Opcode::SREF_PRI:
            case Opcode::SREF_ALT:
            case Opcode::SREF_S_PRI:
            case Opcode::SREF_S_ALT:
            case Opcode::STOR_I:
            case Opcode::STRB_I:
            case Opcode::LIDX:
            case Opcode::LIDX_B:
            case Opcode::PUSH_PRI:
            case Opcode::PUSH_ALT:
            case Opcode::PUSH_R:
            case Opcode::PUSH_C:
            case Opcode::PUSH:
            case Opcode::PUSH_S:
            case Opcode::POP_PRI:
            case Opcode::POP_ALT:
            case Opcode::STACK:
            case Opcode::HEAP:
            case Opcode::PROC:
            case Opcode::RET:
            case Opcode::RETN:
            case Opcode::CALL:
            case Opcode::CALL_PRI:
            case Opcode::SDIV:
            case Opcode::SDIV_ALT:
            case Opcode::UDIV:
            case Opcode::UDIV_ALT:
            case Opcode::INC_I:
            case Opcode::DEC_I:
            case Opcode::MOVS:
            case Opcode::CMPS:
            case Opcode::FILL:
            case Opcode::BOUNDS:
            case Opcode::SYSREQ_PRI:
            case Opcode::SYSREQ_C:
            case Opcode::JUMP_PRI:
            case Opcode::SWAP_PRI:
            case Opcode::SWAP_ALT:
            case Opcode::PUSH_ADR:
            case Opcode::PUSH2_C:
            case Opcode::PUSH2:
            case Opcode::PUSH2_S:
            case Opcode::PUSH2_ADR:
            case Opcode::PUSH3_C:
            case Opcode::PUSH3:
            case Opcode::PUSH3_S:
            case Opcode::PUSH3_ADR:
            case Opcode::PUSH4_C:
            case Opcode::PUSH4:
            case Opcode::PUSH4_S:
            case Opcode::PUSH4_ADR:
            case Opcode::PUSH5_C:
            case Opcode::PUSH5:
            case Opcode::PUSH5_S:
            case Opcode::PUSH5_ADR:
            case Opcode::SYSREQ_N:
            case Opcode::BREAK:
            case Opcode::SCTRL:
            case Opcode::FILE:
            case Opcode::CASETBL:
                effect.read = static_cast<uint8_t>(effect.read | RegPri | RegAlt);
                effect.barrier = true;
                break;

            default:
                break;
        }

        return effect;
    }

    bool IsPureRegisterInstruction(const Instruction& instruction, uint8_t& written)
    {
        written = RegNone;

        switch (instruction.opcode)
        {
            case Opcode::CONST_PRI:
            case Opcode::ADDR_PRI:
            case Opcode::ZERO_PRI:
                written = RegPri;
                return true;

            case Opcode::CONST_ALT:
            case Opcode::ADDR_ALT:
            case Opcode::ZERO_ALT:
                written = RegAlt;
                return true;

            case Opcode::MOVE_PRI:
            case Opcode::SHL:
            case Opcode::SHR:
            case Opcode::SSHR:
            case Opcode::SHL_C_PRI:
            case Opcode::SHR_C_PRI:
            case Opcode::SMUL:
            case Opcode::UMUL:
            case Opcode::ADD:
            case Opcode::SUB:
            case Opcode::SUB_ALT:
            case Opcode::AND:
            case Opcode::OR:
            case Opcode::XOR:
            case Opcode::NOT:
            case Opcode::NEG:
            case Opcode::INVERT:
            case Opcode::ADD_C:
            case Opcode::SMUL_C:
            case Opcode::SIGN_PRI:
            case Opcode::EQ:
            case Opcode::NEQ:
            case Opcode::LESS:
            case Opcode::LEQ:
            case Opcode::GRTR:
            case Opcode::GEQ:
            case Opcode::SLESS:
            case Opcode::SLEQ:
            case Opcode::SGRTR:
            case Opcode::SGEQ:
            case Opcode::EQ_C_PRI:
            case Opcode::EQ_C_ALT:
            case Opcode::INC_PRI:
            case Opcode::DEC_PRI:
            case Opcode::ALIGN_PRI:
            case Opcode::IDXADDR:
            case Opcode::IDXADDR_B:
                written = RegPri;
                return true;

            case Opcode::MOVE_ALT:
            case Opcode::SHL_C_ALT:
            case Opcode::SHR_C_ALT:
            case Opcode::SIGN_ALT:
            case Opcode::INC_ALT:
            case Opcode::DEC_ALT:
            case Opcode::ALIGN_ALT:
                written = RegAlt;
                return true;

            case Opcode::XCHG:
                written = RegPri | RegAlt;
                return true;

            default:
                return false;
        }
    }

    bool IsConditionalDirectBranch(Opcode opcode)
    {
        switch (opcode)
        {
            case Opcode::JZER:
            case Opcode::JNZ:
            case Opcode::JEQ:
            case Opcode::JNEQ:
            case Opcode::JLESS:
            case Opcode::JLEQ:
            case Opcode::JGRTR:
            case Opcode::JGEQ:
            case Opcode::JSLESS:
            case Opcode::JSLEQ:
            case Opcode::JSGRTR:
            case Opcode::JSGEQ:
                return true;
            default:
                return false;
        }
    }

    bool IsUnconditionalDirectBranch(Opcode opcode)
    {
        return opcode == Opcode::JUMP || opcode == Opcode::JREL;
    }

    void EmitInverseConditionalBranch(asmjit::x86::Assembler& a, Opcode opcode, const asmjit::Label& target)
    {
        switch (opcode)
        {
            case Opcode::JZER:
                a.jnz(target);
                break;
            case Opcode::JNZ:
                a.jz(target);
                break;
            case Opcode::JEQ:
                a.jne(target);
                break;
            case Opcode::JNEQ:
                a.je(target);
                break;
            case Opcode::JLESS:
                a.jae(target);
                break;
            case Opcode::JLEQ:
                a.ja(target);
                break;
            case Opcode::JGRTR:
                a.jbe(target);
                break;
            case Opcode::JGEQ:
                a.jb(target);
                break;
            case Opcode::JSLESS:
                a.jge(target);
                break;
            case Opcode::JSLEQ:
                a.jg(target);
                break;
            case Opcode::JSGRTR:
                a.jle(target);
                break;
            case Opcode::JSGEQ:
                a.jl(target);
                break;
            default:
                break;
        }
    }

    bool MatchBranchOverJumpDiamond(AMX* amx, const DecodedProgram& decoded, std::size_t branchIndex, cell& jumpTarget)
    {
        if (branchIndex + 1 >= decoded.instructions.size())
            return false;

        const Instruction& branch = decoded.instructions[branchIndex];
        const Instruction& jump = decoded.instructions[branchIndex + 1];
        if (!IsConditionalDirectBranch(branch.opcode) || !IsUnconditionalDirectBranch(jump.opcode) || jump.cip != branch.Next())
        {
            return false;
        }

        const cell taken = ResolveBranchTarget(amx, branch);
        if (taken < 0 || taken != jump.Next() || FindInstruction(decoded, taken) == nullptr)
            return false;

        /*
            keep jump-threaded taken edges on the existing path. The diamond
            layout relies on this instruction being the physical fallthrough
            after the intermediate unconditional jump is moved out of line.
        */
        if (ResolveThreadedDirectTarget(amx, decoded, taken).target != taken)
            return false;

        jumpTarget = ResolveBranchTarget(amx, jump);
        return jumpTarget >= 0 && FindInstruction(decoded, jumpTarget) != nullptr;
    }

    bool IsDirectCfgTransfer(Opcode opcode)
    {
        return IsConditionalDirectBranch(opcode) || IsUnconditionalDirectBranch(opcode) || opcode == Opcode::SWITCH;
    }

    bool IsHardLivenessBarrier(const Instruction& instruction)
    {
        const RegisterEffect effect = GetRegisterEffect(instruction);
        return effect.barrier && !IsDirectCfgTransfer(instruction.opcode);
    }

    struct ControlFlowGraph
    {
        std::vector<std::vector<std::size_t>> successors;
        std::vector<uint16_t> predecessorCount;
    };

    void AddCfgEdge(ControlFlowGraph& cfg, std::size_t from, std::size_t to)
    {
        if (from >= cfg.successors.size() || to >= cfg.successors.size())
            return;

        auto& edges = cfg.successors[from];
        for (std::size_t existing : edges)
        {
            if (existing == to)
                return;
        }

        edges.push_back(to);
        if (cfg.predecessorCount[to] != std::numeric_limits<uint16_t>::max())
            ++cfg.predecessorCount[to];
    }

    bool HasLoopNormalFallthrough(const Instruction& instruction)
    {
        switch (instruction.opcode)
        {
            case Opcode::HALT:
            case Opcode::RET:
            case Opcode::RETN:
            case Opcode::JUMP_PRI:
                return false;
            default:
                return true;
        }
    }

    ControlFlowGraph BuildControlFlowGraphImpl(AMX* amx, const DecodedProgram& decoded, bool preserveBarrierFallthrough)
    {
        ControlFlowGraph cfg;
        const std::size_t count = decoded.instructions.size();
        cfg.successors.resize(count);
        cfg.predecessorCount.assign(count, 0);

        if (count == 0)
            return cfg;

        auto markTarget = [&](cell cip) -> std::size_t {
            auto it = decoded.byCip.find(cip);
            if (it == decoded.byCip.end())
                return count;
            return it->second;
        };

        for (std::size_t index = 0; index < count; ++index)
        {
            const Instruction& instruction = decoded.instructions[index];
            const bool hardBarrier = IsHardLivenessBarrier(instruction);

            if (IsUnconditionalDirectBranch(instruction.opcode))
            {
                const std::size_t target = markTarget(ResolveBranchTarget(amx, instruction));
                AddCfgEdge(cfg, index, target);
                continue;
            }

            if (IsConditionalDirectBranch(instruction.opcode))
            {
                const std::size_t target = markTarget(ResolveBranchTarget(amx, instruction));
                AddCfgEdge(cfg, index, target);
                if (index + 1 < count)
                    AddCfgEdge(cfg, index, index + 1);
                continue;
            }

            if (instruction.opcode == Opcode::SWITCH)
            {
                const cell tableCip = NormalizeCodeTarget(amx, instruction.operands[0]);
                const Instruction* table = FindInstruction(decoded, tableCip);

                if (table != nullptr && table->opcode == Opcode::CASETBL && table->operands.size() >= 2)
                {
                    const cell caseCount = table->operands[0];
                    const std::size_t defaultTarget = markTarget(NormalizeCodeTarget(amx, table->operands[1]));
                    AddCfgEdge(cfg, index, defaultTarget);

                    for (cell i = 0; i < caseCount; ++i)
                    {
                        const std::size_t operandIndex = 3 + static_cast<std::size_t>(i) * 2;
                        const std::size_t caseTarget = markTarget(NormalizeCodeTarget(amx, table->operands[operandIndex]));
                        AddCfgEdge(cfg, index, caseTarget);
                    }
                }

                continue;
            }

            if (hardBarrier)
            {
                if (index + 1 < count)
                {
                    /*
                        the liveness/dataflow CFG intentionally cuts
                        exceptional instructions. Loop/SCC analysis needs
                        their normal-success edge (BOUNDS, LOAD.I, CALL,
                        SYSREQ, ...), but must still stop at true terminators
                        such as RET/HALT/JUMP.pri.
                    */
                    if (preserveBarrierFallthrough && HasLoopNormalFallthrough(instruction))
                        AddCfgEdge(cfg, index, index + 1);
                }
                continue;
            }

            if (index + 1 < count)
                AddCfgEdge(cfg, index, index + 1);
        }

        return cfg;
    }

    ControlFlowGraph BuildControlFlowGraph(AMX* amx, const DecodedProgram& decoded)
    {
        return BuildControlFlowGraphImpl(amx, decoded, false);
    }

    ControlFlowGraph BuildLoopControlFlowGraph(AMX* amx, const DecodedProgram& decoded)
    {
        return BuildControlFlowGraphImpl(amx, decoded, true);
    }

    struct LoopAnalysis
    {
        std::size_t components = 0;
        std::vector<int32_t> component;
        std::vector<uint8_t> inLoop;
        std::vector<uint8_t> loopHeader;

        bool IsLoopInstruction(std::size_t index) const
        {
            return index < inLoop.size() && inLoop[index] != 0;
        }

        bool SameLoop(std::size_t lhs, std::size_t rhs) const
        {
            return lhs < component.size() && rhs < component.size() && IsLoopInstruction(lhs) && IsLoopInstruction(rhs) && component[lhs] == component[rhs];
        }
    };

    LoopAnalysis BuildLoopAnalysis(const ControlFlowGraph& cfg)
    {
        LoopAnalysis result;
        const std::size_t count = cfg.successors.size();
        result.component.assign(count, -1);
        result.inLoop.assign(count, 0);
        result.loopHeader.assign(count, 0);
        if (count == 0)
            return result;

        /*
            iterative Kosaraju avoids recursive compiler-stack growth on large
            gamemodes while still giving exact SCCs for the optimizer-visible
            CFG.
        */
        std::vector<std::vector<std::size_t>> reverse(count);
        for (std::size_t from = 0; from < count; ++from)
        {
            for (std::size_t to : cfg.successors[from])
            {
                if (to < count)
                    reverse[to].push_back(from);
            }
        }

        std::vector<uint8_t> visited(count, 0);
        std::vector<std::size_t> order;
        order.reserve(count);

        struct DfsFrame
        {
            std::size_t node;
            std::size_t nextEdge;
        };

        for (std::size_t root = 0; root < count; ++root)
        {
            if (visited[root] != 0)
                continue;

            std::vector<DfsFrame> stack;
            stack.push_back({root, 0});
            visited[root] = 1;

            while (!stack.empty())
            {
                DfsFrame& frame = stack.back();
                const auto& edges = cfg.successors[frame.node];
                if (frame.nextEdge < edges.size())
                {
                    const std::size_t successor = edges[frame.nextEdge++];
                    if (successor < count && visited[successor] == 0)
                    {
                        visited[successor] = 1;
                        stack.push_back({successor, 0});
                    }
                    continue;
                }

                order.push_back(frame.node);
                stack.pop_back();
            }
        }

        for (auto it = order.rbegin(); it != order.rend(); ++it)
        {
            const std::size_t root = *it;
            if (result.component[root] >= 0)
                continue;

            const int32_t componentId = static_cast<int32_t>(result.components++);
            std::vector<std::size_t> members;
            std::vector<std::size_t> stack{root};
            result.component[root] = componentId;

            while (!stack.empty())
            {
                const std::size_t node = stack.back();
                stack.pop_back();
                members.push_back(node);

                for (std::size_t predecessor : reverse[node])
                {
                    if (result.component[predecessor] < 0)
                    {
                        result.component[predecessor] = componentId;
                        stack.push_back(predecessor);
                    }
                }
            }

            bool isLoop = members.size() > 1;
            if (!isLoop && !members.empty())
            {
                const std::size_t only = members[0];
                for (std::size_t successor : cfg.successors[only])
                {
                    if (successor == only)
                    {
                        isLoop = true;
                        break;
                    }
                }
            }

            if (!isLoop)
                continue;

            for (std::size_t member : members)
                result.inLoop[member] = 1;
        }

        /*
            derive loop-header metadata once every SCC id is known.
        */
        for (std::size_t node = 0; node < count; ++node)
        {
            if (!result.IsLoopInstruction(node))
                continue;

            const int32_t componentId = result.component[node];
            if (reverse[node].empty())
            {
                result.loopHeader[node] = 1;
            }
            else
            {
                for (std::size_t predecessor : reverse[node])
                {
                    if (predecessor >= count || result.component[predecessor] != componentId)
                    {
                        if (result.loopHeader[node] == 0)
                        {
                            result.loopHeader[node] = 1;
                        }
                    }
                }
            }
        }

        /*
            A loop that starts at an AMX entry point can have no predecessor
            outside its SCC. Give such SCCs one canonical header as well so
            the optimizer has a canonical header.
        */
        std::vector<std::size_t> firstLoopNode(result.components, count);
        std::vector<uint8_t> componentHasHeader(result.components, 0);
        for (std::size_t node = 0; node < count; ++node)
        {
            if (!result.IsLoopInstruction(node))
                continue;
            const std::size_t componentId = static_cast<std::size_t>(result.component[node]);
            firstLoopNode[componentId] = std::min(firstLoopNode[componentId], node);
            if (result.loopHeader[node] != 0)
                componentHasHeader[componentId] = 1;
        }
        for (std::size_t componentId = 0; componentId < result.components; ++componentId)
        {
            if (componentHasHeader[componentId] == 0 && firstLoopNode[componentId] < count)
            {
                result.loopHeader[firstLoopNode[componentId]] = 1;
            }
        }

        return result;
    }

    struct LocalLiveness
    {
        std::vector<uint8_t> liveIn;
        std::vector<uint8_t> liveOut;
        std::vector<uint8_t> deadPure;

        bool IsLiveIn(std::size_t index, uint8_t reg) const
        {
            return index >= liveIn.size() || (liveIn[index] & reg) != 0;
        }

        bool IsLiveOut(std::size_t index, uint8_t reg) const
        {
            return index >= liveOut.size() || (liveOut[index] & reg) != 0;
        }

        bool IsDeadPure(std::size_t index) const
        {
            return index < deadPure.size() && deadPure[index] != 0;
        }
    };

    LocalLiveness BuildLocalLiveness(const DecodedProgram& decoded, const ControlFlowGraph& cfg)
    {
        LocalLiveness result;
        const std::size_t count = decoded.instructions.size();
        result.liveIn.assign(count, RegNone);
        result.liveOut.assign(count, RegNone);
        result.deadPure.assign(count, 0);

        bool changed = true;

        while (changed)
        {
            changed = false;

            for (std::size_t reverse = count; reverse > 0; --reverse)
            {
                const std::size_t index = reverse - 1;
                const Instruction& instruction = decoded.instructions[index];
                const RegisterEffect effect = GetRegisterEffect(instruction);

                uint8_t liveOut = RegNone;

                if (IsHardLivenessBarrier(instruction))
                {
                    /*
                        separate the normal-success dataflow from the
                        exceptional observation represented by effect.read.
                        Fault/native/call opcodes can expose their incoming
                        PRI/ALT on an error path, but their successful outputs
                        only need to stay live if the bytecode fallthrough
                        actually uses them. This lets codegen omit dead
                        POP/STACK/HEAP/DIV results without weakening the
                        error-state contract. True terminators have no
                        fallthrough.
                    */
                    if (HasLoopNormalFallthrough(instruction) && index + 1 < count)
                        liveOut = result.liveIn[index + 1];
                }
                else
                {
                    for (std::size_t successor : cfg.successors[index])
                        liveOut = static_cast<uint8_t>(liveOut | result.liveIn[successor]);
                }

                uint8_t written = RegNone;
                const bool pure = IsPureRegisterInstruction(instruction, written);
                const bool outputsDead = pure && written != RegNone && (written & liveOut) == 0;

                const uint8_t liveIn = outputsDead ? liveOut : static_cast<uint8_t>((liveOut & ~effect.write) | effect.read);

                if (result.liveOut[index] != liveOut || result.liveIn[index] != liveIn)
                {
                    result.liveOut[index] = liveOut;
                    result.liveIn[index] = liveIn;
                    changed = true;
                }
            }
        }

        for (std::size_t index = 0; index < count; ++index)
        {
            uint8_t written = RegNone;
            const bool pure = IsPureRegisterInstruction(decoded.instructions[index], written);
            if (pure && written != RegNone && (written & result.liveOut[index]) == 0)
                result.deadPure[index] = 1;
        }

        return result;
    }

    void EmitLoopKnownConstant(asmjit::x86::Assembler& a, const asmjit::x86::Gp& reg, cell value)
    {
        if (value == 0)
            a.xor_(reg, reg);
        else
            a.mov(reg, value);
    }

    enum class LoopSlotKind : uint8_t
    {
        Global,
        Local,
    };

    struct LoopSlot
    {
        LoopSlotKind kind = LoopSlotKind::Global;
        cell offset = 0;
    };

    bool SameLoopSlot(const LoopSlot& lhs, const LoopSlot& rhs)
    {
        return lhs.kind == rhs.kind && lhs.offset == rhs.offset;
    }

    bool DecodeLoopLoad(const Instruction& instruction, bool wantPri, LoopSlot& slot)
    {
        if (instruction.operands.empty())
            return false;

        if (wantPri && instruction.opcode == Opcode::LOAD_PRI)
        {
            slot.kind = LoopSlotKind::Global;
            slot.offset = instruction.operands[0];
            return true;
        }
        if (!wantPri && instruction.opcode == Opcode::LOAD_ALT)
        {
            slot.kind = LoopSlotKind::Global;
            slot.offset = instruction.operands[0];
            return true;
        }
        if (wantPri && instruction.opcode == Opcode::LOAD_S_PRI)
        {
            slot.kind = LoopSlotKind::Local;
            slot.offset = instruction.operands[0];
            return true;
        }
        if (!wantPri && instruction.opcode == Opcode::LOAD_S_ALT)
        {
            slot.kind = LoopSlotKind::Local;
            slot.offset = instruction.operands[0];
            return true;
        }
        return false;
    }

    bool DecodeLoopStore(const Instruction& instruction, bool fromPri, LoopSlot& slot)
    {
        if (instruction.operands.empty())
            return false;

        if (fromPri && instruction.opcode == Opcode::STOR_PRI)
        {
            slot.kind = LoopSlotKind::Global;
            slot.offset = instruction.operands[0];
            return true;
        }
        if (!fromPri && instruction.opcode == Opcode::STOR_ALT)
        {
            slot.kind = LoopSlotKind::Global;
            slot.offset = instruction.operands[0];
            return true;
        }
        if (fromPri && instruction.opcode == Opcode::STOR_S_PRI)
        {
            slot.kind = LoopSlotKind::Local;
            slot.offset = instruction.operands[0];
            return true;
        }
        if (!fromPri && instruction.opcode == Opcode::STOR_S_ALT)
        {
            slot.kind = LoopSlotKind::Local;
            slot.offset = instruction.operands[0];
            return true;
        }
        return false;
    }

    bool IsLoopCompareOpcode(Opcode opcode)
    {
        switch (opcode)
        {
            case Opcode::EQ:
            case Opcode::NEQ:
            case Opcode::LESS:
            case Opcode::LEQ:
            case Opcode::GRTR:
            case Opcode::GEQ:
            case Opcode::SLESS:
            case Opcode::SLEQ:
            case Opcode::SGRTR:
            case Opcode::SGEQ:
                return true;
            default:
                return false;
        }
    }

    struct LoopCounterBranchBlock
    {
        LoopSlot slot;
        bool increment = true;
        Opcode branchOpcode = Opcode::NONE;
        cell takenCip = 0;
        cell fallthroughCip = 0;
        bool priLive = false;
        std::size_t foldedInstructions = 0;
    };

    bool DecodeLoopMemoryCounter(const Instruction& instruction, LoopSlot& slot, bool& increment)
    {
        if (instruction.operands.empty())
            return false;
        switch (instruction.opcode)
        {
            case Opcode::INC:
                slot.kind = LoopSlotKind::Global;
                slot.offset = instruction.operands[0];
                increment = true;
                return true;
            case Opcode::DEC:
                slot.kind = LoopSlotKind::Global;
                slot.offset = instruction.operands[0];
                increment = false;
                return true;
            case Opcode::INC_S:
                slot.kind = LoopSlotKind::Local;
                slot.offset = instruction.operands[0];
                increment = true;
                return true;
            case Opcode::DEC_S:
                slot.kind = LoopSlotKind::Local;
                slot.offset = instruction.operands[0];
                increment = false;
                return true;
            default:
                return false;
        }
    }

    enum LoopMemoryClobber : uint8_t
    {
        LoopClobberNone = 0,
        LoopClobberIndirectStore = 1u << 0,
        LoopClobberBulkMemory = 1u << 1,
        LoopClobberNative = 1u << 2,
        LoopClobberIndirectCall = 1u << 3,
        LoopClobberDebug = 1u << 4,
        LoopClobberDirectCall = 1u << 5,
    };

    void AddUniqueLoopSlot(std::vector<LoopSlot>& slots, const LoopSlot& slot)
    {
        for (const LoopSlot& current : slots)
        {
            if (SameLoopSlot(current, slot))
                return;
        }
        slots.push_back(slot);
    }

    uint8_t ClassifyLoopMemoryClobber(AMX* amx, const Instruction& instruction)
    {
        switch (instruction.opcode)
        {
            case Opcode::SREF_PRI:
            case Opcode::SREF_ALT:
            case Opcode::SREF_S_PRI:
            case Opcode::SREF_S_ALT:
            case Opcode::STOR_I:
            case Opcode::STRB_I:
            case Opcode::INC_I:
            case Opcode::DEC_I:
                return LoopClobberIndirectStore;

            case Opcode::MOVS:
            case Opcode::FILL:
                return LoopClobberBulkMemory;

            case Opcode::SYSREQ_PRI:
            case Opcode::SYSREQ_C:
            case Opcode::SYSREQ_N:
                return LoopClobberNative;

            case Opcode::CALL_PRI:
                return LoopClobberIndirectCall;

            case Opcode::BREAK:
                /*
                    BREAK is observational when no debugger is installed.
                    Older loop analysis treated every debug line as an
                    arbitrary memory write, which disabled range analysis on
                    debug-info AMXs.
                */
                return amx != nullptr && amx->debug != nullptr ? LoopClobberDebug : LoopClobberNone;

            default:
                return LoopClobberNone;
        }
    }

    bool DecodeLoopDirectWrite(const Instruction& instruction, LoopSlot& slot);

    struct FunctionMemoryEffect
    {
        bool unknownMemory = false;
        std::vector<LoopSlot> globalWrites;
    };

    struct FunctionMemoryEffectCache
    {
        std::unordered_map<cell, FunctionMemoryEffect> effects;
        std::unordered_map<cell, uint8_t> state;
    };

    FunctionMemoryEffect AnalyzeDirectFunctionMemoryEffect(AMX* amx, const DecodedProgram& decoded, cell entryCip, FunctionMemoryEffectCache& cache, std::size_t depth)
    {
        FunctionMemoryEffect unknown;
        unknown.unknownMemory = true;

        if (entryCip < 0 || depth > static_cast<std::size_t>(kLoopEffectMaxDepth))
        {
            return unknown;
        }

        const auto cached = cache.effects.find(entryCip);
        const auto stateIt = cache.state.find(entryCip);
        if (stateIt != cache.state.end())
        {
            if (stateIt->second == 2 && cached != cache.effects.end())
                return cached->second;
            if (stateIt->second == 1)
            {
                /*
                    recursive direct calls are legal Pawn, but proving a
                    memory fixed point is deliberately outside this
                    lightweight pass.
                */
                return unknown;
            }
        }

        const auto entryIt = decoded.byCip.find(entryCip);
        if (entryIt == decoded.byCip.end())
            return unknown;

        cache.state[entryCip] = 1;
        FunctionMemoryEffect effect;
        const std::size_t count = decoded.instructions.size();
        std::vector<uint8_t> visited(count, 0);
        std::vector<std::size_t> worklist{entryIt->second};
        std::size_t visitedInstructions = 0;

        auto enqueueCip = [&](cell cip) {
            auto it = decoded.byCip.find(cip);
            if (it == decoded.byCip.end())
            {
                effect.unknownMemory = true;
                return;
            }
            if (visited[it->second] == 0)
                worklist.push_back(it->second);
        };

        auto enqueueIndex = [&](std::size_t index) {
            if (index < count && visited[index] == 0)
                worklist.push_back(index);
        };

        for (std::size_t cursor = 0; cursor < worklist.size(); ++cursor)
        {
            const std::size_t index = worklist[cursor];
            if (index >= count || visited[index] != 0)
                continue;
            visited[index] = 1;

            if (++visitedInstructions > static_cast<std::size_t>(kLoopEffectMaxOps))
            {
                effect.unknownMemory = true;
                break;
            }

            const Instruction& instruction = decoded.instructions[index];
            LoopSlot written;
            if (DecodeLoopDirectWrite(instruction, written))
            {
                /*
                    STOR.S/INC.S/etc. address the callee's frame and therefore
                    cannot alias a direct local in the caller. Absolute
                    globals retain their AMX data offset across calls.
                */
                if (written.kind == LoopSlotKind::Global)
                    AddUniqueLoopSlot(effect.globalWrites, written);
            }

            const uint8_t clobber = ClassifyLoopMemoryClobber(amx, instruction);
            if (clobber != LoopClobberNone)
                effect.unknownMemory = true;

            if (instruction.opcode == Opcode::CALL)
            {
                const cell target = ResolveBranchTarget(amx, instruction);
                const FunctionMemoryEffect nested = AnalyzeDirectFunctionMemoryEffect(amx, decoded, target, cache, depth + 1);
                if (nested.unknownMemory)
                    effect.unknownMemory = true;
                for (const LoopSlot& slot : nested.globalWrites)
                    AddUniqueLoopSlot(effect.globalWrites, slot);
                enqueueIndex(index + 1);
                continue;
            }

            if (instruction.opcode == Opcode::RET || instruction.opcode == Opcode::RETN || instruction.opcode == Opcode::HALT || instruction.opcode == Opcode::JUMP_PRI ||
                instruction.opcode == Opcode::SCTRL)
            {
                if (instruction.opcode == Opcode::JUMP_PRI || instruction.opcode == Opcode::SCTRL)
                    effect.unknownMemory = true;
                continue;
            }

            if (IsUnconditionalDirectBranch(instruction.opcode))
            {
                enqueueCip(ResolveBranchTarget(amx, instruction));
                continue;
            }

            if (IsConditionalDirectBranch(instruction.opcode))
            {
                enqueueCip(ResolveBranchTarget(amx, instruction));
                enqueueIndex(index + 1);
                continue;
            }

            if (instruction.opcode == Opcode::SWITCH)
            {
                if (instruction.operands.empty())
                {
                    effect.unknownMemory = true;
                    continue;
                }
                const cell tableCip = NormalizeCodeTarget(amx, instruction.operands[0]);
                const Instruction* table = FindInstruction(decoded, tableCip);
                if (table == nullptr || table->opcode != Opcode::CASETBL || table->operands.size() < 2)
                {
                    effect.unknownMemory = true;
                    continue;
                }

                const cell caseCount = table->operands[0];
                enqueueCip(NormalizeCodeTarget(amx, table->operands[1]));
                for (cell i = 0; i < caseCount; ++i)
                {
                    const std::size_t operandIndex = 3 + static_cast<std::size_t>(i) * 2;
                    if (operandIndex >= table->operands.size())
                    {
                        effect.unknownMemory = true;
                        break;
                    }
                    enqueueCip(NormalizeCodeTarget(amx, table->operands[operandIndex]));
                }
                continue;
            }

            enqueueIndex(index + 1);
        }

        cache.state[entryCip] = 2;
        cache.effects[entryCip] = effect;
        return effect;
    }

    struct LoopMemorySummary
    {
        std::vector<std::vector<LoopSlot>> writes;
        std::vector<std::vector<LoopSlot>> instructionWrites;
        std::vector<uint8_t> instructionClobber;
        std::vector<uint8_t> clobberBarrier;

        bool MayClobberSlot(std::size_t instructionIndex, const LoopSlot& slot) const
        {
            if (instructionIndex >= instructionClobber.size())
                return true;
            if (instructionClobber[instructionIndex] != 0)
                return true;
            if (instructionIndex >= instructionWrites.size())
                return true;
            for (const LoopSlot& written : instructionWrites[instructionIndex])
            {
                if (SameLoopSlot(written, slot))
                    return true;
            }
            return false;
        }

        bool IsInvariant(const LoopAnalysis& loops, std::size_t instructionIndex, const LoopSlot& slot) const
        {
            if (!loops.IsLoopInstruction(instructionIndex) || instructionIndex >= loops.component.size())
                return false;
            const int32_t componentId = loops.component[instructionIndex];
            if (componentId < 0 || static_cast<std::size_t>(componentId) >= writes.size())
                return false;
            if (clobberBarrier[static_cast<std::size_t>(componentId)] != 0)
                return false;

            for (const LoopSlot& written : writes[static_cast<std::size_t>(componentId)])
            {
                /*
                    valid direct global addresses live below HEA while direct
                    frame locals live in the stack region. They cannot alias
                    one another without an indirect address operation, which
                    is already represented as an unknown clobber barrier.
                */
                if (SameLoopSlot(written, slot))
                    return false;
            }
            return true;
        }
    };

    bool DecodeLoopDirectWrite(const Instruction& instruction, LoopSlot& slot)
    {
        if (instruction.operands.empty())
            return false;
        slot.offset = instruction.operands[0];
        switch (instruction.opcode)
        {
            case Opcode::STOR_PRI:
            case Opcode::STOR_ALT:
            case Opcode::ZERO:
            case Opcode::INC:
            case Opcode::DEC:
            case Opcode::CONST_OP:
                slot.kind = LoopSlotKind::Global;
                return true;
            case Opcode::STOR_S_PRI:
            case Opcode::STOR_S_ALT:
            case Opcode::ZERO_S:
            case Opcode::INC_S:
            case Opcode::DEC_S:
            case Opcode::CONST_S:
                slot.kind = LoopSlotKind::Local;
                return true;
            default:
                return false;
        }
    }

    LoopMemorySummary BuildLoopMemorySummary(AMX* amx, const DecodedProgram& decoded, const LoopAnalysis& loops)
    {
        LoopMemorySummary summary;
        summary.writes.resize(loops.components);
        summary.instructionWrites.resize(decoded.instructions.size());
        summary.instructionClobber.assign(decoded.instructions.size(), 0);
        summary.clobberBarrier.assign(loops.components, 0);
        FunctionMemoryEffectCache functionEffects;

        auto addComponentBarrier = [&](std::size_t component) {
            if (component < summary.clobberBarrier.size())
                summary.clobberBarrier[component] = 1;
        };

        for (std::size_t index = 0; index < decoded.instructions.size(); ++index)
        {
            const bool inLoop = loops.IsLoopInstruction(index) && index < loops.component.size() && loops.component[index] >= 0;
            const std::size_t component = inLoop ? static_cast<std::size_t>(loops.component[index]) : loops.components;

            const Instruction& instruction = decoded.instructions[index];
            LoopSlot written;
            if (DecodeLoopDirectWrite(instruction, written))
            {
                AddUniqueLoopSlot(summary.instructionWrites[index], written);
                if (inLoop && component < summary.writes.size())
                {
                    AddUniqueLoopSlot(summary.writes[component], written);
                }
                continue;
            }

            if (instruction.opcode == Opcode::CALL)
            {
                const cell target = ResolveBranchTarget(amx, instruction);
                const FunctionMemoryEffect effect = AnalyzeDirectFunctionMemoryEffect(amx, decoded, target, functionEffects, 0);
                if (effect.unknownMemory)
                {
                    summary.instructionClobber[index] = 1;
                    if (inLoop)
                        addComponentBarrier(component);
                }
                else
                {
                    for (const LoopSlot& slot : effect.globalWrites)
                        AddUniqueLoopSlot(summary.instructionWrites[index], slot);

                    if (inLoop)
                    {
                        for (const LoopSlot& slot : effect.globalWrites)
                            AddUniqueLoopSlot(summary.writes[component], slot);
                    }
                }
                continue;
            }

            const uint8_t clobber = ClassifyLoopMemoryClobber(amx, instruction);
            if (instruction.opcode == Opcode::BREAK && clobber == LoopClobberNone)
                continue;
            if (clobber != LoopClobberNone)
            {
                summary.instructionClobber[index] = 1;
                if (inLoop)
                    addComponentBarrier(component);
            }
        }

        return summary;
    }

    struct LoopRangeFact
    {
        LoopSlot slot;
        cell minInclusive = 0;
        cell maxInclusive = 0;
        std::vector<uint8_t> safeInstruction;
    };

    struct LoopGuardBoundFact
    {
        LoopSlot slot;
        cell maxInclusive = 0;
        bool nonNegative = false;
        std::vector<uint8_t> safeInstruction;
    };

    enum class LoopBoundsProof : uint8_t
    {
        None,
        UpperOnly,
        Full,
    };

    struct LoopRangeAnalysis
    {
        std::vector<LoopRangeFact> facts;
        std::vector<LoopGuardBoundFact> guardFacts;

        LoopBoundsProof ProveBounds(std::size_t instructionIndex, const LoopSlot& slot, cell upperBound) const
        {
            if (upperBound < 0)
                return LoopBoundsProof::None;

            for (const LoopRangeFact& fact : facts)
            {
                if (!SameLoopSlot(fact.slot, slot) || instructionIndex >= fact.safeInstruction.size() || fact.safeInstruction[instructionIndex] == 0)
                {
                    continue;
                }
                if (fact.minInclusive >= 0 && fact.maxInclusive <= upperBound)
                    return LoopBoundsProof::Full;
            }

            LoopBoundsProof best = LoopBoundsProof::None;
            for (const LoopGuardBoundFact& fact : guardFacts)
            {
                if (!SameLoopSlot(fact.slot, slot) || instructionIndex >= fact.safeInstruction.size() || fact.safeInstruction[instructionIndex] == 0 ||
                    fact.maxInclusive > upperBound)
                {
                    continue;
                }
                if (fact.nonNegative)
                    return LoopBoundsProof::Full;
                best = LoopBoundsProof::UpperOnly;
            }
            return best;
        }

        bool Proves(std::size_t instructionIndex, const LoopSlot& slot, cell upperBound) const
        {
            return ProveBounds(instructionIndex, slot, upperBound) == LoopBoundsProof::Full;
        }
    };

    bool IsLoopIndirectControlBarrier(const Instruction& instruction)
    {
        switch (instruction.opcode)
        {
            case Opcode::JUMP_PRI:
            case Opcode::CALL_PRI:
            case Opcode::SCTRL:
                return true;
            default:
                return false;
        }
    }

    bool IsZeroLoopStore(const Instruction& instruction, const LoopSlot& slot)
    {
        if (instruction.operands.empty())
            return false;
        if (slot.kind == LoopSlotKind::Global)
            return instruction.opcode == Opcode::ZERO && instruction.operands[0] == slot.offset;
        return instruction.opcode == Opcode::ZERO_S && instruction.operands[0] == slot.offset;
    }

    bool DecodeLoopConstRegister(const Instruction& instruction, bool pri, cell& value)
    {
        if (pri)
        {
            if (instruction.opcode == Opcode::CONST_PRI && !instruction.operands.empty())
            {
                value = instruction.operands[0];
                return true;
            }
            if (instruction.opcode == Opcode::ZERO_PRI)
            {
                value = 0;
                return true;
            }
        }
        else
        {
            if (instruction.opcode == Opcode::CONST_ALT && !instruction.operands.empty())
            {
                value = instruction.operands[0];
                return true;
            }
            if (instruction.opcode == Opcode::ZERO_ALT)
            {
                value = 0;
                return true;
            }
        }
        return false;
    }

    bool FindLoopEntryConstant(const DecodedProgram& decoded, const ControlFlowGraph& cfg, const std::vector<std::vector<std::size_t>>& reverse, const LoopAnalysis& loops,
                               const LoopMemorySummary& memorySummary, std::size_t componentId, std::size_t externalPredecessor, const LoopSlot& slot, cell& value)
    {
        const std::size_t count = decoded.instructions.size();
        std::size_t cursor = externalPredecessor;
        for (std::size_t scanned = 0; scanned < static_cast<std::size_t>(kLoopRangeMaxInitScan) && cursor < count; ++scanned)
        {
            const Instruction& instruction = decoded.instructions[cursor];
            if (IsZeroLoopStore(instruction, slot))
            {
                value = 0;
                return true;
            }

            LoopSlot written;
            bool fromPri = false;
            bool matchedStore = false;
            if (DecodeLoopStore(instruction, true, written))
            {
                fromPri = true;
                matchedStore = SameLoopSlot(written, slot);
            }
            else if (DecodeLoopStore(instruction, false, written))
            {
                fromPri = false;
                matchedStore = SameLoopSlot(written, slot);
            }

            if (matchedStore)
            {
                if (cursor >= reverse.size() || reverse[cursor].size() != 1)
                    return false;
                const std::size_t producer = reverse[cursor][0];
                if (producer >= count || loops.component[producer] == static_cast<int32_t>(componentId) || cfg.successors[producer].size() != 1 ||
                    cfg.successors[producer][0] != cursor)
                {
                    return false;
                }
                return DecodeLoopConstRegister(decoded.instructions[producer], fromPri, value);
            }

            LoopSlot directWrite;
            if (DecodeLoopDirectWrite(instruction, directWrite) && SameLoopSlot(directWrite, slot))
                return false;
            if (memorySummary.MayClobberSlot(cursor, slot) || IsLoopIndirectControlBarrier(instruction))
            {
                return false;
            }

            if (cursor >= reverse.size() || reverse[cursor].size() != 1)
                return false;
            const std::size_t predecessor = reverse[cursor][0];
            if (predecessor >= count || loops.component[predecessor] == static_cast<int32_t>(componentId) || cfg.successors[predecessor].size() != 1 ||
                cfg.successors[predecessor][0] != cursor)
            {
                return false;
            }
            cursor = predecessor;
        }
        return false;
    }

    LoopRangeAnalysis BuildLoopRangeAnalysis(AMX* amx, const DecodedProgram& decoded, const ControlFlowGraph& cfg, const LoopAnalysis& loops,
                                             const LoopMemorySummary& memorySummary)
    {
        LoopRangeAnalysis analysis;
        const std::size_t count = decoded.instructions.size();
        if (count == 0 || loops.components == 0)
            return analysis;

        std::vector<std::vector<std::size_t>> reverse(count);
        for (std::size_t from = 0; from < cfg.successors.size(); ++from)
        {
            for (std::size_t to : cfg.successors[from])
            {
                if (to < count)
                    reverse[to].push_back(from);
            }
        }

        for (std::size_t componentId = 0; componentId < loops.components; ++componentId)
        {
            bool isLoop = false;
            bool indirect = false;
            std::size_t externalEdges = 0;
            std::size_t externalPredecessor = count;
            std::size_t entryNode = count;

            for (std::size_t node = 0; node < count; ++node)
            {
                if (loops.component[node] != static_cast<int32_t>(componentId) || !loops.IsLoopInstruction(node))
                    continue;
                isLoop = true;
                if (IsLoopIndirectControlBarrier(decoded.instructions[node]))
                    indirect = true;
                for (std::size_t predecessor : reverse[node])
                {
                    if (predecessor >= count || loops.component[predecessor] != static_cast<int32_t>(componentId))
                    {
                        ++externalEdges;
                        externalPredecessor = predecessor;
                        entryNode = node;
                    }
                }
            }

            if (!isLoop)
                continue;
            if (indirect)
            {
                continue;
            }

            const bool componentClobbered = componentId >= memorySummary.clobberBarrier.size() || memorySummary.clobberBarrier[componentId] != 0;
            if (componentClobbered)

                if (externalEdges != 1 || externalPredecessor >= count || entryNode >= count || entryNode >= loops.loopHeader.size() || loops.loopHeader[entryNode] == 0)
                {
                    continue;
                }

            for (std::size_t guard = 0; guard + 4 <= count; ++guard)
            {
                if (loops.component[guard] != static_cast<int32_t>(componentId))
                    continue;

                LoopSlot induction;
                if (!DecodeLoopLoad(decoded.instructions[guard], true, induction))
                    continue;

                const Instruction& boundInstruction = decoded.instructions[guard + 1];
                const Instruction& compare = decoded.instructions[guard + 2];
                const Instruction& branch = decoded.instructions[guard + 3];
                if (!loops.SameLoop(guard, guard + 1) || !loops.SameLoop(guard, guard + 2) || !loops.SameLoop(guard, guard + 3) ||
                    (compare.opcode != Opcode::SLESS && compare.opcode != Opcode::LESS && compare.opcode != Opcode::SLEQ && compare.opcode != Opcode::LEQ) ||
                    (branch.opcode != Opcode::JZER && branch.opcode != Opcode::JNZ))
                {
                    continue;
                }

                cell boundValue = 0;
                if (boundInstruction.opcode == Opcode::CONST_ALT && !boundInstruction.operands.empty())
                {
                    boundValue = boundInstruction.operands[0];
                }
                else
                {
                    LoopSlot boundSlot;
                    if (!DecodeLoopLoad(boundInstruction, false, boundSlot) || !memorySummary.IsInvariant(loops, guard, boundSlot) ||
                        !FindLoopEntryConstant(decoded, cfg, reverse, loops, memorySummary, componentId, externalPredecessor, boundSlot, boundValue))
                    {
                        continue;
                    }
                }

                if (boundValue < 0)
                    continue;

                const cell takenCip = ResolveBranchTarget(amx, branch);
                const cell fallthroughCip = branch.Next();
                auto takenIt = decoded.byCip.find(takenCip);
                auto fallthroughIt = decoded.byCip.find(fallthroughCip);
                if (takenIt == decoded.byCip.end() || fallthroughIt == decoded.byCip.end())
                    continue;
                const bool takenInside = loops.component[takenIt->second] == static_cast<int32_t>(componentId);
                const bool fallthroughInside = loops.component[fallthroughIt->second] == static_cast<int32_t>(componentId);
                if (takenInside == fallthroughInside)
                    continue;

                const bool branchTakenWhenTrue = branch.opcode == Opcode::JNZ;
                const bool continueWhenTrue = takenInside ? branchTakenWhenTrue : !branchTakenWhenTrue;
                if (!continueWhenTrue)
                    continue;
                const std::size_t continueIndex = takenInside ? takenIt->second : fallthroughIt->second;

                const int64_t max64 = (compare.opcode == Opcode::SLEQ || compare.opcode == Opcode::LEQ) ? static_cast<int64_t>(boundValue) : static_cast<int64_t>(boundValue) - 1;
                if (max64 < 0 || max64 > std::numeric_limits<cell>::max())
                    continue;

                /*
                    the taken/fallthrough decision itself proves an upper
                    bound for the current iteration. Unlike the canonical
                    induction proof below, this fact only needs to survive
                    from the guard to the use. A debug/native/call later in
                    the loop therefore no longer disables a BOUNDS
                    optimization that occurs before that clobber.
                */
                LoopGuardBoundFact guardFact;
                guardFact.slot = induction;
                guardFact.maxInclusive = static_cast<cell>(max64);
                guardFact.nonNegative = compare.opcode == Opcode::LESS || compare.opcode == Opcode::LEQ;
                guardFact.safeInstruction.assign(count, 0);

                std::vector<std::size_t> guardQueue{continueIndex};
                for (std::size_t cursor = 0; cursor < guardQueue.size(); ++cursor)
                {
                    const std::size_t node = guardQueue[cursor];
                    if (node >= count || guardFact.safeInstruction[node] != 0 || loops.component[node] != static_cast<int32_t>(componentId) || node == guard)
                    {
                        continue;
                    }

                    guardFact.safeInstruction[node] = 1;

                    if (memorySummary.MayClobberSlot(node, induction) || IsLoopIndirectControlBarrier(decoded.instructions[node]))
                    {
                        continue;
                    }

                    for (std::size_t successor : cfg.successors[node])
                    {
                        if (successor < count && successor != guard && loops.component[successor] == static_cast<int32_t>(componentId))
                        {
                            guardQueue.push_back(successor);
                        }
                    }
                }

                analysis.guardFacts.push_back(std::move(guardFact));

                /*
                    A canonical induction proof establishes both the lower and
                    upper range across iterations. Unknown memory clobbers can
                    mutate the induction cell between iterations, so keep this
                    stronger proof conservative even though guard-local facts
                    above remain useful before the clobber point.
                */
                if (componentClobbered)
                    continue;

                std::size_t updateIndex = count;
                std::size_t updateCount = 0;
                bool invalidWrite = false;
                for (std::size_t node = 0; node < count; ++node)
                {
                    if (loops.component[node] != static_cast<int32_t>(componentId))
                        continue;
                    LoopSlot counterSlot;
                    bool increment = false;
                    if (DecodeLoopMemoryCounter(decoded.instructions[node], counterSlot, increment) && SameLoopSlot(counterSlot, induction))
                    {
                        if (!increment)
                        {
                            invalidWrite = true;
                            break;
                        }
                        updateIndex = node;
                        ++updateCount;
                        continue;
                    }
                    LoopSlot written;
                    if (DecodeLoopDirectWrite(decoded.instructions[node], written) && SameLoopSlot(written, induction))
                    {
                        invalidWrite = true;
                        break;
                    }
                }
                if (invalidWrite || updateCount != 1 || updateIndex >= count)
                    continue;

                cell initialValue = 0;
                if (!FindLoopEntryConstant(decoded, cfg, reverse, loops, memorySummary, componentId, externalPredecessor, induction, initialValue))
                {
                    continue;
                }
                if (initialValue < 0)
                    continue;
                /*
                    signed <= INT_MAX followed by INC can wrap to INT_MIN and
                    satisfy the guard again, invalidating the non-negative
                    range.
                */
                if (compare.opcode == Opcode::SLEQ && boundValue == std::numeric_limits<cell>::max())
                    continue;
                if (static_cast<int64_t>(initialValue) > max64)
                    continue;

                LoopRangeFact fact;
                fact.slot = induction;
                fact.minInclusive = initialValue;
                fact.maxInclusive = static_cast<cell>(max64);
                fact.safeInstruction.assign(count, 0);

                std::vector<std::size_t> queue;
                queue.push_back(continueIndex);
                for (std::size_t cursor = 0; cursor < queue.size(); ++cursor)
                {
                    const std::size_t node = queue[cursor];
                    if (node >= count || fact.safeInstruction[node] != 0 || loops.component[node] != static_cast<int32_t>(componentId) || node == updateIndex || node == guard)
                    {
                        continue;
                    }
                    fact.safeInstruction[node] = 1;
                    for (std::size_t successor : cfg.successors[node])
                    {
                        if (successor < count && successor != updateIndex && successor != guard && loops.component[successor] == static_cast<int32_t>(componentId))
                        {
                            queue.push_back(successor);
                        }
                    }
                }

                analysis.facts.push_back(std::move(fact));
                break;
            }
        }
        return analysis;
    }

    struct LoopInductionCompareBranchBlock
    {
        LoopSlot inductionSlot;
        LoopSlot boundSlot;
        bool increment = true;
        bool boundFromMemory = false;
        cell immediate = 0;
        Opcode compareOpcode = Opcode::NONE;
        Opcode branchOpcode = Opcode::NONE;
        cell takenCip = 0;
        cell fallthroughCip = 0;
        bool priLiveTaken = false;
        bool priLiveFallthrough = false;
        bool altLive = false;
        std::size_t foldedInstructions = 0;
    };

    bool BuildLoopInductionCompareBranchBlock(AMX* amx, const DecodedProgram& decoded, const LoopAnalysis& loops, const LocalLiveness& liveness, std::size_t startIndex,
                                              LoopInductionCompareBranchBlock& block)
    {
        const std::size_t count = decoded.instructions.size();
        if (startIndex + 5 > count || !loops.IsLoopInstruction(startIndex))
            return false;

        LoopSlot induction;
        bool increment = true;
        if (!DecodeLoopMemoryCounter(decoded.instructions[startIndex], induction, increment))
            return false;

        LoopSlot loaded;
        if (!DecodeLoopLoad(decoded.instructions[startIndex + 1], true, loaded) || !SameLoopSlot(induction, loaded))
            return false;

        const Instruction& bound = decoded.instructions[startIndex + 2];
        const Instruction& compare = decoded.instructions[startIndex + 3];
        const Instruction& branch = decoded.instructions[startIndex + 4];
        for (std::size_t index = startIndex + 1; index <= startIndex + 4; ++index)
        {
            if (!loops.SameLoop(startIndex, index))
                return false;
        }
        if (!IsLoopCompareOpcode(compare.opcode) || (branch.opcode != Opcode::JZER && branch.opcode != Opcode::JNZ))
            return false;

        bool boundFromMemory = false;
        LoopSlot boundSlot;
        cell immediate = 0;
        if (bound.opcode == Opcode::CONST_ALT && !bound.operands.empty())
        {
            immediate = bound.operands[0];
        }
        else if (DecodeLoopLoad(bound, false, boundSlot))
        {
            boundFromMemory = true;
        }
        else
        {
            return false;
        }

        const cell takenCip = ResolveBranchTarget(amx, branch);
        const cell fallthroughCip = branch.Next();
        auto takenIt = decoded.byCip.find(takenCip);
        auto fallthroughIt = decoded.byCip.find(fallthroughCip);
        if (takenIt == decoded.byCip.end() || fallthroughIt == decoded.byCip.end())
            return false;

        block.inductionSlot = induction;
        block.boundSlot = boundSlot;
        block.increment = increment;
        block.boundFromMemory = boundFromMemory;
        block.immediate = immediate;
        block.compareOpcode = compare.opcode;
        block.branchOpcode = branch.opcode;
        block.takenCip = takenCip;
        block.fallthroughCip = fallthroughCip;
        block.priLiveTaken = liveness.IsLiveIn(takenIt->second, RegPri);
        block.priLiveFallthrough = liveness.IsLiveIn(fallthroughIt->second, RegPri);
        block.altLive = liveness.IsLiveIn(takenIt->second, RegAlt) || liveness.IsLiveIn(fallthroughIt->second, RegAlt);
        block.foldedInstructions = 5;
        return true;
    }

    struct LoopMemoryCompareBranchBlock
    {
        LoopSlot lhsSlot;
        LoopSlot rhsSlot;
        Opcode compareOpcode = Opcode::NONE;
        Opcode branchOpcode = Opcode::NONE;
        cell takenCip = 0;
        cell fallthroughCip = 0;
        bool priLiveTaken = false;
        bool priLiveFallthrough = false;
        bool altLive = false;
        std::size_t foldedInstructions = 0;
    };

    bool BuildLoopMemoryCompareBranchBlock(AMX* amx, const DecodedProgram& decoded, const LoopAnalysis& loops, const LocalLiveness& liveness, std::size_t startIndex,
                                           LoopMemoryCompareBranchBlock& block)
    {
        const std::size_t count = decoded.instructions.size();
        if (startIndex + 4 > count || !loops.IsLoopInstruction(startIndex))
            return false;
        LoopSlot lhs, rhs;
        if (!DecodeLoopLoad(decoded.instructions[startIndex], true, lhs) || !DecodeLoopLoad(decoded.instructions[startIndex + 1], false, rhs))
            return false;
        const Instruction& compare = decoded.instructions[startIndex + 2];
        const Instruction& branch = decoded.instructions[startIndex + 3];
        if (!loops.SameLoop(startIndex, startIndex + 1) || !loops.SameLoop(startIndex, startIndex + 2) || !loops.SameLoop(startIndex, startIndex + 3) ||
            !IsLoopCompareOpcode(compare.opcode) || (branch.opcode != Opcode::JZER && branch.opcode != Opcode::JNZ))
            return false;

        const cell takenCip = ResolveBranchTarget(amx, branch);
        const cell fallthroughCip = branch.Next();
        auto takenIt = decoded.byCip.find(takenCip);
        auto fallthroughIt = decoded.byCip.find(fallthroughCip);
        if (takenIt == decoded.byCip.end() || fallthroughIt == decoded.byCip.end())
            return false;

        block.lhsSlot = lhs;
        block.rhsSlot = rhs;
        block.compareOpcode = compare.opcode;
        block.branchOpcode = branch.opcode;
        block.takenCip = takenCip;
        block.fallthroughCip = fallthroughCip;
        block.priLiveTaken = liveness.IsLiveIn(takenIt->second, RegPri);
        block.priLiveFallthrough = liveness.IsLiveIn(fallthroughIt->second, RegPri);
        block.altLive = liveness.IsLiveIn(takenIt->second, RegAlt) || liveness.IsLiveIn(fallthroughIt->second, RegAlt);
        block.foldedInstructions = 4;
        return true;
    }

    struct LoopBoundsIndexBlock
    {
        LoopSlot indexSlot;
        cell upperBound = 0;
        cell baseOffset = 0;
        uint8_t indexShift = 2;
        cell boundsErrorCip = 0;
        cell memoryErrorCip = 0;
        cell targetCip = 0;
        bool baseFrameRelative = false;
        LoopBoundsProof boundsProof = LoopBoundsProof::None;
        bool updateAfterAccess = false;
        bool incrementAfterAccess = true;
        bool altLive = false;
        std::size_t foldedInstructions = 0;
    };

    bool BuildLoopBoundsIndexBlock(const DecodedProgram& decoded, const LoopAnalysis& loops, const LoopRangeAnalysis& ranges, const LocalLiveness& liveness, std::size_t startIndex,
                                   LoopBoundsIndexBlock& block)
    {
        const std::size_t count = decoded.instructions.size();
        if (startIndex + 4 > count || !loops.IsLoopInstruction(startIndex))
            return false;
        LoopSlot indexSlot;
        if (!DecodeLoopLoad(decoded.instructions[startIndex], true, indexSlot))
            return false;
        const Instruction& bounds = decoded.instructions[startIndex + 1];
        const Instruction& base = decoded.instructions[startIndex + 2];
        const Instruction& indexed = decoded.instructions[startIndex + 3];
        if (!loops.SameLoop(startIndex, startIndex + 1) || !loops.SameLoop(startIndex, startIndex + 2) || !loops.SameLoop(startIndex, startIndex + 3) ||
            bounds.opcode != Opcode::BOUNDS || bounds.operands.empty() || (base.opcode != Opcode::CONST_ALT && base.opcode != Opcode::ADDR_ALT) || base.operands.empty() ||
            (indexed.opcode != Opcode::LIDX && indexed.opcode != Opcode::LIDX_B))
            return false;

        uint8_t shift = 2;
        if (indexed.opcode == Opcode::LIDX_B)
        {
            if (indexed.operands.empty())
                return false;
            const ucell rawShift = static_cast<ucell>(indexed.operands[0]);
            if (rawShift > 31u)
                return false;
            shift = static_cast<uint8_t>(rawShift);
        }

        block.indexSlot = indexSlot;
        block.upperBound = bounds.operands[0];
        block.baseOffset = base.operands[0];
        block.baseFrameRelative = base.opcode == Opcode::ADDR_ALT;
        block.indexShift = shift;
        block.boundsErrorCip = bounds.Next();
        block.memoryErrorCip = indexed.cip;
        block.boundsProof = ranges.ProveBounds(startIndex, indexSlot, block.upperBound);
        block.targetCip = indexed.Next();
        block.foldedInstructions = 4;

        const std::size_t updateIndex = startIndex + 4;
        if (updateIndex < count && loops.SameLoop(startIndex, updateIndex))
        {
            LoopSlot updateSlot;
            bool increment = true;
            if (DecodeLoopMemoryCounter(decoded.instructions[updateIndex], updateSlot, increment) && SameLoopSlot(updateSlot, indexSlot))
            {
                block.updateAfterAccess = true;
                block.incrementAfterAccess = increment;
                block.targetCip = decoded.instructions[updateIndex].Next();
                block.foldedInstructions = 5;
            }
        }

        auto targetIt = decoded.byCip.find(block.targetCip);
        if (targetIt == decoded.byCip.end())
            return false;
        block.altLive = liveness.IsLiveIn(targetIt->second, RegAlt);
        return true;
    }

    struct LoopBoundsAddressBlock
    {
        LoopSlot indexSlot;
        cell upperBound = 0;
        cell baseOffset = 0;
        uint8_t indexShift = 2;
        cell boundsErrorCip = 0;
        cell targetCip = 0;
        bool baseFrameRelative = false;
        LoopBoundsProof boundsProof = LoopBoundsProof::None;
        bool updateAfterAccess = false;
        bool incrementAfterAccess = true;
        bool altLive = false;
        std::size_t foldedInstructions = 0;
    };

    bool BuildLoopBoundsAddressBlock(const DecodedProgram& decoded, const LoopAnalysis& loops, const LoopRangeAnalysis& ranges, const LocalLiveness& liveness,
                                     std::size_t startIndex, LoopBoundsAddressBlock& block)
    {
        const std::size_t count = decoded.instructions.size();
        if (startIndex + 4 > count || !loops.IsLoopInstruction(startIndex))
            return false;
        LoopSlot indexSlot;
        if (!DecodeLoopLoad(decoded.instructions[startIndex], true, indexSlot))
            return false;
        const Instruction& bounds = decoded.instructions[startIndex + 1];
        const Instruction& base = decoded.instructions[startIndex + 2];
        const Instruction& indexed = decoded.instructions[startIndex + 3];
        if (!loops.SameLoop(startIndex, startIndex + 1) || !loops.SameLoop(startIndex, startIndex + 2) || !loops.SameLoop(startIndex, startIndex + 3) ||
            bounds.opcode != Opcode::BOUNDS || bounds.operands.empty() || (base.opcode != Opcode::CONST_ALT && base.opcode != Opcode::ADDR_ALT) || base.operands.empty() ||
            (indexed.opcode != Opcode::IDXADDR && indexed.opcode != Opcode::IDXADDR_B))
            return false;

        uint8_t shift = 2;
        if (indexed.opcode == Opcode::IDXADDR_B)
        {
            if (indexed.operands.empty())
                return false;
            const ucell rawShift = static_cast<ucell>(indexed.operands[0]);
            if (rawShift > 31u)
                return false;
            shift = static_cast<uint8_t>(rawShift);
        }

        block.indexSlot = indexSlot;
        block.upperBound = bounds.operands[0];
        block.baseOffset = base.operands[0];
        block.baseFrameRelative = base.opcode == Opcode::ADDR_ALT;
        block.indexShift = shift;
        block.boundsErrorCip = bounds.Next();
        block.boundsProof = ranges.ProveBounds(startIndex, indexSlot, block.upperBound);
        block.targetCip = indexed.Next();
        block.foldedInstructions = 4;

        const std::size_t updateIndex = startIndex + 4;
        if (updateIndex < count && loops.SameLoop(startIndex, updateIndex))
        {
            LoopSlot updateSlot;
            bool increment = true;
            if (DecodeLoopMemoryCounter(decoded.instructions[updateIndex], updateSlot, increment) && SameLoopSlot(updateSlot, indexSlot))
            {
                block.updateAfterAccess = true;
                block.incrementAfterAccess = increment;
                block.targetCip = decoded.instructions[updateIndex].Next();
                block.foldedInstructions = 5;
            }
        }

        auto targetIt = decoded.byCip.find(block.targetCip);
        if (targetIt == decoded.byCip.end())
            return false;
        block.altLive = liveness.IsLiveIn(targetIt->second, RegAlt);
        return true;
    }

    bool BuildLoopCounterBranchBlock(AMX* amx, const DecodedProgram& decoded, const LoopAnalysis& loops, const LocalLiveness& liveness, std::size_t startIndex,
                                     LoopCounterBranchBlock& block)
    {
        const std::size_t count = decoded.instructions.size();
        if (startIndex + 3 > count || !loops.IsLoopInstruction(startIndex))
            return false;

        LoopSlot slot;
        bool increment = true;
        if (!DecodeLoopMemoryCounter(decoded.instructions[startIndex], slot, increment))
            return false;

        LoopSlot loadSlot;
        if (!DecodeLoopLoad(decoded.instructions[startIndex + 1], true, loadSlot) || !SameLoopSlot(slot, loadSlot))
        {
            return false;
        }

        const Instruction& branch = decoded.instructions[startIndex + 2];
        if ((branch.opcode != Opcode::JZER && branch.opcode != Opcode::JNZ) || !loops.SameLoop(startIndex, startIndex + 1) || !loops.SameLoop(startIndex, startIndex + 2))
        {
            return false;
        }

        const cell takenCip = ResolveBranchTarget(amx, branch);
        const cell fallthroughCip = branch.Next();
        auto takenIt = decoded.byCip.find(takenCip);
        auto fallthroughIt = decoded.byCip.find(fallthroughCip);
        if (takenIt == decoded.byCip.end() || fallthroughIt == decoded.byCip.end())
            return false;

        block.slot = slot;
        block.increment = increment;
        block.branchOpcode = branch.opcode;
        block.takenCip = takenCip;
        block.fallthroughCip = fallthroughCip;
        block.priLive = liveness.IsLiveIn(takenIt->second, RegPri) || liveness.IsLiveIn(fallthroughIt->second, RegPri);
        block.foldedInstructions = 3;
        return true;
    }

    struct LoopCompareBranchBlock
    {
        LoopSlot slot;
        cell immediate = 0;
        Opcode compareOpcode = Opcode::NONE;
        Opcode branchOpcode = Opcode::NONE;
        cell takenCip = 0;
        cell fallthroughCip = 0;
        bool priLiveTaken = false;
        bool priLiveFallthrough = false;
        bool altLive = false;
        std::size_t foldedInstructions = 0;
    };

    bool BuildLoopCompareBranchBlock(AMX* amx, const DecodedProgram& decoded, const LoopAnalysis& loops, const LocalLiveness& liveness, std::size_t startIndex,
                                     LoopCompareBranchBlock& block)
    {
        const std::size_t count = decoded.instructions.size();
        if (startIndex + 4 > count || !loops.IsLoopInstruction(startIndex))
            return false;

        LoopSlot slot;
        if (!DecodeLoopLoad(decoded.instructions[startIndex], true, slot))
            return false;

        const Instruction& constant = decoded.instructions[startIndex + 1];
        const Instruction& compare = decoded.instructions[startIndex + 2];
        const Instruction& branch = decoded.instructions[startIndex + 3];

        if (!loops.SameLoop(startIndex, startIndex + 1) || !loops.SameLoop(startIndex, startIndex + 2) || !loops.SameLoop(startIndex, startIndex + 3) ||
            constant.opcode != Opcode::CONST_ALT || constant.operands.empty() || !IsLoopCompareOpcode(compare.opcode) ||
            (branch.opcode != Opcode::JZER && branch.opcode != Opcode::JNZ))
        {
            return false;
        }

        const cell takenCip = ResolveBranchTarget(amx, branch);
        const cell fallthroughCip = branch.Next();
        auto takenIt = decoded.byCip.find(takenCip);
        auto fallthroughIt = decoded.byCip.find(fallthroughCip);
        if (takenIt == decoded.byCip.end() || fallthroughIt == decoded.byCip.end())
            return false;

        block.slot = slot;
        block.immediate = constant.operands[0];
        block.compareOpcode = compare.opcode;
        block.branchOpcode = branch.opcode;
        block.takenCip = takenCip;
        block.fallthroughCip = fallthroughCip;
        block.priLiveTaken = liveness.IsLiveIn(takenIt->second, RegPri);
        block.priLiveFallthrough = liveness.IsLiveIn(fallthroughIt->second, RegPri);
        block.altLive = liveness.IsLiveIn(takenIt->second, RegAlt) || liveness.IsLiveIn(fallthroughIt->second, RegAlt);
        block.foldedInstructions = 4;
        return true;
    }

    enum class LoopRmwKind : uint8_t
    {
        Add,
        Sub,
        And,
        Or,
        Xor,
        Shl,
        Shr,
        Sar,
        Mul,
        Neg,
        Invert,
        Inc,
        Dec,
    };

    struct LoopRmwBlock
    {
        LoopSlot slot;
        LoopRmwKind kind = LoopRmwKind::Add;
        cell immediate = 0;
        cell targetCip = 0;
        bool resultInPri = true;
        bool materializeResult = false;
        bool materializeAlt = false;
        std::size_t foldedInstructions = 0;
    };

    bool DecodeUnaryLoopRmw(Opcode opcode, bool pri, LoopRmwKind& kind)
    {
        if (pri)
        {
            if (opcode == Opcode::INC_PRI)
            {
                kind = LoopRmwKind::Inc;
                return true;
            }
            if (opcode == Opcode::DEC_PRI)
            {
                kind = LoopRmwKind::Dec;
                return true;
            }
            if (opcode == Opcode::NEG)
            {
                kind = LoopRmwKind::Neg;
                return true;
            }
            if (opcode == Opcode::INVERT)
            {
                kind = LoopRmwKind::Invert;
                return true;
            }
            return false;
        }
        if (opcode == Opcode::INC_ALT)
        {
            kind = LoopRmwKind::Inc;
            return true;
        }
        if (opcode == Opcode::DEC_ALT)
        {
            kind = LoopRmwKind::Dec;
            return true;
        }
        return false;
    }

    bool DecodeImmediateLoopRmw(Opcode opcode, bool pri, LoopRmwKind& kind)
    {
        if (pri)
        {
            if (opcode == Opcode::ADD_C)
            {
                kind = LoopRmwKind::Add;
                return true;
            }
            if (opcode == Opcode::SMUL_C)
            {
                kind = LoopRmwKind::Mul;
                return true;
            }
            if (opcode == Opcode::SHL_C_PRI)
            {
                kind = LoopRmwKind::Shl;
                return true;
            }
            if (opcode == Opcode::SHR_C_PRI)
            {
                kind = LoopRmwKind::Shr;
                return true;
            }
            return false;
        }
        if (opcode == Opcode::SHL_C_ALT)
        {
            kind = LoopRmwKind::Shl;
            return true;
        }
        if (opcode == Opcode::SHR_C_ALT)
        {
            kind = LoopRmwKind::Shr;
            return true;
        }
        return false;
    }

    bool DecodeAltConstantLoopRmw(Opcode opcode, LoopRmwKind& kind)
    {
        switch (opcode)
        {
            case Opcode::ADD:
                kind = LoopRmwKind::Add;
                return true;
            case Opcode::SUB:
                kind = LoopRmwKind::Sub;
                return true;
            case Opcode::AND:
                kind = LoopRmwKind::And;
                return true;
            case Opcode::OR:
                kind = LoopRmwKind::Or;
                return true;
            case Opcode::XOR:
                kind = LoopRmwKind::Xor;
                return true;
            case Opcode::SHL:
                kind = LoopRmwKind::Shl;
                return true;
            case Opcode::SHR:
                kind = LoopRmwKind::Shr;
                return true;
            case Opcode::SSHR:
                kind = LoopRmwKind::Sar;
                return true;
            case Opcode::SMUL:
            case Opcode::UMUL:
                kind = LoopRmwKind::Mul;
                return true;
            default:
                return false;
        }
    }

    bool IsPositivePowerOfTwo(cell value, uint8_t& shift)
    {
        const ucell bits = static_cast<ucell>(value);
        if (value <= 0 || (bits & (bits - 1u)) != 0)
            return false;
        shift = 0;
        for (ucell copy = bits; copy > 1u; copy >>= 1u)
            ++shift;
        return true;
    }

    bool BuildLoopRmwBlock(const DecodedProgram& decoded, const LoopAnalysis& loops, const LocalLiveness& liveness, std::size_t startIndex, LoopRmwBlock& block)
    {
        const std::size_t count = decoded.instructions.size();
        if (startIndex + 3 > count || !loops.IsLoopInstruction(startIndex))
            return false;

        LoopSlot loadSlot;
        bool resultInPri = true;
        if (!DecodeLoopLoad(decoded.instructions[startIndex], true, loadSlot))
        {
            resultInPri = false;
            if (!DecodeLoopLoad(decoded.instructions[startIndex], false, loadSlot))
                return false;
        }

        LoopRmwKind kind = LoopRmwKind::Add;
        cell immediate = 0;
        std::size_t storeIndex = startIndex + 2;
        bool altWasConstant = false;
        bool matched = false;
        const Instruction& op1 = decoded.instructions[startIndex + 1];

        if (DecodeUnaryLoopRmw(op1.opcode, resultInPri, kind))
        {
            matched = true;
        }
        else if (!op1.operands.empty() && DecodeImmediateLoopRmw(op1.opcode, resultInPri, kind))
        {
            immediate = op1.operands[0];
            matched = true;
        }
        else if (resultInPri && startIndex + 4 <= count && op1.opcode == Opcode::CONST_ALT && !op1.operands.empty())
        {
            const Instruction& op2 = decoded.instructions[startIndex + 2];
            if (DecodeAltConstantLoopRmw(op2.opcode, kind))
            {
                immediate = op1.operands[0];
                storeIndex = startIndex + 3;
                altWasConstant = true;
                matched = true;
            }
        }

        if (!matched || storeIndex >= count)
            return false;

        for (std::size_t index = startIndex + 1; index <= storeIndex; ++index)
        {
            if (!loops.SameLoop(startIndex, index))
                return false;
        }

        LoopSlot storeSlot;
        if (!DecodeLoopStore(decoded.instructions[storeIndex], resultInPri, storeSlot) || !SameLoopSlot(loadSlot, storeSlot) || storeIndex + 1 >= count)
        {
            return false;
        }

        block.slot = loadSlot;
        block.kind = kind;
        block.immediate = immediate;
        block.targetCip = decoded.instructions[storeIndex + 1].cip;
        block.resultInPri = resultInPri;
        block.materializeResult = liveness.IsLiveIn(storeIndex + 1, resultInPri ? RegPri : RegAlt);
        block.materializeAlt = altWasConstant && liveness.IsLiveIn(storeIndex + 1, RegAlt);
        block.foldedInstructions = storeIndex - startIndex + 1;

        return true;
    }

    asmjit::x86::Mem LoopSlotMem(const LoopSlot& slot, uint32_t dataBase)
    {
        if (slot.kind == LoopSlotKind::Global)
            return DataAbs(dataBase, slot.offset);
        return asmjit::x86::dword_ptr(asmjit::x86::ebp, DataDisp(dataBase, slot.offset));
    }

    void EmitLoopRmwOperation(asmjit::x86::Assembler& a, const LoopRmwBlock& block, uint32_t dataBase)
    {
        using namespace asmjit::x86;
        const Mem memory = LoopSlotMem(block.slot, dataBase);
        switch (block.kind)
        {
            case LoopRmwKind::Add:
                if (block.immediate != 0)
                    a.add(memory, block.immediate);
                break;
            case LoopRmwKind::Sub:
                if (block.immediate != 0)
                    a.sub(memory, block.immediate);
                break;
            case LoopRmwKind::And:
                a.and_(memory, block.immediate);
                break;
            case LoopRmwKind::Or:
                if (block.immediate != 0)
                    a.or_(memory, block.immediate);
                break;
            case LoopRmwKind::Xor:
                if (block.immediate != 0)
                    a.xor_(memory, block.immediate);
                break;
            case LoopRmwKind::Shl:
            {
                const uint8_t shift = static_cast<uint8_t>(static_cast<ucell>(block.immediate) & 31u);
                if (shift != 0)
                    a.shl(memory, shift);
                break;
            }
            case LoopRmwKind::Shr:
            {
                const uint8_t shift = static_cast<uint8_t>(static_cast<ucell>(block.immediate) & 31u);
                if (shift != 0)
                    a.shr(memory, shift);
                break;
            }
            case LoopRmwKind::Sar:
            {
                const uint8_t shift = static_cast<uint8_t>(static_cast<ucell>(block.immediate) & 31u);
                if (shift != 0)
                    a.sar(memory, shift);
                break;
            }
            case LoopRmwKind::Mul:
            {
                if (block.immediate == 0)
                {
                    a.mov(memory, 0);
                    break;
                }
                if (block.immediate == 1)
                    break;
                if (block.immediate == -1)
                {
                    a.neg(memory);
                    break;
                }
                uint8_t shift = 0;
                if (IsPositivePowerOfTwo(block.immediate, shift))
                {
                    a.shl(memory, shift);
                    break;
                }
                a.imul(eax, memory, block.immediate);
                a.mov(memory, eax);
                break;
            }
            case LoopRmwKind::Neg:
                a.neg(memory);
                break;
            case LoopRmwKind::Invert:
                a.not_(memory);
                break;
            case LoopRmwKind::Inc:
                a.inc(memory);
                break;
            case LoopRmwKind::Dec:
                a.dec(memory);
                break;
        }

        if (block.materializeResult)
        {
            if (block.resultInPri)
                a.mov(edi, memory);
            else
                a.mov(ebx, memory);
        }
        if (block.materializeAlt)
            EmitLoopKnownConstant(a, ebx, block.immediate);
    }

    void EmitLoopBooleanBranchResult(asmjit::x86::Assembler& a, Opcode compareOpcode, Opcode branchOpcode, bool priLiveTaken, bool priLiveFallthrough,
                                     const asmjit::Label& takenTarget, const asmjit::Label& fallthroughTarget)
    {
        const bool takenWhenTrue = branchOpcode == Opcode::JNZ;
        const cell takenPri = takenWhenTrue ? 1 : 0;
        const cell fallthroughPri = takenWhenTrue ? 0 : 1;
        asmjit::Label takenPath = a.new_label();
        EmitComparisonJump(a, compareOpcode, takenPath, takenWhenTrue);
        if (priLiveFallthrough)
            EmitLoopKnownConstant(a, asmjit::x86::edi, fallthroughPri);
        a.jmp(fallthroughTarget);
        a.bind(takenPath);
        if (priLiveTaken)
            EmitLoopKnownConstant(a, asmjit::x86::edi, takenPri);
        a.jmp(takenTarget);
    }

    void EmitLoopInductionCompareBranch(asmjit::x86::Assembler& a, const LoopInductionCompareBranchBlock& block, uint32_t dataBase, const asmjit::Label& takenTarget,
                                        const asmjit::Label& fallthroughTarget)
    {
        using namespace asmjit::x86;
        const Mem induction = LoopSlotMem(block.inductionSlot, dataBase);
        if (block.increment)
            a.inc(induction);
        else
            a.dec(induction);

        if (block.boundFromMemory)
        {
            const Mem bound = LoopSlotMem(block.boundSlot, dataBase);
            if (block.altLive)
            {
                a.mov(ebx, bound);
                a.cmp(induction, ebx);
            }
            else
            {
                a.mov(eax, bound);
                a.cmp(induction, eax);
            }
        }
        else
        {
            if (block.altLive)
                EmitLoopKnownConstant(a, ebx, block.immediate);
            a.cmp(induction, block.immediate);
        }

        EmitLoopBooleanBranchResult(a, block.compareOpcode, block.branchOpcode, block.priLiveTaken, block.priLiveFallthrough, takenTarget, fallthroughTarget);
    }

    void EmitLoopMemoryCompareBranch(asmjit::x86::Assembler& a, const LoopMemoryCompareBranchBlock& block, uint32_t dataBase, const asmjit::Label& takenTarget,
                                     const asmjit::Label& fallthroughTarget)
    {
        using namespace asmjit::x86;
        const Mem lhs = LoopSlotMem(block.lhsSlot, dataBase);
        const Mem rhs = LoopSlotMem(block.rhsSlot, dataBase);
        if (block.altLive)
        {
            a.mov(ebx, rhs);
            a.cmp(lhs, ebx);
        }
        else
        {
            a.mov(eax, rhs);
            a.cmp(lhs, eax);
        }
        EmitLoopBooleanBranchResult(a, block.compareOpcode, block.branchOpcode, block.priLiveTaken, block.priLiveFallthrough, takenTarget, fallthroughTarget);
    }

    void EmitLoopBoundsIndex(asmjit::x86::Assembler& a, const LoopBoundsIndexBlock& block, uint32_t dataBase, cell stp, const asmjit::Label& boundsError,
                             const asmjit::Label& memoryError)
    {
        using namespace asmjit::x86;
        const Mem index = LoopSlotMem(block.indexSlot, dataBase);
        a.mov(eax, index);
        if (block.boundsProof != LoopBoundsProof::Full)
        {
            a.mov(Ctx(offsetof(JitContext, cip)), block.boundsErrorCip);
            if (block.boundsProof == LoopBoundsProof::UpperOnly)
            {
                /*
                    A signed loop guard already proved the upper half of the
                    BOUNDS predicate. BOUNDS itself is unsigned, so retain
                    only the negative-index rejection needed for exact
                    semantics.
                */
                a.test(eax, eax);
                a.js(boundsError);
            }
            else
            {
                a.cmp(eax, block.upperBound);
                a.ja(boundsError);
            }
        }

        if (block.altLive)
        {
            if (block.baseFrameRelative)
            {
                a.mov(ebx, ebp);
                if (block.baseOffset != 0)
                    a.add(ebx, block.baseOffset);
            }
            else
            {
                EmitLoopKnownConstant(a, ebx, block.baseOffset);
            }
        }

        a.mov(edx, eax);
        if (block.indexShift != 0)
            a.shl(edx, block.indexShift);
        if (block.baseFrameRelative)
            a.add(edx, ebp);
        if (block.baseOffset != 0)
            a.add(edx, block.baseOffset);
        a.mov(Ctx(offsetof(JitContext, cip)), block.memoryErrorCip);
        EmitCheckAddress(a, dataBase, edx, sizeof(cell), stp, memoryError);
        a.mov(edi, DataAt(edx, dataBase));

        if (block.updateAfterAccess)
        {
            if (block.incrementAfterAccess)
                a.inc(index);
            else
                a.dec(index);
        }
    }

    void EmitLoopBoundsAddress(asmjit::x86::Assembler& a, const LoopBoundsAddressBlock& block, uint32_t dataBase, const asmjit::Label& boundsError)
    {
        using namespace asmjit::x86;
        const Mem index = LoopSlotMem(block.indexSlot, dataBase);
        a.mov(eax, index);
        if (block.boundsProof != LoopBoundsProof::Full)
        {
            a.mov(Ctx(offsetof(JitContext, cip)), block.boundsErrorCip);
            if (block.boundsProof == LoopBoundsProof::UpperOnly)
            {
                /*
                    A signed loop guard already proved the upper half of the
                    BOUNDS predicate. BOUNDS itself is unsigned, so retain
                    only the negative-index rejection needed for exact
                    semantics.
                */
                a.test(eax, eax);
                a.js(boundsError);
            }
            else
            {
                a.cmp(eax, block.upperBound);
                a.ja(boundsError);
            }
        }

        if (block.altLive)
        {
            if (block.baseFrameRelative)
            {
                a.mov(ebx, ebp);
                if (block.baseOffset != 0)
                    a.add(ebx, block.baseOffset);
            }
            else
            {
                EmitLoopKnownConstant(a, ebx, block.baseOffset);
            }
        }

        a.mov(edi, eax);
        if (block.indexShift != 0)
            a.shl(edi, block.indexShift);
        if (block.baseFrameRelative)
            a.add(edi, ebp);
        if (block.baseOffset != 0)
            a.add(edi, block.baseOffset);

        if (block.updateAfterAccess)
        {
            if (block.incrementAfterAccess)
                a.inc(index);
            else
                a.dec(index);
        }
    }

    void EmitLoopCounterBranch(asmjit::x86::Assembler& a, const LoopCounterBranchBlock& block, uint32_t dataBase, const asmjit::Label& takenTarget,
                               const asmjit::Label& fallthroughTarget)
    {
        using namespace asmjit::x86;
        const Mem memory = LoopSlotMem(block.slot, dataBase);
        if (block.increment)
            a.inc(memory);
        else
            a.dec(memory);

        /*
            MOV does not modify flags, so PRI can be reconstructed without
            losing the ZF produced by INC/DEC.
        */
        if (block.priLive)
            a.mov(edi, memory);

        if (block.branchOpcode == Opcode::JZER)
            a.jz(takenTarget);
        else
            a.jnz(takenTarget);
        a.jmp(fallthroughTarget);
    }

    void EmitLoopCompareBranch(asmjit::x86::Assembler& a, const LoopCompareBranchBlock& block, uint32_t dataBase, const asmjit::Label& takenTarget,
                               const asmjit::Label& fallthroughTarget)
    {
        using namespace asmjit::x86;
        const Mem memory = LoopSlotMem(block.slot, dataBase);
        const bool takenWhenTrue = block.branchOpcode == Opcode::JNZ;
        const cell takenPri = takenWhenTrue ? 1 : 0;
        const cell fallthroughPri = takenWhenTrue ? 0 : 1;

        asmjit::Label takenPath = a.new_label();
        /*
            materialize ALT before CMP: xor reg,reg is our zero fast-path and
            would otherwise clobber the comparison flags.
        */
        if (block.altLive)
            EmitLoopKnownConstant(a, ebx, block.immediate);
        a.cmp(memory, block.immediate);
        EmitComparisonJump(a, block.compareOpcode, takenPath, takenWhenTrue);

        if (block.priLiveFallthrough)
            EmitLoopKnownConstant(a, edi, fallthroughPri);
        a.jmp(fallthroughTarget);
        a.bind(takenPath);
        if (block.priLiveTaken)
            EmitLoopKnownConstant(a, edi, takenPri);
        a.jmp(takenTarget);
    }

    enum class DseSlotKind : uint8_t
    {
        Global,
        Local,
    };

    struct DseSlot
    {
        DseSlotKind kind = DseSlotKind::Global;
        cell offset = 0;
    };

    enum class DseDirectAccessKind : uint8_t
    {
        None,
        Read,
        Overwrite,
        ReadWrite,
    };

    struct DseDirectAccess
    {
        DseDirectAccessKind kind = DseDirectAccessKind::None;
        DseSlot slot;
    };

    bool SameDseSlot(const DseSlot& lhs, const DseSlot& rhs)
    {
        return lhs.kind == rhs.kind && lhs.offset == rhs.offset;
    }

    bool DseSlotsOverlap(const DseSlot& lhs, const DseSlot& rhs)
    {
        if (lhs.kind != rhs.kind)
            return true;

        const int64_t a = static_cast<int64_t>(lhs.offset);
        const int64_t b = static_cast<int64_t>(rhs.offset);
        const int64_t distance = a >= b ? a - b : b - a;
        return distance < static_cast<int64_t>(sizeof(cell));
    }

    bool DecodeDseDirectAccess(const Instruction& instruction, DseDirectAccess& access)
    {
        if (instruction.operands.empty())
            return false;

        access.slot.offset = instruction.operands[0];

        switch (instruction.opcode)
        {
            case Opcode::LOAD_PRI:
            case Opcode::LOAD_ALT:
                access.kind = DseDirectAccessKind::Read;
                access.slot.kind = DseSlotKind::Global;
                return true;

            case Opcode::LOAD_S_PRI:
            case Opcode::LOAD_S_ALT:
                access.kind = DseDirectAccessKind::Read;
                access.slot.kind = DseSlotKind::Local;
                return true;

            case Opcode::STOR_PRI:
            case Opcode::STOR_ALT:
            case Opcode::ZERO:
                access.kind = DseDirectAccessKind::Overwrite;
                access.slot.kind = DseSlotKind::Global;
                return true;

            case Opcode::STOR_S_PRI:
            case Opcode::STOR_S_ALT:
            case Opcode::ZERO_S:
                access.kind = DseDirectAccessKind::Overwrite;
                access.slot.kind = DseSlotKind::Local;
                return true;

            case Opcode::INC:
            case Opcode::DEC:
                access.kind = DseDirectAccessKind::ReadWrite;
                access.slot.kind = DseSlotKind::Global;
                return true;

            case Opcode::INC_S:
            case Opcode::DEC_S:
                access.kind = DseDirectAccessKind::ReadWrite;
                access.slot.kind = DseSlotKind::Local;
                return true;

            default:
                return false;
        }
    }

    bool DecodeDseStartingStore(const Instruction& instruction, DseSlot& slot)
    {
        DseDirectAccess access;
        if (!DecodeDseDirectAccess(instruction, access) || access.kind != DseDirectAccessKind::Overwrite)
        {
            return false;
        }

        slot = access.slot;
        return true;
    }

    bool IsDseTransparentInstruction(const Instruction& instruction, const DseSlot& target)
    {
        DseDirectAccess access;
        if (DecodeDseDirectAccess(instruction, access))
        {
            if (access.slot.kind != target.kind)
                return false;

            if (DseSlotsOverlap(access.slot, target))
                return false;

            return true;
        }

        uint8_t written = RegNone;
        if (IsPureRegisterInstruction(instruction, written))
            return true;

        if (IsDirectCfgTransfer(instruction.opcode))
            return true;

        switch (instruction.opcode)
        {
            case Opcode::NOP:
            case Opcode::LINE:
            case Opcode::SYMBOL:
            case Opcode::SRANGE:
            case Opcode::SYMTAG:
                return true;

            default:
                return false;
        }
    }

    enum class DseProofState : uint8_t
    {
        Unknown,
        Visiting,
        Proven,
        Failed,
    };

    struct RedundantStoreProof
    {
        std::size_t inspectedInstructions = 0;
        std::size_t killingStores = 0;
    };

    bool ProveStoreOverwrittenOnAllPaths(const DecodedProgram& decoded, const ControlFlowGraph& cfg, const DseSlot& target, std::size_t startIndex, std::size_t index,
                                         std::vector<DseProofState>& states, RedundantStoreProof& proof)
    {
        const std::size_t count = decoded.instructions.size();
        if (index >= count || index == startIndex)
            return false;

        DseProofState& state = states[index];
        if (state == DseProofState::Proven)
            return true;
        if (state == DseProofState::Failed || state == DseProofState::Visiting)
            return false;

        if (proof.inspectedInstructions >= static_cast<std::size_t>(kRedundantStoreTraceMaxOps))
        {
            state = DseProofState::Failed;
            return false;
        }

        state = DseProofState::Visiting;
        ++proof.inspectedInstructions;

        const Instruction& instruction = decoded.instructions[index];
        DseDirectAccess access;
        if (DecodeDseDirectAccess(instruction, access) && SameDseSlot(access.slot, target))
        {
            if (access.kind == DseDirectAccessKind::Overwrite)
            {
                ++proof.killingStores;
                state = DseProofState::Proven;
                return true;
            }

            state = DseProofState::Failed;
            return false;
        }

        if (!IsDseTransparentInstruction(instruction, target))
        {
            state = DseProofState::Failed;
            return false;
        }

        const auto& successors = cfg.successors[index];
        if (successors.empty())
        {
            state = DseProofState::Failed;
            return false;
        }

        for (std::size_t successor : successors)
        {
            if (!ProveStoreOverwrittenOnAllPaths(decoded, cfg, target, startIndex, successor, states, proof))
            {
                state = DseProofState::Failed;
                return false;
            }
        }

        state = DseProofState::Proven;
        return true;
    }

    bool IsRedundantDirectStore(const DecodedProgram& decoded, const ControlFlowGraph& cfg, std::size_t startIndex)
    {
        RedundantStoreProof proof;
        const std::size_t count = decoded.instructions.size();
        if (startIndex >= count)
            return false;

        DseSlot target;
        if (!DecodeDseStartingStore(decoded.instructions[startIndex], target))
            return false;

        const auto& successors = cfg.successors[startIndex];
        if (successors.empty())
            return false;

        std::vector<DseProofState> states(count, DseProofState::Unknown);
        states[startIndex] = DseProofState::Visiting;

        for (std::size_t successor : successors)
        {
            if (!ProveStoreOverwrittenOnAllPaths(decoded, cfg, target, startIndex, successor, states, proof))
            {
                return false;
            }
        }

        return proof.killingStores != 0;
    }

    struct ConstantState
    {
        bool priKnown = false;
        bool altKnown = false;
        cell pri = 0;
        cell alt = 0;
    };

    struct ConstantSuperblock
    {
        ConstantState state;
        cell targetCip = 0;
        bool materializePri = true;
        bool materializeAlt = true;
    };

    enum class ConstantIndirectTransferKind : uint8_t
    {
        Call,
        Jump,
        Native,
    };

    struct ConstantIndirectTransferBlock
    {
        ConstantState state;
        ConstantIndirectTransferKind kind = ConstantIndirectTransferKind::Jump;
        std::size_t transferIndex = 0;
        cell targetCip = -1;
    };

    cell WrapAdd(cell lhs, cell rhs)
    {
        return static_cast<cell>(static_cast<ucell>(lhs) + static_cast<ucell>(rhs));
    }

    cell WrapSub(cell lhs, cell rhs)
    {
        return static_cast<cell>(static_cast<ucell>(lhs) - static_cast<ucell>(rhs));
    }

    cell WrapMul(cell lhs, cell rhs)
    {
        const uint64_t product = static_cast<uint64_t>(static_cast<ucell>(lhs)) * static_cast<uint64_t>(static_cast<ucell>(rhs));
        return static_cast<cell>(static_cast<ucell>(product));
    }

    cell WrapNeg(cell value)
    {
        return static_cast<cell>(ucell{0} - static_cast<ucell>(value));
    }

    cell ShiftLeft(cell value, cell count)
    {
        const uint32_t shift = static_cast<uint32_t>(count) & 31u;
        return static_cast<cell>(static_cast<ucell>(value) << shift);
    }

    cell ShiftRightLogical(cell value, cell count)
    {
        const uint32_t shift = static_cast<uint32_t>(count) & 31u;
        return static_cast<cell>(static_cast<ucell>(value) >> shift);
    }

    cell ShiftRightArithmetic(cell value, cell count)
    {
        const uint32_t shift = static_cast<uint32_t>(count) & 31u;

        if (shift == 0)
            return value;

        const uint32_t bits = static_cast<uint32_t>(static_cast<ucell>(value));
        uint32_t result = bits >> shift;

        if ((bits & 0x80000000u) != 0)
            result |= 0xffffffffu << (32u - shift);

        return static_cast<cell>(result);
    }

    bool ApplyConstantOpcode(const Instruction& instruction, ConstantState& state)
    {
        const cell operand = instruction.operands.empty() ? 0 : instruction.operands[0];

        switch (instruction.opcode)
        {
            case Opcode::CONST_PRI:
            case Opcode::ZERO_PRI:
                state.priKnown = true;
                state.pri = instruction.opcode == Opcode::ZERO_PRI ? 0 : operand;
                return true;

            case Opcode::CONST_ALT:
            case Opcode::ZERO_ALT:
                state.altKnown = true;
                state.alt = instruction.opcode == Opcode::ZERO_ALT ? 0 : operand;
                return true;

            case Opcode::MOVE_PRI:
                if (!state.altKnown)
                    return false;
                state.priKnown = true;
                state.pri = state.alt;
                return true;

            case Opcode::MOVE_ALT:
                if (!state.priKnown)
                    return false;
                state.altKnown = true;
                state.alt = state.pri;
                return true;

            case Opcode::XCHG:
                if (!state.priKnown || !state.altKnown)
                    return false;
                std::swap(state.pri, state.alt);
                return true;

            case Opcode::SHL:
            case Opcode::SHR:
            case Opcode::SSHR:
                if (!state.priKnown || !state.altKnown)
                    return false;
                if (instruction.opcode == Opcode::SHL)
                    state.pri = ShiftLeft(state.pri, state.alt);
                else if (instruction.opcode == Opcode::SHR)
                    state.pri = ShiftRightLogical(state.pri, state.alt);
                else
                    state.pri = ShiftRightArithmetic(state.pri, state.alt);
                return true;

            case Opcode::SHL_C_PRI:
            case Opcode::SHR_C_PRI:
                if (!state.priKnown)
                    return false;
                state.pri = instruction.opcode == Opcode::SHL_C_PRI ? ShiftLeft(state.pri, operand) : ShiftRightLogical(state.pri, operand);
                return true;

            case Opcode::SHL_C_ALT:
            case Opcode::SHR_C_ALT:
                if (!state.altKnown)
                    return false;
                state.alt = instruction.opcode == Opcode::SHL_C_ALT ? ShiftLeft(state.alt, operand) : ShiftRightLogical(state.alt, operand);
                return true;

            case Opcode::SMUL:
            case Opcode::UMUL:
                if (!state.priKnown || !state.altKnown)
                    return false;
                state.pri = WrapMul(state.pri, state.alt);
                return true;

            case Opcode::ADD:
                if (!state.priKnown || !state.altKnown)
                    return false;
                state.pri = WrapAdd(state.pri, state.alt);
                return true;

            case Opcode::SUB:
                if (!state.priKnown || !state.altKnown)
                    return false;
                state.pri = WrapSub(state.pri, state.alt);
                return true;

            case Opcode::SUB_ALT:
                if (!state.priKnown || !state.altKnown)
                    return false;
                state.pri = WrapSub(state.alt, state.pri);
                return true;

            case Opcode::AND:
            case Opcode::OR:
            case Opcode::XOR:
                if (!state.priKnown || !state.altKnown)
                    return false;
                if (instruction.opcode == Opcode::AND)
                    state.pri = static_cast<cell>(static_cast<ucell>(state.pri) & static_cast<ucell>(state.alt));
                else if (instruction.opcode == Opcode::OR)
                    state.pri = static_cast<cell>(static_cast<ucell>(state.pri) | static_cast<ucell>(state.alt));
                else
                    state.pri = static_cast<cell>(static_cast<ucell>(state.pri) ^ static_cast<ucell>(state.alt));
                return true;

            case Opcode::NOT:
                if (!state.priKnown)
                    return false;
                state.pri = state.pri == 0 ? 1 : 0;
                return true;

            case Opcode::NEG:
                if (!state.priKnown)
                    return false;
                state.pri = WrapNeg(state.pri);
                return true;

            case Opcode::INVERT:
                if (!state.priKnown)
                    return false;
                state.pri = static_cast<cell>(~static_cast<ucell>(state.pri));
                return true;

            case Opcode::ADD_C:
                if (!state.priKnown)
                    return false;
                state.pri = WrapAdd(state.pri, operand);
                return true;

            case Opcode::SMUL_C:
                if (!state.priKnown)
                    return false;
                state.pri = WrapMul(state.pri, operand);
                return true;

            case Opcode::SIGN_PRI:
                if (!state.priKnown)
                    return false;
                state.pri = static_cast<cell>(static_cast<int8_t>(static_cast<ucell>(state.pri) & 0xffu));
                return true;

            case Opcode::SIGN_ALT:
                if (!state.altKnown)
                    return false;
                state.alt = static_cast<cell>(static_cast<int8_t>(static_cast<ucell>(state.alt) & 0xffu));
                return true;

            case Opcode::EQ:
            case Opcode::NEQ:
            case Opcode::LESS:
            case Opcode::LEQ:
            case Opcode::GRTR:
            case Opcode::GEQ:
            case Opcode::SLESS:
            case Opcode::SLEQ:
            case Opcode::SGRTR:
            case Opcode::SGEQ:
                if (!state.priKnown || !state.altKnown)
                    return false;
                switch (instruction.opcode)
                {
                    case Opcode::EQ:
                        state.pri = state.pri == state.alt;
                        break;
                    case Opcode::NEQ:
                        state.pri = state.pri != state.alt;
                        break;
                    case Opcode::LESS:
                        state.pri = static_cast<ucell>(state.pri) < static_cast<ucell>(state.alt);
                        break;
                    case Opcode::LEQ:
                        state.pri = static_cast<ucell>(state.pri) <= static_cast<ucell>(state.alt);
                        break;
                    case Opcode::GRTR:
                        state.pri = static_cast<ucell>(state.pri) > static_cast<ucell>(state.alt);
                        break;
                    case Opcode::GEQ:
                        state.pri = static_cast<ucell>(state.pri) >= static_cast<ucell>(state.alt);
                        break;
                    case Opcode::SLESS:
                        state.pri = state.pri < state.alt;
                        break;
                    case Opcode::SLEQ:
                        state.pri = state.pri <= state.alt;
                        break;
                    case Opcode::SGRTR:
                        state.pri = state.pri > state.alt;
                        break;
                    case Opcode::SGEQ:
                        state.pri = state.pri >= state.alt;
                        break;
                    default:
                        break;
                }
                return true;

            case Opcode::EQ_C_PRI:
                if (!state.priKnown)
                    return false;
                state.pri = state.pri == operand;
                return true;

            case Opcode::EQ_C_ALT:
                if (!state.altKnown)
                    return false;
                state.priKnown = true;
                state.pri = state.alt == operand;
                return true;

            case Opcode::INC_PRI:
                if (!state.priKnown)
                    return false;
                state.pri = WrapAdd(state.pri, 1);
                return true;

            case Opcode::INC_ALT:
                if (!state.altKnown)
                    return false;
                state.alt = WrapAdd(state.alt, 1);
                return true;

            case Opcode::DEC_PRI:
                if (!state.priKnown)
                    return false;
                state.pri = WrapSub(state.pri, 1);
                return true;

            case Opcode::DEC_ALT:
                if (!state.altKnown)
                    return false;
                state.alt = WrapSub(state.alt, 1);
                return true;

            default:
                return false;
        }
    }

    bool ResolveConstantBranch(AMX* amx, const DecodedProgram& decoded, const Instruction& instruction, const ConstantState& state, cell& targetCip)
    {
        switch (instruction.opcode)
        {
            case Opcode::JUMP:
            case Opcode::JREL:
                targetCip = ResolveBranchTarget(amx, instruction);
                return true;

            case Opcode::JZER:
            case Opcode::JNZ:
                if (!state.priKnown)
                    return false;
                if ((instruction.opcode == Opcode::JZER && state.pri == 0) || (instruction.opcode == Opcode::JNZ && state.pri != 0))
                {
                    targetCip = ResolveBranchTarget(amx, instruction);
                }
                else
                {
                    targetCip = instruction.Next();
                }
                return true;

            case Opcode::JEQ:
            case Opcode::JNEQ:
            case Opcode::JLESS:
            case Opcode::JLEQ:
            case Opcode::JGRTR:
            case Opcode::JGEQ:
            case Opcode::JSLESS:
            case Opcode::JSLEQ:
            case Opcode::JSGRTR:
            case Opcode::JSGEQ:
                if (!state.priKnown || !state.altKnown)
                    return false;
                {
                    bool take = false;
                    switch (instruction.opcode)
                    {
                        case Opcode::JEQ:
                            take = state.pri == state.alt;
                            break;
                        case Opcode::JNEQ:
                            take = state.pri != state.alt;
                            break;
                        case Opcode::JLESS:
                            take = static_cast<ucell>(state.pri) < static_cast<ucell>(state.alt);
                            break;
                        case Opcode::JLEQ:
                            take = static_cast<ucell>(state.pri) <= static_cast<ucell>(state.alt);
                            break;
                        case Opcode::JGRTR:
                            take = static_cast<ucell>(state.pri) > static_cast<ucell>(state.alt);
                            break;
                        case Opcode::JGEQ:
                            take = static_cast<ucell>(state.pri) >= static_cast<ucell>(state.alt);
                            break;
                        case Opcode::JSLESS:
                            take = state.pri < state.alt;
                            break;
                        case Opcode::JSLEQ:
                            take = state.pri <= state.alt;
                            break;
                        case Opcode::JSGRTR:
                            take = state.pri > state.alt;
                            break;
                        case Opcode::JSGEQ:
                            take = state.pri >= state.alt;
                            break;
                        default:
                            break;
                    }
                    targetCip = take ? ResolveBranchTarget(amx, instruction) : instruction.Next();
                    return true;
                }

            case Opcode::SWITCH:
                if (!state.priKnown || instruction.operands.empty())
                    return false;
                {
                    const cell tableCip = NormalizeCodeTarget(amx, instruction.operands[0]);
                    const Instruction* table = FindInstruction(decoded, tableCip);

                    if (table == nullptr || table->opcode != Opcode::CASETBL || table->operands.size() < 2)
                        return false;

                    const cell caseCount = table->operands[0];
                    targetCip = NormalizeCodeTarget(amx, table->operands[1]);

                    for (cell i = 0; i < caseCount; ++i)
                    {
                        const std::size_t valueIndex = 2 + static_cast<std::size_t>(i) * 2;
                        const std::size_t targetIndex = valueIndex + 1;

                        if (targetIndex >= table->operands.size())
                            return false;

                        if (table->operands[valueIndex] == state.pri)
                        {
                            targetCip = NormalizeCodeTarget(amx, table->operands[targetIndex]);
                            break;
                        }
                    }

                    return targetCip >= 0;
                }

            default:
                return false;
        }
    }

    bool BuildConstantIndirectTransferBlock(AMX* amx, const DecodedProgram& decoded, std::size_t startIndex, ConstantIndirectTransferBlock& block)
    {
        if (amx == nullptr || amx->base == nullptr || startIndex >= decoded.instructions.size())
            return false;

        const Opcode root = decoded.instructions[startIndex].opcode;
        if (root != Opcode::CONST_PRI && root != Opcode::CONST_ALT && root != Opcode::ZERO_PRI && root != Opcode::ZERO_ALT)
        {
            return false;
        }

        ConstantState state;
        const std::size_t maxTraceOps = static_cast<std::size_t>(kConstTraceMaxOps);
        std::size_t index = startIndex;
        std::size_t folded = 0;

        while (index < decoded.instructions.size() && folded < maxTraceOps)
        {
            const Instruction& current = decoded.instructions[index];
            if (index != startIndex && decoded.instructions[index - 1].Next() != current.cip)
            {
                return false;
            }

            if (ApplyConstantOpcode(current, state))
            {
                ++folded;
                ++index;
                continue;
            }

            if (!state.priKnown || (current.opcode != Opcode::CALL_PRI && current.opcode != Opcode::JUMP_PRI && current.opcode != Opcode::SYSREQ_PRI))
            {
                return false;
            }

            block.state = state;
            block.transferIndex = index;

            if (current.opcode == Opcode::SYSREQ_PRI)
            {
                block.kind = ConstantIndirectTransferKind::Native;
                block.targetCip = -1;
                return folded != 0;
            }

            const cell target = state.pri;

            if (target < 0 || (target % static_cast<cell>(sizeof(cell))) != 0 || FindInstruction(decoded, target) == nullptr)
            {
                return false;
            }

            const ThreadedDirectTarget threaded = ResolveThreadedDirectTarget(amx, decoded, target);
            if (threaded.target < 0 || FindInstruction(decoded, threaded.target) == nullptr)
                return false;

            block.kind = current.opcode == Opcode::CALL_PRI ? ConstantIndirectTransferKind::Call : ConstantIndirectTransferKind::Jump;
            block.targetCip = threaded.target;
            return folded != 0;
        }

        return false;
    }

    void AugmentCallGraphWithConstantIndirectCalls(AMX* amx, const DecodedProgram& decoded, CallGraphSummary& graph)
    {
        for (std::size_t startIndex = 0; startIndex < decoded.instructions.size(); ++startIndex)
        {
            ConstantIndirectTransferBlock block;
            if (!BuildConstantIndirectTransferBlock(amx, decoded, startIndex, block) || block.kind != ConstantIndirectTransferKind::Call ||
                block.transferIndex >= decoded.instructions.size())
            {
                continue;
            }

            graph.devirtualizedCallTargetByTransferInstruction[block.transferIndex] = block.targetCip;

            const cell functionEntry = ResolveCallGraphFunctionEntry(decoded, graph, block.targetCip);
            if (functionEntry < 0)
                continue;

            const cell returnCip = decoded.instructions[block.transferIndex].Next();
            AddUniqueReturnCip(graph.directReturnCipsByFunction[functionEntry], returnCip);
        }
    }

    bool HasCfgEdge(const ControlFlowGraph& cfg, std::size_t from, std::size_t to)
    {
        if (from >= cfg.successors.size())
            return false;

        for (std::size_t successor : cfg.successors[from])
        {
            if (successor == to)
                return true;
        }

        return false;
    }

    bool BuildConstantSuperblock(AMX* amx, const DecodedProgram& decoded, const ControlFlowGraph& cfg, const LocalLiveness& liveness, std::size_t startIndex,
                                 ConstantSuperblock& block)
    {
        const std::size_t count = decoded.instructions.size();
        if (startIndex >= count)
            return false;

        const Opcode root = decoded.instructions[startIndex].opcode;
        if (root != Opcode::CONST_PRI && root != Opcode::CONST_ALT && root != Opcode::ZERO_PRI && root != Opcode::ZERO_ALT)
        {
            return false;
        }

        ConstantState state;
        std::size_t index = startIndex;
        std::size_t pureCount = 0;
        std::size_t foldedTransfers = 0;
        std::size_t conditionalTransfers = 0;
        std::vector<uint8_t> visited(count, 0);
        const std::size_t maxTraceOps = static_cast<std::size_t>(kConstTraceMaxOps);

        while (index < count && pureCount + foldedTransfers < maxTraceOps)
        {
            if (visited[index] != 0)
                break;

            const Instruction& current = decoded.instructions[index];

            if (ApplyConstantOpcode(current, state))
            {
                visited[index] = 1;
                ++pureCount;
                ++index;
                continue;
            }

            cell branchTarget = 0;
            if (!ResolveConstantBranch(amx, decoded, current, state, branchTarget))
                break;

            auto targetIt = decoded.byCip.find(branchTarget);
            if (targetIt == decoded.byCip.end())
                break;

            const std::size_t targetIndex = targetIt->second;
            if (!HasCfgEdge(cfg, index, targetIndex))
                break;

            visited[index] = 1;
            ++foldedTransfers;
            if (IsConditionalDirectBranch(current.opcode) || current.opcode == Opcode::SWITCH)
                ++conditionalTransfers;

            index = targetIndex;
            continue;
        }

        if (index >= count)
            return false;

        const std::size_t totalCount = pureCount + foldedTransfers;
        if (totalCount == 0)
            return false;

        const cell targetCip = decoded.instructions[index].cip;
        const bool materializePri = state.priKnown && liveness.IsLiveIn(index, RegPri);
        const bool materializeAlt = state.altKnown && liveness.IsLiveIn(index, RegAlt);
        const std::size_t materializedRegisters = (materializePri ? 1u : 0u) + (materializeAlt ? 1u : 0u);

        /*
            conditional branches are normally at least CMP/TEST + Jcc. If the
            condition is known, MOV/XOR + JMP is already a win at two folded
            ops.
        */
        const bool branchWin = conditionalTransfers != 0 && totalCount >= 2;

        /*
            the fast trace bypasses all preserved canonical instruction bodies
            with one JMP. Require a real instruction-count win for arithmetic-
            only traces. Direct CFG transfers count as eliminated runtime
            instructions.
        */
        const bool arithmeticWin = pureCount >= static_cast<std::size_t>(kConstPropMinOps) && totalCount > materializedRegisters + 1u;

        if (!branchWin && !arithmeticWin)
            return false;

        block.state = state;
        block.targetCip = targetCip;
        block.materializePri = materializePri;
        block.materializeAlt = materializeAlt;
        return true;
    }

    void EmitKnownConstant(asmjit::x86::Assembler& a, const asmjit::x86::Gp& reg, cell value)
    {
        if (value == 0)
            a.xor_(reg, reg);
        else
            a.mov(reg, value);
    }

    enum class SymbolicOrigin : uint8_t
    {
        InitialPri,
        InitialAlt,
        Constant,
        Derived,
    };

    struct SymbolicValue
    {
        SymbolicOrigin origin = SymbolicOrigin::Derived;
        cell constant = 0;
        uint32_t id = 0;
    };

    struct SymbolicState
    {
        SymbolicValue pri{SymbolicOrigin::InitialPri, 0, 1};
        SymbolicValue alt{SymbolicOrigin::InitialAlt, 0, 2};
        uint32_t nextId = 3;
    };

    struct ValueSuperblock
    {
        SymbolicState state;
        cell targetCip = 0;
        bool materializePri = true;
        bool materializeAlt = true;
    };

    bool SymbolicEqual(const SymbolicValue& lhs, const SymbolicValue& rhs)
    {
        if (lhs.origin == SymbolicOrigin::Constant && rhs.origin == SymbolicOrigin::Constant)
            return lhs.constant == rhs.constant;
        return lhs.id != 0 && lhs.id == rhs.id;
    }

    bool IsSymbolicConstant(const SymbolicValue& value, cell expected)
    {
        return value.origin == SymbolicOrigin::Constant && value.constant == expected;
    }

    SymbolicValue MakeSymbolicConstant(cell value)
    {
        return {SymbolicOrigin::Constant, value, 0};
    }

    SymbolicValue MakeDerived(SymbolicState& state)
    {
        return {SymbolicOrigin::Derived, 0, state.nextId++};
    }

    bool IsMaterializableSymbolic(const SymbolicValue& value)
    {
        return value.origin != SymbolicOrigin::Derived;
    }

    bool EvaluateSymbolicComparison(Opcode opcode, const SymbolicValue& pri, const SymbolicValue& alt, cell& result)
    {
        if (pri.origin == SymbolicOrigin::Constant && alt.origin == SymbolicOrigin::Constant)
        {
            const cell lhs = pri.constant;
            const cell rhs = alt.constant;
            bool value = false;
            switch (opcode)
            {
                case Opcode::EQ:
                case Opcode::JEQ:
                    value = lhs == rhs;
                    break;
                case Opcode::NEQ:
                case Opcode::JNEQ:
                    value = lhs != rhs;
                    break;
                case Opcode::LESS:
                case Opcode::JLESS:
                    value = static_cast<ucell>(lhs) < static_cast<ucell>(rhs);
                    break;
                case Opcode::LEQ:
                case Opcode::JLEQ:
                    value = static_cast<ucell>(lhs) <= static_cast<ucell>(rhs);
                    break;
                case Opcode::GRTR:
                case Opcode::JGRTR:
                    value = static_cast<ucell>(lhs) > static_cast<ucell>(rhs);
                    break;
                case Opcode::GEQ:
                case Opcode::JGEQ:
                    value = static_cast<ucell>(lhs) >= static_cast<ucell>(rhs);
                    break;
                case Opcode::SLESS:
                case Opcode::JSLESS:
                    value = lhs < rhs;
                    break;
                case Opcode::SLEQ:
                case Opcode::JSLEQ:
                    value = lhs <= rhs;
                    break;
                case Opcode::SGRTR:
                case Opcode::JSGRTR:
                    value = lhs > rhs;
                    break;
                case Opcode::SGEQ:
                case Opcode::JSGEQ:
                    value = lhs >= rhs;
                    break;
                default:
                    return false;
            }
            result = value ? 1 : 0;
            return true;
        }

        if (!SymbolicEqual(pri, alt))
            return false;

        switch (opcode)
        {
            case Opcode::EQ:
            case Opcode::JEQ:
            case Opcode::LEQ:
            case Opcode::JLEQ:
            case Opcode::GEQ:
            case Opcode::JGEQ:
            case Opcode::SLEQ:
            case Opcode::JSLEQ:
            case Opcode::SGEQ:
            case Opcode::JSGEQ:
                result = 1;
                return true;

            case Opcode::NEQ:
            case Opcode::JNEQ:
            case Opcode::LESS:
            case Opcode::JLESS:
            case Opcode::GRTR:
            case Opcode::JGRTR:
            case Opcode::SLESS:
            case Opcode::JSLESS:
            case Opcode::SGRTR:
            case Opcode::JSGRTR:
                result = 0;
                return true;

            default:
                return false;
        }
    }

    bool ApplySymbolicOpcode(const Instruction& instruction, SymbolicState& state, bool& equalityFold)
    {
        equalityFold = false;
        const cell operand = instruction.operands.empty() ? 0 : instruction.operands[0];
        auto derivePri = [&]() {
            state.pri = MakeDerived(state);
        };
        auto deriveAlt = [&]() {
            state.alt = MakeDerived(state);
        };

        switch (instruction.opcode)
        {
            case Opcode::CONST_PRI:
                state.pri = MakeSymbolicConstant(operand);
                return true;
            case Opcode::ZERO_PRI:
                state.pri = MakeSymbolicConstant(0);
                return true;
            case Opcode::CONST_ALT:
                state.alt = MakeSymbolicConstant(operand);
                return true;
            case Opcode::ZERO_ALT:
                state.alt = MakeSymbolicConstant(0);
                return true;

            case Opcode::MOVE_PRI:
                state.pri = state.alt;
                equalityFold = true;
                return true;
            case Opcode::MOVE_ALT:
                state.alt = state.pri;
                equalityFold = true;
                return true;
            case Opcode::XCHG:
                std::swap(state.pri, state.alt);
                return true;

            case Opcode::SHL_C_PRI:
            case Opcode::SHR_C_PRI:
                if ((static_cast<uint32_t>(operand) & 31u) == 0)
                {
                    equalityFold = true;
                    return true;
                }
                if (state.pri.origin == SymbolicOrigin::Constant)
                {
                    state.pri =
                        MakeSymbolicConstant(instruction.opcode == Opcode::SHL_C_PRI ? ShiftLeft(state.pri.constant, operand) : ShiftRightLogical(state.pri.constant, operand));
                }
                else
                {
                    derivePri();
                }
                return true;

            case Opcode::SHL_C_ALT:
            case Opcode::SHR_C_ALT:
                if ((static_cast<uint32_t>(operand) & 31u) == 0)
                {
                    equalityFold = true;
                    return true;
                }
                if (state.alt.origin == SymbolicOrigin::Constant)
                {
                    state.alt =
                        MakeSymbolicConstant(instruction.opcode == Opcode::SHL_C_ALT ? ShiftLeft(state.alt.constant, operand) : ShiftRightLogical(state.alt.constant, operand));
                }
                else
                {
                    deriveAlt();
                }
                return true;

            case Opcode::SHL:
            case Opcode::SHR:
            case Opcode::SSHR:
                if (state.alt.origin == SymbolicOrigin::Constant && (static_cast<uint32_t>(state.alt.constant) & 31u) == 0)
                {
                    equalityFold = true;
                    return true;
                }
                if (state.pri.origin == SymbolicOrigin::Constant && state.alt.origin == SymbolicOrigin::Constant)
                {
                    cell value = 0;
                    if (instruction.opcode == Opcode::SHL)
                        value = ShiftLeft(state.pri.constant, state.alt.constant);
                    else if (instruction.opcode == Opcode::SHR)
                        value = ShiftRightLogical(state.pri.constant, state.alt.constant);
                    else
                        value = ShiftRightArithmetic(state.pri.constant, state.alt.constant);
                    state.pri = MakeSymbolicConstant(value);
                }
                else
                {
                    derivePri();
                }
                return true;

            case Opcode::ADD:
                if (state.pri.origin == SymbolicOrigin::Constant && state.alt.origin == SymbolicOrigin::Constant)
                    state.pri = MakeSymbolicConstant(WrapAdd(state.pri.constant, state.alt.constant));
                else if (IsSymbolicConstant(state.alt, 0))
                    equalityFold = true;
                else if (IsSymbolicConstant(state.pri, 0))
                {
                    state.pri = state.alt;
                    equalityFold = true;
                }
                else
                    derivePri();
                return true;

            case Opcode::SUB:
                if (state.pri.origin == SymbolicOrigin::Constant && state.alt.origin == SymbolicOrigin::Constant)
                    state.pri = MakeSymbolicConstant(WrapSub(state.pri.constant, state.alt.constant));
                else if (SymbolicEqual(state.pri, state.alt))
                {
                    state.pri = MakeSymbolicConstant(0);
                    equalityFold = true;
                }
                else if (IsSymbolicConstant(state.alt, 0))
                    equalityFold = true;
                else
                    derivePri();
                return true;

            case Opcode::SUB_ALT:
                if (state.pri.origin == SymbolicOrigin::Constant && state.alt.origin == SymbolicOrigin::Constant)
                    state.pri = MakeSymbolicConstant(WrapSub(state.alt.constant, state.pri.constant));
                else if (SymbolicEqual(state.pri, state.alt))
                {
                    state.pri = MakeSymbolicConstant(0);
                    equalityFold = true;
                }
                else if (IsSymbolicConstant(state.pri, 0))
                {
                    state.pri = state.alt;
                    equalityFold = true;
                }
                else
                    derivePri();
                return true;

            case Opcode::AND:
                if (state.pri.origin == SymbolicOrigin::Constant && state.alt.origin == SymbolicOrigin::Constant)
                    state.pri = MakeSymbolicConstant(static_cast<cell>(static_cast<ucell>(state.pri.constant) & static_cast<ucell>(state.alt.constant)));
                else if (SymbolicEqual(state.pri, state.alt))
                    equalityFold = true;
                else if (IsSymbolicConstant(state.pri, 0) || IsSymbolicConstant(state.alt, 0))
                {
                    state.pri = MakeSymbolicConstant(0);
                    equalityFold = true;
                }
                else if (IsSymbolicConstant(state.pri, -1))
                {
                    state.pri = state.alt;
                    equalityFold = true;
                }
                else if (IsSymbolicConstant(state.alt, -1))
                    equalityFold = true;
                else
                    derivePri();
                return true;

            case Opcode::OR:
                if (state.pri.origin == SymbolicOrigin::Constant && state.alt.origin == SymbolicOrigin::Constant)
                    state.pri = MakeSymbolicConstant(static_cast<cell>(static_cast<ucell>(state.pri.constant) | static_cast<ucell>(state.alt.constant)));
                else if (SymbolicEqual(state.pri, state.alt))
                    equalityFold = true;
                else if (IsSymbolicConstant(state.pri, 0))
                {
                    state.pri = state.alt;
                    equalityFold = true;
                }
                else if (IsSymbolicConstant(state.alt, 0))
                    equalityFold = true;
                else if (IsSymbolicConstant(state.pri, -1) || IsSymbolicConstant(state.alt, -1))
                {
                    state.pri = MakeSymbolicConstant(-1);
                    equalityFold = true;
                }
                else
                    derivePri();
                return true;

            case Opcode::XOR:
                if (state.pri.origin == SymbolicOrigin::Constant && state.alt.origin == SymbolicOrigin::Constant)
                    state.pri = MakeSymbolicConstant(static_cast<cell>(static_cast<ucell>(state.pri.constant) ^ static_cast<ucell>(state.alt.constant)));
                else if (SymbolicEqual(state.pri, state.alt))
                {
                    state.pri = MakeSymbolicConstant(0);
                    equalityFold = true;
                }
                else if (IsSymbolicConstant(state.pri, 0))
                {
                    state.pri = state.alt;
                    equalityFold = true;
                }
                else if (IsSymbolicConstant(state.alt, 0))
                    equalityFold = true;
                else
                    derivePri();
                return true;

            case Opcode::SMUL:
            case Opcode::UMUL:
                if (state.pri.origin == SymbolicOrigin::Constant && state.alt.origin == SymbolicOrigin::Constant)
                    state.pri = MakeSymbolicConstant(WrapMul(state.pri.constant, state.alt.constant));
                else if (IsSymbolicConstant(state.pri, 0) || IsSymbolicConstant(state.alt, 0))
                {
                    state.pri = MakeSymbolicConstant(0);
                    equalityFold = true;
                }
                else if (IsSymbolicConstant(state.pri, 1))
                {
                    state.pri = state.alt;
                    equalityFold = true;
                }
                else if (IsSymbolicConstant(state.alt, 1))
                    equalityFold = true;
                else
                    derivePri();
                return true;

            case Opcode::NOT:
                if (state.pri.origin == SymbolicOrigin::Constant)
                    state.pri = MakeSymbolicConstant(state.pri.constant == 0 ? 1 : 0);
                else
                    derivePri();
                return true;
            case Opcode::NEG:
                if (state.pri.origin == SymbolicOrigin::Constant)
                    state.pri = MakeSymbolicConstant(WrapNeg(state.pri.constant));
                else
                    derivePri();
                return true;
            case Opcode::INVERT:
                if (state.pri.origin == SymbolicOrigin::Constant)
                    state.pri = MakeSymbolicConstant(static_cast<cell>(~static_cast<ucell>(state.pri.constant)));
                else
                    derivePri();
                return true;

            case Opcode::ADD_C:
                if (operand == 0)
                {
                    equalityFold = true;
                    return true;
                }
                if (state.pri.origin == SymbolicOrigin::Constant)
                    state.pri = MakeSymbolicConstant(WrapAdd(state.pri.constant, operand));
                else
                    derivePri();
                return true;

            case Opcode::SMUL_C:
                if (operand == 0)
                {
                    state.pri = MakeSymbolicConstant(0);
                    equalityFold = true;
                }
                else if (operand == 1)
                {
                    equalityFold = true;
                }
                else if (state.pri.origin == SymbolicOrigin::Constant)
                    state.pri = MakeSymbolicConstant(WrapMul(state.pri.constant, operand));
                else
                    derivePri();
                return true;

            case Opcode::SIGN_PRI:
                if (state.pri.origin == SymbolicOrigin::Constant)
                    state.pri = MakeSymbolicConstant(static_cast<cell>(static_cast<int8_t>(static_cast<ucell>(state.pri.constant) & 0xffu)));
                else
                    derivePri();
                return true;
            case Opcode::SIGN_ALT:
                if (state.alt.origin == SymbolicOrigin::Constant)
                    state.alt = MakeSymbolicConstant(static_cast<cell>(static_cast<int8_t>(static_cast<ucell>(state.alt.constant) & 0xffu)));
                else
                    deriveAlt();
                return true;

            case Opcode::EQ:
            case Opcode::NEQ:
            case Opcode::LESS:
            case Opcode::LEQ:
            case Opcode::GRTR:
            case Opcode::GEQ:
            case Opcode::SLESS:
            case Opcode::SLEQ:
            case Opcode::SGRTR:
            case Opcode::SGEQ:
            {
                cell result = 0;
                if (EvaluateSymbolicComparison(instruction.opcode, state.pri, state.alt, result))
                {
                    state.pri = MakeSymbolicConstant(result);
                    equalityFold = true;
                }
                else
                {
                    derivePri();
                }
                return true;
            }

            case Opcode::EQ_C_PRI:
                if (state.pri.origin == SymbolicOrigin::Constant)
                    state.pri = MakeSymbolicConstant(state.pri.constant == operand ? 1 : 0);
                else
                    derivePri();
                return true;
            case Opcode::EQ_C_ALT:
                if (state.alt.origin == SymbolicOrigin::Constant)
                    state.pri = MakeSymbolicConstant(state.alt.constant == operand ? 1 : 0);
                else
                    derivePri();
                return true;

            case Opcode::INC_PRI:
                if (state.pri.origin == SymbolicOrigin::Constant)
                    state.pri = MakeSymbolicConstant(WrapAdd(state.pri.constant, 1));
                else
                    derivePri();
                return true;
            case Opcode::DEC_PRI:
                if (state.pri.origin == SymbolicOrigin::Constant)
                    state.pri = MakeSymbolicConstant(WrapSub(state.pri.constant, 1));
                else
                    derivePri();
                return true;
            case Opcode::INC_ALT:
                if (state.alt.origin == SymbolicOrigin::Constant)
                    state.alt = MakeSymbolicConstant(WrapAdd(state.alt.constant, 1));
                else
                    deriveAlt();
                return true;
            case Opcode::DEC_ALT:
                if (state.alt.origin == SymbolicOrigin::Constant)
                    state.alt = MakeSymbolicConstant(WrapSub(state.alt.constant, 1));
                else
                    deriveAlt();
                return true;

            default:
                return false;
        }
    }

    bool ResolveSymbolicBranch(AMX* amx, const DecodedProgram& decoded, const Instruction& instruction, const SymbolicState& state, cell& targetCip, bool& equalityFold)
    {
        equalityFold = false;

        switch (instruction.opcode)
        {
            case Opcode::JUMP:
            case Opcode::JREL:
                targetCip = ResolveBranchTarget(amx, instruction);
                return true;

            case Opcode::JZER:
            case Opcode::JNZ:
                if (state.pri.origin != SymbolicOrigin::Constant)
                    return false;
                targetCip = ((instruction.opcode == Opcode::JZER && state.pri.constant == 0) || (instruction.opcode == Opcode::JNZ && state.pri.constant != 0))
                                ? ResolveBranchTarget(amx, instruction)
                                : instruction.Next();
                return true;

            case Opcode::JEQ:
            case Opcode::JNEQ:
            case Opcode::JLESS:
            case Opcode::JLEQ:
            case Opcode::JGRTR:
            case Opcode::JGEQ:
            case Opcode::JSLESS:
            case Opcode::JSLEQ:
            case Opcode::JSGRTR:
            case Opcode::JSGEQ:
            {
                cell result = 0;
                if (!EvaluateSymbolicComparison(instruction.opcode, state.pri, state.alt, result))
                    return false;
                equalityFold = !(state.pri.origin == SymbolicOrigin::Constant && state.alt.origin == SymbolicOrigin::Constant);
                targetCip = result != 0 ? ResolveBranchTarget(amx, instruction) : instruction.Next();
                return true;
            }

            case Opcode::SWITCH:
                if (state.pri.origin != SymbolicOrigin::Constant || instruction.operands.empty())
                    return false;
                {
                    ConstantState constant;
                    constant.priKnown = true;
                    constant.pri = state.pri.constant;
                    return ResolveConstantBranch(amx, decoded, instruction, constant, targetCip);
                }

            default:
                return false;
        }
    }

    std::size_t EstimateSymbolicMaterializationCost(const SymbolicState& state, bool needPri, bool needAlt)
    {
        if (!needPri && !needAlt)
            return 0;
        if (needPri && !IsMaterializableSymbolic(state.pri))
            return std::numeric_limits<std::size_t>::max();
        if (needAlt && !IsMaterializableSymbolic(state.alt))
            return std::numeric_limits<std::size_t>::max();

        if (needPri && !needAlt)
            return state.pri.origin == SymbolicOrigin::InitialPri ? 0 : 1;
        if (!needPri && needAlt)
            return state.alt.origin == SymbolicOrigin::InitialAlt ? 0 : 1;

        if (state.pri.origin == SymbolicOrigin::InitialAlt && state.alt.origin == SymbolicOrigin::InitialPri)
            return 1;

        std::size_t cost = 0;
        if (state.pri.origin != SymbolicOrigin::InitialPri)
            ++cost;
        if (state.alt.origin != SymbolicOrigin::InitialAlt)
            ++cost;
        return cost;
    }

    bool BuildValueSuperblock(AMX* amx, const DecodedProgram& decoded, const ControlFlowGraph& cfg, const LocalLiveness& liveness, std::size_t startIndex, ValueSuperblock& block)
    {
        const std::size_t count = decoded.instructions.size();
        if (startIndex >= count)
            return false;

        const Opcode root = decoded.instructions[startIndex].opcode;
        if (root != Opcode::MOVE_PRI && root != Opcode::MOVE_ALT)
            return false;

        SymbolicState state;
        std::size_t index = startIndex;
        std::size_t folded = 0;
        std::size_t equalityFolds = 0;
        std::size_t conditionalTransfers = 0;
        std::vector<uint8_t> visited(count, 0);
        const std::size_t maxTraceOps = static_cast<std::size_t>(kValueTraceMaxOps);

        while (index < count && folded < maxTraceOps)
        {
            if (visited[index] != 0)
                break;

            const Instruction& current = decoded.instructions[index];
            bool equalityFold = false;

            if (ApplySymbolicOpcode(current, state, equalityFold))
            {
                visited[index] = 1;
                ++folded;
                if (equalityFold)
                    ++equalityFolds;
                ++index;
                continue;
            }

            cell branchTarget = 0;
            if (!ResolveSymbolicBranch(amx, decoded, current, state, branchTarget, equalityFold))
                break;

            auto targetIt = decoded.byCip.find(branchTarget);
            if (targetIt == decoded.byCip.end())
                break;

            const std::size_t targetIndex = targetIt->second;
            if (!HasCfgEdge(cfg, index, targetIndex))
                break;

            visited[index] = 1;
            ++folded;
            if (equalityFold)
                ++equalityFolds;
            if (IsConditionalDirectBranch(current.opcode) || current.opcode == Opcode::SWITCH)
                ++conditionalTransfers;

            index = targetIndex;
            continue;
        }

        if (index >= count || folded == 0)
            return false;

        const bool materializePri = liveness.IsLiveIn(index, RegPri);
        const bool materializeAlt = liveness.IsLiveIn(index, RegAlt);
        const std::size_t materializeCost = EstimateSymbolicMaterializationCost(state, materializePri, materializeAlt);

        if (materializeCost == std::numeric_limits<std::size_t>::max())
            return false;

        const bool branchWin = conditionalTransfers != 0 && equalityFolds != 0 && folded >= 2;
        const bool arithmeticWin = equalityFolds != 0 && folded >= static_cast<std::size_t>(kValuePropMinOps) && folded > materializeCost + 1u;

        if (!branchWin && !arithmeticWin)
            return false;

        block.state = state;
        block.targetCip = decoded.instructions[index].cip;
        block.materializePri = materializePri;
        block.materializeAlt = materializeAlt;
        return true;
    }

    void EmitOneSymbolicValue(asmjit::x86::Assembler& a, const asmjit::x86::Gp& destination, bool destinationIsPri, const SymbolicValue& value)
    {
        using namespace asmjit::x86;
        switch (value.origin)
        {
            case SymbolicOrigin::InitialPri:
                if (!destinationIsPri)
                    a.mov(destination, edi);
                break;
            case SymbolicOrigin::InitialAlt:
                if (destinationIsPri)
                    a.mov(destination, ebx);
                break;
            case SymbolicOrigin::Constant:
                EmitKnownConstant(a, destination, value.constant);
                break;
            case SymbolicOrigin::Derived:
                break;
        }
    }

    void EmitSymbolicState(asmjit::x86::Assembler& a, const ValueSuperblock& block)
    {
        using namespace asmjit::x86;
        const auto& pri = block.state.pri;
        const auto& alt = block.state.alt;

        if (block.materializePri && block.materializeAlt)
        {
            if (pri.origin == SymbolicOrigin::InitialAlt && alt.origin == SymbolicOrigin::InitialPri)
            {
                a.xchg(edi, ebx);
                return;
            }

            if (alt.origin == SymbolicOrigin::InitialPri && pri.origin != SymbolicOrigin::InitialPri)
            {
                a.mov(ebx, edi);
                EmitOneSymbolicValue(a, edi, true, pri);
                return;
            }

            if (pri.origin == SymbolicOrigin::InitialAlt && alt.origin != SymbolicOrigin::InitialAlt)
            {
                a.mov(edi, ebx);
                EmitOneSymbolicValue(a, ebx, false, alt);
                return;
            }

            EmitOneSymbolicValue(a, edi, true, pri);
            EmitOneSymbolicValue(a, ebx, false, alt);
            return;
        }

        if (block.materializePri)
            EmitOneSymbolicValue(a, edi, true, pri);
        if (block.materializeAlt)
            EmitOneSymbolicValue(a, ebx, false, alt);
    }

    enum class DirectSlotKind : uint8_t
    {
        Global,
        Local,
    };

    struct DirectSlot
    {
        DirectSlotKind kind = DirectSlotKind::Global;
        cell offset = 0;
    };

    enum class StoredValueKind : uint8_t
    {
        Pri,
        Alt,
        Zero,
    };

    struct StoreForwardBlock
    {
        SymbolicState state;
        cell targetCip = 0;
        bool materializePri = true;
        bool materializeAlt = true;
    };

    bool DecodeDirectStore(const Instruction& instruction, DirectSlot& slot, StoredValueKind& valueKind)
    {
        if (instruction.operands.empty())
            return false;

        slot.offset = instruction.operands[0];
        switch (instruction.opcode)
        {
            case Opcode::STOR_PRI:
                slot.kind = DirectSlotKind::Global;
                valueKind = StoredValueKind::Pri;
                return true;
            case Opcode::STOR_ALT:
                slot.kind = DirectSlotKind::Global;
                valueKind = StoredValueKind::Alt;
                return true;
            case Opcode::STOR_S_PRI:
                slot.kind = DirectSlotKind::Local;
                valueKind = StoredValueKind::Pri;
                return true;
            case Opcode::STOR_S_ALT:
                slot.kind = DirectSlotKind::Local;
                valueKind = StoredValueKind::Alt;
                return true;
            case Opcode::ZERO:
                slot.kind = DirectSlotKind::Global;
                valueKind = StoredValueKind::Zero;
                return true;
            case Opcode::ZERO_S:
                slot.kind = DirectSlotKind::Local;
                valueKind = StoredValueKind::Zero;
                return true;
            default:
                return false;
        }
    }

    bool DecodeMatchingDirectLoad(const Instruction& instruction, const DirectSlot& slot, bool& destinationPri)
    {
        if (instruction.operands.empty() || instruction.operands[0] != slot.offset)
            return false;

        if (slot.kind == DirectSlotKind::Global)
        {
            if (instruction.opcode == Opcode::LOAD_PRI)
            {
                destinationPri = true;
                return true;
            }
            if (instruction.opcode == Opcode::LOAD_ALT)
            {
                destinationPri = false;
                return true;
            }
            return false;
        }

        if (instruction.opcode == Opcode::LOAD_S_PRI)
        {
            destinationPri = true;
            return true;
        }
        if (instruction.opcode == Opcode::LOAD_S_ALT)
        {
            destinationPri = false;
            return true;
        }
        return false;
    }

    bool BuildStoreForwardBlock(AMX* amx, const DecodedProgram& decoded, const ControlFlowGraph& cfg, const LocalLiveness& liveness, std::size_t startIndex,
                                StoreForwardBlock& block)
    {
        const std::size_t count = decoded.instructions.size();
        if (startIndex >= count)
            return false;

        DirectSlot slot;
        StoredValueKind valueKind = StoredValueKind::Pri;
        if (!DecodeDirectStore(decoded.instructions[startIndex], slot, valueKind))
            return false;

        SymbolicState state;
        SymbolicValue storedValue;
        switch (valueKind)
        {
            case StoredValueKind::Pri:
                storedValue = state.pri;
                break;
            case StoredValueKind::Alt:
                storedValue = state.alt;
                break;
            case StoredValueKind::Zero:
                storedValue = MakeSymbolicConstant(0);
                break;
        }
        std::size_t index = startIndex + 1;
        std::size_t folded = 0;
        std::size_t forwardedLoads = 0;
        std::vector<uint8_t> visited(count, 0);
        visited[startIndex] = 1;
        const std::size_t maxTraceOps = static_cast<std::size_t>(kStoreForwardTraceMaxOps);

        while (index < count && folded < maxTraceOps)
        {
            if (visited[index] != 0)
                break;

            const Instruction& current = decoded.instructions[index];
            bool destinationPri = true;
            if (DecodeMatchingDirectLoad(current, slot, destinationPri))
            {
                if (destinationPri)
                    state.pri = storedValue;
                else
                    state.alt = storedValue;

                visited[index] = 1;
                ++folded;
                ++forwardedLoads;
                ++index;
                continue;
            }

            bool equalityFold = false;
            if (ApplySymbolicOpcode(current, state, equalityFold))
            {
                visited[index] = 1;
                ++folded;
                ++index;
                continue;
            }

            cell branchTarget = 0;
            if (!ResolveSymbolicBranch(amx, decoded, current, state, branchTarget, equalityFold))
                break;

            auto targetIt = decoded.byCip.find(branchTarget);
            if (targetIt == decoded.byCip.end())
                break;

            const std::size_t targetIndex = targetIt->second;
            if (!HasCfgEdge(cfg, index, targetIndex))
                break;

            visited[index] = 1;
            ++folded;
            index = targetIndex;
            continue;
        }

        if (index >= count || forwardedLoads == 0)
            return false;

        const bool materializePri = liveness.IsLiveIn(index, RegPri);
        const bool materializeAlt = liveness.IsLiveIn(index, RegAlt);
        const std::size_t materializeCost = EstimateSymbolicMaterializationCost(state, materializePri, materializeAlt);

        if (materializeCost == std::numeric_limits<std::size_t>::max())
            return false;

        /*
            the original store itself is still emitted. The fast path replaces
            every instruction after it with register materialization + one
            JMP.
        */
        if (folded <= materializeCost + 1u)
            return false;

        block.state = state;
        block.targetCip = decoded.instructions[index].cip;
        block.materializePri = materializePri;
        block.materializeAlt = materializeAlt;
        return true;
    }

    void EmitStoreForwardState(asmjit::x86::Assembler& a, const StoreForwardBlock& block)
    {
        ValueSuperblock materialized;
        materialized.state = block.state;
        materialized.materializePri = block.materializePri;
        materialized.materializeAlt = block.materializeAlt;
        EmitSymbolicState(a, materialized);
    }

    struct MemoryValueStoreAction
    {
        DirectSlot slot;
        SymbolicValue value;
        bool active = true;
    };

    struct MemoryValueSlot
    {
        DirectSlot slot;
        SymbolicValue value;
        std::size_t lastStoreAction = std::numeric_limits<std::size_t>::max();
    };

    struct MemoryValueBlock
    {
        SymbolicState state;
        std::vector<MemoryValueStoreAction> stores;
        cell targetCip = 0;
        bool materializePri = true;
        bool materializeAlt = true;
    };

    bool SameDirectSlot(const DirectSlot& lhs, const DirectSlot& rhs)
    {
        return lhs.kind == rhs.kind && lhs.offset == rhs.offset;
    }

    bool DirectSlotsOverlap(const DirectSlot& lhs, const DirectSlot& rhs)
    {
        /*
            A global absolute address and a frame-relative local may resolve
            to the same data cell at runtime. Keep those address spaces
            aliased.
        */
        if (lhs.kind != rhs.kind)
            return true;

        const int64_t lhsBegin = static_cast<int64_t>(lhs.offset);
        const int64_t lhsEnd = lhsBegin + static_cast<int64_t>(sizeof(cell));
        const int64_t rhsBegin = static_cast<int64_t>(rhs.offset);
        const int64_t rhsEnd = rhsBegin + static_cast<int64_t>(sizeof(cell));
        return lhsBegin < rhsEnd && rhsBegin < lhsEnd;
    }

    bool DecodeAnyDirectLoad(const Instruction& instruction, DirectSlot& slot, bool& destinationPri)
    {
        if (instruction.operands.empty())
            return false;

        slot.offset = instruction.operands[0];
        switch (instruction.opcode)
        {
            case Opcode::LOAD_PRI:
                slot.kind = DirectSlotKind::Global;
                destinationPri = true;
                return true;
            case Opcode::LOAD_ALT:
                slot.kind = DirectSlotKind::Global;
                destinationPri = false;
                return true;
            case Opcode::LOAD_S_PRI:
                slot.kind = DirectSlotKind::Local;
                destinationPri = true;
                return true;
            case Opcode::LOAD_S_ALT:
                slot.kind = DirectSlotKind::Local;
                destinationPri = false;
                return true;
            default:
                return false;
        }
    }

    std::size_t FindMemoryValueSlot(const std::vector<MemoryValueSlot>& slots, const DirectSlot& slot)
    {
        for (std::size_t i = 0; i < slots.size(); ++i)
        {
            if (SameDirectSlot(slots[i].slot, slot))
                return i;
        }
        return std::numeric_limits<std::size_t>::max();
    }

    bool HasConflictingTrackedSlot(const std::vector<MemoryValueSlot>& slots, const DirectSlot& slot)
    {
        for (const MemoryValueSlot& tracked : slots)
        {
            if (!SameDirectSlot(tracked.slot, slot) && DirectSlotsOverlap(tracked.slot, slot))
                return true;
        }
        return false;
    }

    SymbolicValue StoredSymbolicValue(const SymbolicState& state, StoredValueKind valueKind)
    {
        switch (valueKind)
        {
            case StoredValueKind::Pri:
                return state.pri;
            case StoredValueKind::Alt:
                return state.alt;
            case StoredValueKind::Zero:
                return MakeSymbolicConstant(0);
        }
        return {};
    }

    bool BuildMemoryValueBlock(AMX* amx, const DecodedProgram& decoded, const ControlFlowGraph& cfg, const LocalLiveness& liveness, std::size_t startIndex, MemoryValueBlock& block)
    {
        const std::size_t count = decoded.instructions.size();
        if (startIndex >= count)
            return false;

        DirectSlot rootSlot;
        StoredValueKind rootValueKind = StoredValueKind::Pri;
        if (!DecodeDirectStore(decoded.instructions[startIndex], rootSlot, rootValueKind))
            return false;

        SymbolicState state;
        std::vector<MemoryValueSlot> slots;
        std::vector<MemoryValueStoreAction> stores;
        std::vector<uint8_t> visited(count, 0);
        std::size_t folded = 0;
        std::size_t forwardedLoads = 0;
        std::size_t index = startIndex;
        const std::size_t maxTraceOps = static_cast<std::size_t>(kMemoryValueTraceMaxOps);
        const std::size_t maxSlots = static_cast<std::size_t>(kMemoryValueMaxSlots);

        while (index < count && folded < maxTraceOps)
        {
            if (visited[index] != 0)
                break;

            const Instruction& current = decoded.instructions[index];

            DirectSlot storeSlot;
            StoredValueKind valueKind = StoredValueKind::Pri;
            if (DecodeDirectStore(current, storeSlot, valueKind))
            {
                if (HasConflictingTrackedSlot(slots, storeSlot))
                    break;

                SymbolicValue value = StoredSymbolicValue(state, valueKind);
                if (!IsMaterializableSymbolic(value))
                    break;

                std::size_t trackedIndex = FindMemoryValueSlot(slots, storeSlot);
                if (trackedIndex == std::numeric_limits<std::size_t>::max())
                {
                    if (slots.size() >= maxSlots)
                        break;
                    trackedIndex = slots.size();
                    slots.push_back({storeSlot, value, std::numeric_limits<std::size_t>::max()});
                }

                MemoryValueSlot& tracked = slots[trackedIndex];
                if (tracked.lastStoreAction != std::numeric_limits<std::size_t>::max())
                {
                    MemoryValueStoreAction& previous = stores[tracked.lastStoreAction];
                    if (previous.active)
                    {
                        previous.active = false;
                    }
                }

                const std::size_t actionIndex = stores.size();
                stores.push_back({storeSlot, value, true});
                tracked.value = value;
                tracked.lastStoreAction = actionIndex;

                visited[index] = 1;
                ++folded;
                ++index;
                continue;
            }

            DirectSlot loadSlot;
            bool destinationPri = true;
            if (DecodeAnyDirectLoad(current, loadSlot, destinationPri))
            {
                if (HasConflictingTrackedSlot(slots, loadSlot))
                    break;

                const std::size_t trackedIndex = FindMemoryValueSlot(slots, loadSlot);
                if (trackedIndex == std::numeric_limits<std::size_t>::max())
                    break;

                if (destinationPri)
                    state.pri = slots[trackedIndex].value;
                else
                    state.alt = slots[trackedIndex].value;

                visited[index] = 1;
                ++folded;
                ++forwardedLoads;
                ++index;
                continue;
            }

            bool equalityFold = false;
            if (ApplySymbolicOpcode(current, state, equalityFold))
            {
                visited[index] = 1;
                ++folded;
                ++index;
                continue;
            }

            cell branchTarget = 0;
            if (!ResolveSymbolicBranch(amx, decoded, current, state, branchTarget, equalityFold))
                break;

            auto targetIt = decoded.byCip.find(branchTarget);
            if (targetIt == decoded.byCip.end())
                break;

            const std::size_t targetIndex = targetIt->second;
            if (!HasCfgEdge(cfg, index, targetIndex))
                break;

            visited[index] = 1;
            ++folded;
            index = targetIndex;
            continue;
        }

        if (index >= count || forwardedLoads < 2 || slots.size() < 2)
            return false;

        const bool materializePri = liveness.IsLiveIn(index, RegPri);
        const bool materializeAlt = liveness.IsLiveIn(index, RegAlt);
        const std::size_t materializeCost = EstimateSymbolicMaterializationCost(state, materializePri, materializeAlt);
        if (materializeCost == std::numeric_limits<std::size_t>::max())
            return false;

        std::size_t activeStores = 0;
        for (const MemoryValueStoreAction& action : stores)
        {
            if (action.active)
                ++activeStores;
        }

        /*
            active stores still have to execute. Everything else in the trace
            is replaced by forwarding/folding plus final register
            materialization.
        */
        if (folded <= activeStores + materializeCost + 1u)
            return false;

        block.state = state;
        block.stores = std::move(stores);
        block.targetCip = decoded.instructions[index].cip;
        block.materializePri = materializePri;
        block.materializeAlt = materializeAlt;
        return true;
    }

    void EmitMemoryValueStore(asmjit::x86::Assembler& a, uint32_t dataBase, const MemoryValueStoreAction& action, const asmjit::Label& memoryError)
    {
        using namespace asmjit::x86;
        (void)memoryError;
        if (!action.active)
            return;

        auto emitGlobal = [&](const SymbolicValue& value) {
            if (value.origin == SymbolicOrigin::Constant)
                a.mov(DataAbs(dataBase, action.slot.offset), value.constant);
            else if (value.origin == SymbolicOrigin::InitialPri)
                a.mov(DataAbs(dataBase, action.slot.offset), edi);
            else if (value.origin == SymbolicOrigin::InitialAlt)
                a.mov(DataAbs(dataBase, action.slot.offset), ebx);
        };

        auto emitLocal = [&](const SymbolicValue& value) {
            const auto memory = dword_ptr(ebp, DataDisp(dataBase, action.slot.offset));
            if (value.origin == SymbolicOrigin::Constant)
                a.mov(memory, value.constant);
            else if (value.origin == SymbolicOrigin::InitialPri)
                a.mov(memory, edi);
            else if (value.origin == SymbolicOrigin::InitialAlt)
                a.mov(memory, ebx);
        };

        if (action.slot.kind == DirectSlotKind::Global)
            emitGlobal(action.value);
        else
            emitLocal(action.value);
    }

    void EmitMemoryValueState(asmjit::x86::Assembler& a, const MemoryValueBlock& block)
    {
        ValueSuperblock materialized;
        materialized.state = block.state;
        materialized.materializePri = block.materializePri;
        materialized.materializeAlt = block.materializeAlt;
        EmitSymbolicState(a, materialized);
    }

    struct MemoryMergeArm
    {
        SymbolicState state;
        std::vector<MemoryValueSlot> slots;
        std::vector<std::size_t> visitedIndices;
        std::size_t mergeIndex = std::numeric_limits<std::size_t>::max();
        std::size_t foldedInstructions = 0;
    };

    struct MemoryMergeBlock
    {
        SymbolicState state;
        std::vector<MemoryValueStoreAction> stores;
        std::vector<MemoryValueStoreAction> takenStores;
        std::vector<MemoryValueStoreAction> fallthroughStores;
        cell targetCip = 0;
        bool materializePri = true;
        bool materializeAlt = true;
        bool preserveBranch = false;
    };

    bool IsMemoryMergeMetadata(Opcode opcode)
    {
        switch (opcode)
        {
            case Opcode::NOP:
            case Opcode::LINE:
            case Opcode::SYMBOL:
            case Opcode::SRANGE:
            case Opcode::SYMTAG:
                return true;
            default:
                return false;
        }
    }

    bool AddOrUpdateMemoryMergeSlot(std::vector<MemoryValueSlot>& slots, const DirectSlot& slot, const SymbolicValue& value, std::size_t maxSlots)
    {
        if (!IsMaterializableSymbolic(value) || HasConflictingTrackedSlot(slots, slot))
            return false;

        const std::size_t existing = FindMemoryValueSlot(slots, slot);
        if (existing != std::numeric_limits<std::size_t>::max())
        {
            slots[existing].value = value;
            return true;
        }

        if (slots.size() >= maxSlots)
            return false;

        slots.push_back({slot, value, std::numeric_limits<std::size_t>::max()});
        return true;
    }

    bool SameMemoryMergeState(const SymbolicState& lhs, const SymbolicState& rhs);
    bool SameMemoryMergeSlots(const std::vector<MemoryValueSlot>& lhs, const std::vector<MemoryValueSlot>& rhs);
    std::vector<MemoryValueSlot> IntersectMemoryMergeSlots(const std::vector<MemoryValueSlot>& lhs, const std::vector<MemoryValueSlot>& rhs);
    std::vector<MemoryValueStoreAction> BuildMemoryMergeSideStores(const std::vector<MemoryValueSlot>& armSlots, const std::vector<MemoryValueSlot>& commonSlots);
    void EliminateDeadMergeSideStores(const DecodedProgram& decoded, const ControlFlowGraph& cfg, std::size_t mergeIndex, std::vector<MemoryValueStoreAction>& stores);

    bool TraceMemoryMergeArm(const DecodedProgram& decoded, const ControlFlowGraph& cfg, std::size_t startIndex, std::size_t maxOps, std::size_t maxSlots, MemoryMergeArm& arm,
                             std::size_t depth = 0)
    {
        const std::size_t count = decoded.instructions.size();
        if (startIndex >= count)
            return false;

        std::vector<uint8_t> visited(count, 0);
        std::size_t index = startIndex;
        std::size_t bypassJoinIndex = std::numeric_limits<std::size_t>::max();

        while (index < count && arm.foldedInstructions < maxOps)
        {
            /*
                the first CFG join is normally the arm boundary. A nested
                diamond that we just proved and collapsed is the one
                exception: consume that exact merge instruction once, then
                restore the normal join barrier so the outer arm can continue
                to its real merge point.
            */
            if (cfg.predecessorCount[index] > 1)
            {
                if (index == bypassJoinIndex)
                {
                    bypassJoinIndex = std::numeric_limits<std::size_t>::max();
                }
                else
                {
                    arm.mergeIndex = index;
                    return true;
                }
            }

            if (visited[index] != 0)
                return false;

            visited[index] = 1;
            arm.visitedIndices.push_back(index);
            const Instruction& current = decoded.instructions[index];

            DirectSlot storeSlot;
            StoredValueKind valueKind = StoredValueKind::Pri;
            if (DecodeDirectStore(current, storeSlot, valueKind))
            {
                const SymbolicValue value = StoredSymbolicValue(arm.state, valueKind);
                if (!AddOrUpdateMemoryMergeSlot(arm.slots, storeSlot, value, maxSlots))
                    return false;

                ++arm.foldedInstructions;
                ++index;
                continue;
            }

            DirectSlot loadSlot;
            bool destinationPri = true;
            if (DecodeAnyDirectLoad(current, loadSlot, destinationPri))
            {
                /*
                    an untracked direct load is safe to discard only when it
                    cannot alias any store already hoisted by this arm. Its
                    runtime value becomes a fresh symbolic value; nested exact
                    diamonds may still overwrite it with a materializable
                    state.
                */
                if (HasConflictingTrackedSlot(arm.slots, loadSlot))
                    return false;

                SymbolicValue value;
                const std::size_t trackedIndex = FindMemoryValueSlot(arm.slots, loadSlot);
                if (trackedIndex != std::numeric_limits<std::size_t>::max())
                    value = arm.slots[trackedIndex].value;
                else
                    value = MakeDerived(arm.state);

                if (destinationPri)
                    arm.state.pri = value;
                else
                    arm.state.alt = value;

                ++arm.foldedInstructions;
                ++index;
                continue;
            }

            bool equalityFold = false;
            if (ApplySymbolicOpcode(current, arm.state, equalityFold))
            {
                ++arm.foldedInstructions;
                ++index;
                continue;
            }

            if (IsMemoryMergeMetadata(current.opcode))
            {
                ++arm.foldedInstructions;
                ++index;
                continue;
            }

            if (IsConditionalDirectBranch(current.opcode))
            {
                if (depth >= static_cast<std::size_t>(kMemoryMergeMaxDepth))
                    return false;

                const auto& nestedSuccessors = cfg.successors[index];
                if (nestedSuccessors.size() != 2 || nestedSuccessors[0] <= index || nestedSuccessors[1] <= index)
                {
                    return false;
                }

                const std::size_t remainingOps = maxOps > arm.foldedInstructions + 1u ? maxOps - arm.foldedInstructions - 1u : 0u;
                if (remainingOps == 0)
                    return false;

                MemoryMergeArm nestedLeft;
                MemoryMergeArm nestedRight;
                nestedLeft.state = arm.state;
                nestedRight.state = arm.state;
                nestedLeft.slots = arm.slots;
                nestedRight.slots = arm.slots;

                if (!TraceMemoryMergeArm(decoded, cfg, nestedSuccessors[0], remainingOps, maxSlots, nestedLeft, depth + 1u) ||
                    !TraceMemoryMergeArm(decoded, cfg, nestedSuccessors[1], remainingOps, maxSlots, nestedRight, depth + 1u))
                {
                    return false;
                }

                if (nestedLeft.mergeIndex == std::numeric_limits<std::size_t>::max() || nestedLeft.mergeIndex != nestedRight.mergeIndex || nestedLeft.mergeIndex <= index ||
                    !SameMemoryMergeState(nestedLeft.state, nestedRight.state))
                {
                    return false;
                }

                std::vector<MemoryValueSlot> nestedSlots;

                if (SameMemoryMergeSlots(nestedLeft.slots, nestedRight.slots))
                {
                    nestedSlots = nestedLeft.slots;
                }
                else
                {
                    nestedSlots = IntersectMemoryMergeSlots(nestedLeft.slots, nestedRight.slots);
                    if (nestedSlots.empty())
                        return false;

                    std::vector<MemoryValueStoreAction> nestedTakenStores = BuildMemoryMergeSideStores(nestedLeft.slots, nestedSlots);
                    std::vector<MemoryValueStoreAction> nestedFallthroughStores = BuildMemoryMergeSideStores(nestedRight.slots, nestedSlots);

                    EliminateDeadMergeSideStores(decoded, cfg, nestedLeft.mergeIndex, nestedTakenStores);
                    EliminateDeadMergeSideStores(decoded, cfg, nestedLeft.mergeIndex, nestedFallthroughStores);

                    auto hasActiveStore = [](const std::vector<MemoryValueStoreAction>& stores) {
                        for (const MemoryValueStoreAction& action : stores)
                        {
                            if (action.active)
                                return true;
                        }
                        return false;
                    };

                    /*
                        A nested partial intersection can be collapsed only
                        when every branch-specific side effect is proven dead
                        after the nested merge. Otherwise the runtime branch
                        must stay, which the arm tracer deliberately does not
                        encode.
                    */
                    if (hasActiveStore(nestedTakenStores) || hasActiveStore(nestedFallthroughStores))
                    {
                        return false;
                    }
                }

                visited[index] = 1;
                arm.visitedIndices.push_back(index);
                arm.visitedIndices.insert(arm.visitedIndices.end(), nestedLeft.visitedIndices.begin(), nestedLeft.visitedIndices.end());
                arm.visitedIndices.insert(arm.visitedIndices.end(), nestedRight.visitedIndices.begin(), nestedRight.visitedIndices.end());

                arm.state = nestedLeft.state;
                arm.slots = std::move(nestedSlots);
                arm.foldedInstructions += 1u + std::min(nestedLeft.foldedInstructions, nestedRight.foldedInstructions);
                bypassJoinIndex = nestedLeft.mergeIndex;
                index = nestedLeft.mergeIndex;
                continue;
            }

            if (!IsUnconditionalDirectBranch(current.opcode))
                return false;

            const auto& successors = cfg.successors[index];
            if (successors.size() != 1)
                return false;

            ++arm.foldedInstructions;
            index = successors[0];
        }

        return false;
    }

    bool SameMemoryMergeState(const SymbolicState& lhs, const SymbolicState& rhs)
    {
        if (!IsMaterializableSymbolic(lhs.pri) || !IsMaterializableSymbolic(lhs.alt) || !IsMaterializableSymbolic(rhs.pri) || !IsMaterializableSymbolic(rhs.alt))
        {
            return false;
        }

        return SymbolicEqual(lhs.pri, rhs.pri) && SymbolicEqual(lhs.alt, rhs.alt);
    }

    bool SameMemoryMergeSlots(const std::vector<MemoryValueSlot>& lhs, const std::vector<MemoryValueSlot>& rhs)
    {
        if (lhs.empty() || lhs.size() != rhs.size())
            return false;

        for (const MemoryValueSlot& left : lhs)
        {
            const std::size_t rightIndex = FindMemoryValueSlot(rhs, left.slot);
            if (rightIndex == std::numeric_limits<std::size_t>::max())
                return false;

            if (!IsMaterializableSymbolic(left.value) || !IsMaterializableSymbolic(rhs[rightIndex].value) || !SymbolicEqual(left.value, rhs[rightIndex].value))
            {
                return false;
            }
        }

        return true;
    }

    std::vector<MemoryValueSlot> IntersectMemoryMergeSlots(const std::vector<MemoryValueSlot>& lhs, const std::vector<MemoryValueSlot>& rhs)
    {
        std::vector<MemoryValueSlot> intersection;
        intersection.reserve(lhs.size() < rhs.size() ? lhs.size() : rhs.size());

        for (const MemoryValueSlot& left : lhs)
        {
            const std::size_t rightIndex = FindMemoryValueSlot(rhs, left.slot);
            if (rightIndex == std::numeric_limits<std::size_t>::max())
                continue;

            const MemoryValueSlot& right = rhs[rightIndex];
            if (!IsMaterializableSymbolic(left.value) || !IsMaterializableSymbolic(right.value) || !SymbolicEqual(left.value, right.value))
            {
                continue;
            }

            intersection.push_back({left.slot, left.value, std::numeric_limits<std::size_t>::max()});
        }

        return intersection;
    }

    std::vector<MemoryValueStoreAction> BuildMemoryMergeSideStores(const std::vector<MemoryValueSlot>& armSlots, const std::vector<MemoryValueSlot>& commonSlots)
    {
        std::vector<MemoryValueStoreAction> stores;
        stores.reserve(armSlots.size());

        for (const MemoryValueSlot& slot : armSlots)
        {
            const std::size_t commonIndex = FindMemoryValueSlot(commonSlots, slot.slot);
            if (commonIndex != std::numeric_limits<std::size_t>::max() && SymbolicEqual(commonSlots[commonIndex].value, slot.value))
            {
                continue;
            }
            stores.push_back({slot.slot, slot.value, true});
        }

        return stores;
    }

    DseSlot ToDseSlot(const DirectSlot& slot)
    {
        DseSlot converted;
        converted.kind = slot.kind == DirectSlotKind::Global ? DseSlotKind::Global : DseSlotKind::Local;
        converted.offset = slot.offset;
        return converted;
    }

    void EliminateDeadMergeSideStores(const DecodedProgram& decoded, const ControlFlowGraph& cfg, std::size_t mergeIndex, std::vector<MemoryValueStoreAction>& stores)
    {
        if (mergeIndex >= decoded.instructions.size())
            return;

        const std::size_t noCycleAnchor = decoded.instructions.size();

        for (MemoryValueStoreAction& action : stores)
        {
            if (!action.active)
                continue;

            RedundantStoreProof proof;
            std::vector<DseProofState> states(decoded.instructions.size(), DseProofState::Unknown);
            const DseSlot target = ToDseSlot(action.slot);

            if (ProveStoreOverwrittenOnAllPaths(decoded, cfg, target, noCycleAnchor, mergeIndex, states, proof))
            {
                action.active = false;
            }
        }
    }

    bool BuildMemoryMergeBlock(AMX* amx, const DecodedProgram& decoded, const ControlFlowGraph& cfg, const LocalLiveness& liveness, std::size_t startIndex, MemoryMergeBlock& block)
    {
        const std::size_t count = decoded.instructions.size();
        if (startIndex >= count || !IsConditionalDirectBranch(decoded.instructions[startIndex].opcode))
            return false;

        const auto& branchSuccessors = cfg.successors[startIndex];
        if (branchSuccessors.size() != 2)
            return false;

        /*
            keep the merge transform intentionally forward-only. This avoids
            treating loop headers as diamonds and keeps indirect/cold entries
            canonical.
        */
        if (branchSuccessors[0] <= startIndex || branchSuccessors[1] <= startIndex)
            return false;

        const std::size_t maxArmOps = static_cast<std::size_t>(kMemoryMergeArmMaxOps);
        const std::size_t maxTraceOps = static_cast<std::size_t>(kMemoryMergeTraceMaxOps);
        const std::size_t maxSlots = static_cast<std::size_t>(kMemoryValueMaxSlots);

        MemoryMergeArm left;
        MemoryMergeArm right;
        if (!TraceMemoryMergeArm(decoded, cfg, branchSuccessors[0], maxArmOps, maxSlots, left) ||
            !TraceMemoryMergeArm(decoded, cfg, branchSuccessors[1], maxArmOps, maxSlots, right))
        {
            return false;
        }

        if (left.mergeIndex == std::numeric_limits<std::size_t>::max() || left.mergeIndex != right.mergeIndex || left.mergeIndex <= startIndex)
        {
            return false;
        }

        if (!SameMemoryMergeState(left.state, right.state))
            return false;

        const bool identicalSlots = SameMemoryMergeSlots(left.slots, right.slots);
        std::vector<MemoryValueSlot> slots;
        bool preserveBranch = false;

        if (identicalSlots)
        {
            slots = left.slots;
        }
        else
        {
            slots = IntersectMemoryMergeSlots(left.slots, right.slots);
            if (slots.empty())
                return false;
            preserveBranch = true;
        }

        SymbolicState state = left.state;
        std::vector<MemoryValueStoreAction> stores;
        stores.reserve(slots.size());
        for (std::size_t i = 0; i < slots.size(); ++i)
        {
            const std::size_t actionIndex = stores.size();
            stores.push_back({slots[i].slot, slots[i].value, true});
            slots[i].lastStoreAction = actionIndex;
        }

        std::vector<MemoryValueStoreAction> takenStores;
        std::vector<MemoryValueStoreAction> fallthroughStores;
        if (preserveBranch)
        {
            takenStores = BuildMemoryMergeSideStores(left.slots, slots);
            fallthroughStores = BuildMemoryMergeSideStores(right.slots, slots);
        }

        if (preserveBranch)
        {
            EliminateDeadMergeSideStores(decoded, cfg, left.mergeIndex, takenStores);
            EliminateDeadMergeSideStores(decoded, cfg, left.mergeIndex, fallthroughStores);
        }

        std::vector<uint8_t> visited(count, 0);
        visited[startIndex] = 1;
        for (std::size_t index : left.visitedIndices)
            visited[index] = 1;
        for (std::size_t index : right.visitedIndices)
            visited[index] = 1;

        std::size_t index = left.mergeIndex;
        std::size_t continuationOps = 0;
        std::size_t forwardedLoads = 0;

        while (index < count && continuationOps < maxTraceOps)
        {
            if (visited[index] != 0)
                break;

            const Instruction& current = decoded.instructions[index];

            DirectSlot storeSlot;
            StoredValueKind valueKind = StoredValueKind::Pri;
            if (DecodeDirectStore(current, storeSlot, valueKind))
            {
                if (HasConflictingTrackedSlot(slots, storeSlot))
                    break;

                const SymbolicValue value = StoredSymbolicValue(state, valueKind);
                if (!IsMaterializableSymbolic(value))
                    break;

                std::size_t trackedIndex = FindMemoryValueSlot(slots, storeSlot);
                if (trackedIndex == std::numeric_limits<std::size_t>::max())
                {
                    if (slots.size() >= maxSlots)
                        break;
                    trackedIndex = slots.size();
                    slots.push_back({storeSlot, value, std::numeric_limits<std::size_t>::max()});
                }

                MemoryValueSlot& tracked = slots[trackedIndex];
                if (tracked.lastStoreAction != std::numeric_limits<std::size_t>::max())
                {
                    MemoryValueStoreAction& previous = stores[tracked.lastStoreAction];
                    if (previous.active)
                    {
                        previous.active = false;
                    }
                }

                const std::size_t actionIndex = stores.size();
                stores.push_back({storeSlot, value, true});
                tracked.value = value;
                tracked.lastStoreAction = actionIndex;

                visited[index] = 1;
                ++continuationOps;
                ++index;
                continue;
            }

            DirectSlot loadSlot;
            bool destinationPri = true;
            if (DecodeAnyDirectLoad(current, loadSlot, destinationPri))
            {
                if (HasConflictingTrackedSlot(slots, loadSlot))
                    break;

                const std::size_t trackedIndex = FindMemoryValueSlot(slots, loadSlot);
                if (trackedIndex == std::numeric_limits<std::size_t>::max())
                    break;

                if (destinationPri)
                    state.pri = slots[trackedIndex].value;
                else
                    state.alt = slots[trackedIndex].value;

                visited[index] = 1;
                ++continuationOps;
                ++forwardedLoads;
                ++index;
                continue;
            }

            bool equalityFold = false;
            if (ApplySymbolicOpcode(current, state, equalityFold))
            {
                visited[index] = 1;
                ++continuationOps;
                ++index;
                continue;
            }

            cell branchTarget = 0;
            if (!ResolveSymbolicBranch(amx, decoded, current, state, branchTarget, equalityFold))
                break;

            auto targetIt = decoded.byCip.find(branchTarget);
            if (targetIt == decoded.byCip.end())
                break;

            const std::size_t targetIndex = targetIt->second;
            if (!HasCfgEdge(cfg, index, targetIndex))
                break;

            visited[index] = 1;
            ++continuationOps;
            index = targetIndex;
        }

        if (index >= count || forwardedLoads == 0)
            return false;

        const bool materializePri = liveness.IsLiveIn(index, RegPri);
        const bool materializeAlt = liveness.IsLiveIn(index, RegAlt);
        const std::size_t materializeCost = EstimateSymbolicMaterializationCost(state, materializePri, materializeAlt);
        if (materializeCost == std::numeric_limits<std::size_t>::max())
            return false;

        std::size_t activeStores = 0;
        for (const MemoryValueStoreAction& action : stores)
        {
            if (action.active)
                ++activeStores;
        }

        const std::size_t shorterArm = left.foldedInstructions < right.foldedInstructions ? left.foldedInstructions : right.foldedInstructions;
        const std::size_t runtimeFolded = 1u + shorterArm + continuationOps;
        auto countActiveStores = [](const std::vector<MemoryValueStoreAction>& actions) {
            std::size_t active = 0;
            for (const MemoryValueStoreAction& action : actions)
            {
                if (action.active)
                    ++active;
            }
            return active;
        };
        const std::size_t activeTakenStores = countActiveStores(takenStores);
        const std::size_t activeFallthroughStores = countActiveStores(fallthroughStores);
        const std::size_t branchSpecificCost = preserveBranch ? (activeTakenStores > activeFallthroughStores ? activeTakenStores : activeFallthroughStores) + 2u : 0u;
        if (runtimeFolded <= activeStores + branchSpecificCost + materializeCost + 1u)
            return false;

        block.state = state;
        block.stores = std::move(stores);
        block.takenStores = std::move(takenStores);
        block.fallthroughStores = std::move(fallthroughStores);
        block.targetCip = decoded.instructions[index].cip;
        block.materializePri = materializePri;
        block.materializeAlt = materializeAlt;
        block.preserveBranch = preserveBranch;
        return true;
    }

    void EmitMemoryMergeBranch(asmjit::x86::Assembler& a, Opcode opcode, const asmjit::Label& taken)
    {
        using namespace asmjit::x86;
        switch (opcode)
        {
            case Opcode::JZER:
                a.test(edi, edi);
                a.jz(taken);
                break;
            case Opcode::JNZ:
                a.test(edi, edi);
                a.jnz(taken);
                break;
            case Opcode::JEQ:
            case Opcode::JNEQ:
            case Opcode::JLESS:
            case Opcode::JLEQ:
            case Opcode::JGRTR:
            case Opcode::JGEQ:
            case Opcode::JSLESS:
            case Opcode::JSLEQ:
            case Opcode::JSGRTR:
            case Opcode::JSGEQ:
                a.cmp(edi, ebx);
                switch (opcode)
                {
                    case Opcode::JEQ:
                        a.je(taken);
                        break;
                    case Opcode::JNEQ:
                        a.jne(taken);
                        break;
                    case Opcode::JLESS:
                        a.jb(taken);
                        break;
                    case Opcode::JLEQ:
                        a.jbe(taken);
                        break;
                    case Opcode::JGRTR:
                        a.ja(taken);
                        break;
                    case Opcode::JGEQ:
                        a.jae(taken);
                        break;
                    case Opcode::JSLESS:
                        a.jl(taken);
                        break;
                    case Opcode::JSLEQ:
                        a.jle(taken);
                        break;
                    case Opcode::JSGRTR:
                        a.jg(taken);
                        break;
                    case Opcode::JSGEQ:
                        a.jge(taken);
                        break;
                    default:
                        break;
                }
                break;
            default:
                break;
        }
    }

    void EmitMemoryMergeState(asmjit::x86::Assembler& a, const MemoryMergeBlock& block)
    {
        ValueSuperblock materialized;
        materialized.state = block.state;
        materialized.materializePri = block.materializePri;
        materialized.materializeAlt = block.materializeAlt;
        EmitSymbolicState(a, materialized);
    }

    bool NeedsCurrentCip(Opcode opcode)
    {
        switch (opcode)
        {
            case Opcode::LREF_PRI:
            case Opcode::LREF_ALT:
            case Opcode::LREF_S_PRI:
            case Opcode::LREF_S_ALT:
            case Opcode::LOAD_I:
            case Opcode::LODB_I:
            case Opcode::SREF_PRI:
            case Opcode::SREF_ALT:
            case Opcode::SREF_S_PRI:
            case Opcode::SREF_S_ALT:
            case Opcode::STOR_I:
            case Opcode::STRB_I:
            case Opcode::LIDX:
            case Opcode::LIDX_B:
            case Opcode::PUSH_PRI:
            case Opcode::PUSH_ALT:
            case Opcode::PUSH_R:
            case Opcode::PUSH_C:
            case Opcode::PUSH:
            case Opcode::PUSH_S:
            case Opcode::POP_PRI:
            case Opcode::POP_ALT:
            case Opcode::STACK:
            case Opcode::HEAP:
            case Opcode::PROC:
            case Opcode::RET:
            case Opcode::RETN:
            case Opcode::CALL:
            case Opcode::CALL_PRI:
            case Opcode::SDIV:
            case Opcode::SDIV_ALT:
            case Opcode::UDIV:
            case Opcode::UDIV_ALT:
            case Opcode::INC_I:
            case Opcode::DEC_I:
            case Opcode::MOVS:
            case Opcode::CMPS:
            case Opcode::FILL:
            case Opcode::FILE:
            case Opcode::CASETBL:
            case Opcode::SWAP_PRI:
            case Opcode::SWAP_ALT:
            case Opcode::PUSH_ADR:
            case Opcode::PUSH2_C:
            case Opcode::PUSH2:
            case Opcode::PUSH2_S:
            case Opcode::PUSH2_ADR:
            case Opcode::PUSH3_C:
            case Opcode::PUSH3:
            case Opcode::PUSH3_S:
            case Opcode::PUSH3_ADR:
            case Opcode::PUSH4_C:
            case Opcode::PUSH4:
            case Opcode::PUSH4_S:
            case Opcode::PUSH4_ADR:
            case Opcode::PUSH5_C:
            case Opcode::PUSH5:
            case Opcode::PUSH5_S:
            case Opcode::PUSH5_ADR:
            case Opcode::SYSREQ_N:
            case Opcode::JUMP_PRI:
                return true;

            case Opcode::SCTRL:
                return true;

            default:
                return false;
        }
    }

    void EmitInlineFunctionClone(asmjit::x86::Assembler& a, AMX* amx, const DecodedProgram& decoded, const FunctionInliningSummary& inlining, const LocalLiveness& liveness,
                                 cell entry, cell expectedReturnCip, uint32_t dataBase, uint32_t dispatchAddress, const asmjit::Label& invalidAddress,
                                 const asmjit::Label& invalidInstruction, const asmjit::Label& memoryError, const asmjit::Label& stackError, const asmjit::Label& stackLow,
                                 const asmjit::Label& heapLow, const asmjit::Label& boundsError, const asmjit::Label& divideError, const asmjit::Label& exitLabel)
    {
        using namespace asmjit::x86;

        const uint32_t amxAddress = Pointer32(amx);
        const cell hlw = amx->hlw;
        const cell stp = amx->stp;
        const auto functionIt = inlining.functions.find(entry);
        if (functionIt == inlining.functions.end() || !functionIt->second.inlineable)
        {
            a.jmp(invalidInstruction);
            return;
        }

        const InlineFunctionInfo& info = functionIt->second;
        const cell expectedReturnValue = EncodeReturnAddress(expectedReturnCip);
        const std::size_t rangeSize = info.endIndex - info.beginIndex + 1;
        std::vector<asmjit::Label> cloneLabels(rangeSize);
        for (asmjit::Label& label : cloneLabels)
            label = a.new_label();
        const asmjit::Label cloneDone = a.new_label();
        std::vector<cell> deferredJumpTargets(rangeSize, static_cast<cell>(-1));
        std::vector<uint8_t> deferredConditionalBranches(rangeSize, 0);

        auto cloneIndexForCip = [&](cell cip) -> std::size_t {
            const auto targetIt = decoded.byCip.find(cip);
            if (targetIt == decoded.byCip.end() || targetIt->second < info.beginIndex || targetIt->second > info.endIndex)
            {
                return rangeSize;
            }
            return targetIt->second - info.beginIndex;
        };

        for (std::size_t index = info.beginIndex; index <= info.endIndex; ++index)
        {
            const std::size_t localIndex = index - info.beginIndex;
            if (deferredJumpTargets[localIndex] >= 0 || deferredConditionalBranches[localIndex] != 0)
            {
                continue;
            }

            a.bind(cloneLabels[localIndex]);
            const Instruction& instruction = decoded.instructions[index];
            if (liveness.IsDeadPure(index))
                continue;
            if (NeedsCurrentCip(instruction.opcode))
                a.mov(Ctx(offsetof(JitContext, cip)), instruction.cip);
            const cell operand = instruction.operands.empty() ? 0 : instruction.operands[0];

            switch (instruction.opcode)
            {
                case Opcode::PROC:
                    EmitCheckPushSpace(a, dataBase, static_cast<int>(sizeof(cell)), stackError);
                    a.push(ebp);
                    a.lea(ebp, ptr(esp, NegDataDisp(dataBase)));
                    a.mov(edx, Ctx(offsetof(JitContext, hea)));
                    EmitCheckHeapStackMargin(a, edx, ebp, stackError);
                    break;

                case Opcode::RETN:
                {
                    /*
                        keep the exact Pawn frame/parameter layout, but bypass
                        the return-CIP dispatcher because this clone has one
                        native continuation: the instruction following the
                        CALL site.
                    */
                    EmitPopFrameAndReturn(a, dataBase, stp, stackLow);
                    EmitRetnParameterCleanup(a, dataBase, stp, memoryError, stackLow);
                    if (PortableRetnPublishesStack(decoded))
                        EmitPublishPortableRetnStack(a, dataBase, amxAddress);

                    /*
                        normal inline returns fall through locally.  If exotic
                        Pawn bytecode rewrote its saved return CIP, preserve
                        the canonical RETN semantics instead of forcing our
                        caller.
                    */
                    a.cmp(eax, expectedReturnValue);
                    a.je(cloneDone);
                    EmitReturnDispatchEax(a, amx, dispatchAddress, invalidAddress);
                    break;
                }

                case Opcode::CALL:
                {
                    if (index >= inlining.targetByCallInstruction.size())
                    {
                        a.jmp(invalidInstruction);
                        return;
                    }
                    const cell target = inlining.targetByCallInstruction[index];
                    if (target < 0)
                    {
                        a.jmp(invalidInstruction);
                        return;
                    }
                    EmitCheckPushSpace(a, dataBase, static_cast<int>(sizeof(cell)), stackError);
                    a.push(EncodeReturnAddress(instruction.Next()));
                    EmitInlineFunctionClone(a, amx, decoded, inlining, liveness, target, instruction.Next(), dataBase, dispatchAddress, invalidAddress, invalidInstruction,
                                            memoryError, stackError, stackLow, heapLow, boundsError, divideError, exitLabel);
                    break;
                }

                case Opcode::JUMP:
                case Opcode::JREL:
                {
                    const std::size_t targetIndex = cloneIndexForCip(ResolveBranchTarget(amx, instruction));
                    if (targetIndex >= cloneLabels.size())
                    {
                        a.jmp(invalidInstruction);
                        break;
                    }
                    if (targetIndex != localIndex + 1)
                        a.jmp(cloneLabels[targetIndex]);
                    break;
                }

                case Opcode::JZER:
                case Opcode::JNZ:
                {
                    cell diamondJumpTarget = -1;
                    if (index + 1 <= info.endIndex && MatchBranchOverJumpDiamond(amx, decoded, index, diamondJumpTarget))
                    {
                        const std::size_t jumpLocal = localIndex + 1;
                        const std::size_t falseTarget = cloneIndexForCip(diamondJumpTarget);
                        if (jumpLocal < rangeSize && falseTarget < rangeSize)
                        {
                            deferredJumpTargets[jumpLocal] = diamondJumpTarget;
                            if (falseTarget == localIndex + 2)
                                break;
                            a.test(edi, edi);
                            EmitInverseConditionalBranch(a, instruction.opcode, cloneLabels[falseTarget]);
                            break;
                        }
                    }

                    const std::size_t targetIndex = cloneIndexForCip(ResolveBranchTarget(amx, instruction));
                    if (targetIndex >= cloneLabels.size())
                    {
                        a.jmp(invalidInstruction);
                        break;
                    }
                    const asmjit::Label& target = cloneLabels[targetIndex];
                    a.test(edi, edi);
                    if (instruction.opcode == Opcode::JZER)
                        a.jz(target);
                    else
                        a.jnz(target);
                    break;
                }

                case Opcode::JEQ:
                case Opcode::JNEQ:
                case Opcode::JLESS:
                case Opcode::JLEQ:
                case Opcode::JGRTR:
                case Opcode::JGEQ:
                case Opcode::JSLESS:
                case Opcode::JSLEQ:
                case Opcode::JSGRTR:
                case Opcode::JSGEQ:
                {
                    cell diamondJumpTarget = -1;
                    if (index + 1 <= info.endIndex && MatchBranchOverJumpDiamond(amx, decoded, index, diamondJumpTarget))
                    {
                        const std::size_t jumpLocal = localIndex + 1;
                        const std::size_t falseTarget = cloneIndexForCip(diamondJumpTarget);
                        if (jumpLocal < rangeSize && falseTarget < rangeSize)
                        {
                            deferredJumpTargets[jumpLocal] = diamondJumpTarget;
                            if (falseTarget == localIndex + 2)
                                break;
                            a.cmp(edi, ebx);
                            EmitInverseConditionalBranch(a, instruction.opcode, cloneLabels[falseTarget]);
                            break;
                        }
                    }

                    const std::size_t targetIndex = cloneIndexForCip(ResolveBranchTarget(amx, instruction));
                    if (targetIndex >= cloneLabels.size())
                    {
                        a.jmp(invalidInstruction);
                        break;
                    }
                    const asmjit::Label& target = cloneLabels[targetIndex];

                    a.cmp(edi, ebx);
                    switch (instruction.opcode)
                    {
                        case Opcode::JEQ:
                            a.je(target);
                            break;
                        case Opcode::JNEQ:
                            a.jne(target);
                            break;
                        case Opcode::JLESS:
                            a.jb(target);
                            break;
                        case Opcode::JLEQ:
                            a.jbe(target);
                            break;
                        case Opcode::JGRTR:
                            a.ja(target);
                            break;
                        case Opcode::JGEQ:
                            a.jae(target);
                            break;
                        case Opcode::JSLESS:
                            a.jl(target);
                            break;
                        case Opcode::JSLEQ:
                            a.jle(target);
                            break;
                        case Opcode::JSGRTR:
                            a.jg(target);
                            break;
                        case Opcode::JSGEQ:
                            a.jge(target);
                            break;
                        default:
                            break;
                    }
                    break;
                }

                case Opcode::LOAD_PRI:
                    a.mov(edi, DataAbs(dataBase, operand));
                    break;
                case Opcode::LOAD_ALT:
                    a.mov(ebx, DataAbs(dataBase, operand));
                    break;
                case Opcode::LOAD_S_PRI:
                    a.mov(edi, dword_ptr(ebp, DataDisp(dataBase, operand)));
                    break;
                case Opcode::LOAD_S_ALT:
                    a.mov(ebx, dword_ptr(ebp, DataDisp(dataBase, operand)));
                    break;
                case Opcode::LREF_PRI:
                    a.mov(edx, DataAbs(dataBase, operand));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(edi, DataAt(edx, dataBase));
                    break;
                case Opcode::LREF_ALT:
                    a.mov(edx, DataAbs(dataBase, operand));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(ebx, DataAt(edx, dataBase));
                    break;
                case Opcode::LREF_S_PRI:
                    a.lea(edx, ptr(ebp, operand));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(edx, DataAt(edx, dataBase));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(edi, DataAt(edx, dataBase));
                    break;
                case Opcode::LREF_S_ALT:
                    a.lea(edx, ptr(ebp, operand));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(edx, DataAt(edx, dataBase));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(ebx, DataAt(edx, dataBase));
                    break;
                case Opcode::LOAD_I:
                    EmitCheckAddress(a, dataBase, edi, sizeof(cell), stp, memoryError);
                    a.mov(edi, DataAt(edi, dataBase));
                    break;
                case Opcode::LODB_I:
                    EmitCheckAddress(a, dataBase, edi, 1, stp, memoryError);
                    if (operand == 1)
                    {
                        a.movzx(edi, byte_ptr(edi, DataDisp(dataBase)));
                    }
                    else if (operand == 2)
                    {
                        a.movzx(edi, word_ptr(edi, DataDisp(dataBase)));
                    }
                    else if (operand == 4)
                    {
                        a.mov(edi, DataAt(edi, dataBase));
                    }
                    break;
                case Opcode::CONST_PRI:
                    if (operand == 0)
                        a.xor_(edi, edi);
                    else
                        a.mov(edi, operand);
                    break;
                case Opcode::CONST_ALT:
                    if (operand == 0)
                        a.xor_(ebx, ebx);
                    else
                        a.mov(ebx, operand);
                    break;
                case Opcode::ADDR_PRI:
                    a.lea(edi, ptr(ebp, operand));
                    break;
                case Opcode::ADDR_ALT:
                    a.lea(ebx, ptr(ebp, operand));
                    break;
                case Opcode::STOR_PRI:
                    a.mov(DataAbs(dataBase, operand), edi);
                    break;
                case Opcode::STOR_ALT:
                    a.mov(DataAbs(dataBase, operand), ebx);
                    break;
                case Opcode::STOR_S_PRI:
                    a.mov(dword_ptr(ebp, DataDisp(dataBase, operand)), edi);
                    break;
                case Opcode::STOR_S_ALT:
                    a.mov(dword_ptr(ebp, DataDisp(dataBase, operand)), ebx);
                    break;
                case Opcode::SREF_PRI:
                    a.mov(edx, DataAbs(dataBase, operand));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(DataAt(edx, dataBase), edi);
                    break;
                case Opcode::SREF_ALT:
                    a.mov(edx, DataAbs(dataBase, operand));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(DataAt(edx, dataBase), ebx);
                    break;
                case Opcode::SREF_S_PRI:
                    a.lea(edx, ptr(ebp, operand));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(edx, DataAt(edx, dataBase));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(DataAt(edx, dataBase), edi);
                    break;
                case Opcode::SREF_S_ALT:
                    a.lea(edx, ptr(ebp, operand));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(edx, DataAt(edx, dataBase));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(DataAt(edx, dataBase), ebx);
                    break;
                case Opcode::STOR_I:
                    EmitCheckAddress(a, dataBase, ebx, sizeof(cell), stp, memoryError);
                    a.mov(DataAt(ebx, dataBase), edi);
                    break;
                case Opcode::STRB_I:
                    EmitCheckAddress(a, dataBase, ebx, 1, stp, memoryError);
                    if (operand == 1)
                    {
                        a.mov(eax, edi);
                        a.mov(byte_ptr(ebx, DataDisp(dataBase)), al);
                    }
                    else if (operand == 2)
                    {
                        a.mov(eax, edi);
                        a.mov(word_ptr(ebx, DataDisp(dataBase)), ax);
                    }
                    else if (operand == 4)
                    {
                        a.mov(DataAt(ebx, dataBase), edi);
                    }
                    break;
                case Opcode::LIDX:
                    a.lea(edi, ptr(ebx, edi, 2));
                    EmitCheckAddress(a, dataBase, edi, sizeof(cell), stp, memoryError);
                    a.mov(edi, DataAt(edi, dataBase));
                    break;
                case Opcode::LIDX_B:
                {
                    const uint32_t shift = static_cast<uint32_t>(static_cast<ucell>(operand)) & 31u;
                    if (shift <= 3u)
                    {
                        a.lea(edi, ptr(ebx, edi, shift));
                    }
                    else
                    {
                        a.shl(edi, static_cast<uint8_t>(shift));
                        a.add(edi, ebx);
                    }
                    EmitCheckAddress(a, dataBase, edi, sizeof(cell), stp, memoryError);
                    a.mov(edi, DataAt(edi, dataBase));
                    break;
                }
                case Opcode::IDXADDR:
                    a.lea(edi, ptr(ebx, edi, 2));
                    break;
                case Opcode::IDXADDR_B:
                {
                    const uint32_t shift = static_cast<uint32_t>(static_cast<ucell>(operand)) & 31u;
                    if (shift <= 3u)
                        a.lea(edi, ptr(ebx, edi, shift));
                    else
                    {
                        a.shl(edi, static_cast<uint8_t>(shift));
                        a.add(edi, ebx);
                    }
                    break;
                }
                case Opcode::ALIGN_PRI:
                    if (AlignApplies(decoded, operand))
                    {
                        const cell mask = static_cast<cell>(sizeof(cell)) - operand;
                        if (mask != 0)
                            a.xor_(edi, mask);
                    }
                    break;
                case Opcode::ALIGN_ALT:
                    if (AlignApplies(decoded, operand))
                    {
                        const cell mask = static_cast<cell>(sizeof(cell)) - operand;
                        if (mask != 0)
                            a.xor_(ebx, mask);
                    }
                    break;
                case Opcode::MOVE_PRI:
                    a.mov(edi, ebx);
                    break;
                case Opcode::MOVE_ALT:
                    a.mov(ebx, edi);
                    break;
                case Opcode::XCHG:
                {
                    const bool priLive = liveness.IsLiveOut(index, RegPri);
                    const bool altLive = liveness.IsLiveOut(index, RegAlt);
                    if (priLive && altLive)
                        a.xchg(edi, ebx);
                    else if (priLive)
                        a.mov(edi, ebx);
                    else if (altLive)
                        a.mov(ebx, edi);
                    break;
                }
                case Opcode::PUSH_PRI:
                    EmitCheckPushSpace(a, dataBase, static_cast<int>(sizeof(cell)), stackError);
                    a.push(edi);
                    break;
                case Opcode::PUSH_ALT:
                    EmitCheckPushSpace(a, dataBase, static_cast<int>(sizeof(cell)), stackError);
                    a.push(ebx);
                    break;
                case Opcode::PUSH_R:
                    if (operand > 0)
                    {
                        const cell bytes = operand * static_cast<cell>(sizeof(cell));
                        EmitCheckPushSpace(a, dataBase, bytes, stackError);
                        if (operand <= 4)
                        {
                            for (cell i = 0; i < operand; ++i)
                                a.push(edi);
                        }
                        else
                        {
                            asmjit::Label loop = a.new_label();
                            a.mov(ecx, operand);
                            a.bind(loop);
                            a.push(edi);
                            a.dec(ecx);
                            a.jnz(loop);
                        }
                    }
                    break;
                case Opcode::PUSH_C:
                    EmitCheckPushSpace(a, dataBase, static_cast<int>(sizeof(cell)), stackError);
                    a.push(operand);
                    break;
                case Opcode::PUSH:
                    EmitCheckPushSpace(a, dataBase, static_cast<int>(sizeof(cell)), stackError);
                    a.push(DataAbs(dataBase, operand));
                    break;
                case Opcode::PUSH_S:
                    EmitCheckPushSpace(a, dataBase, static_cast<int>(sizeof(cell)), stackError);
                    a.push(dword_ptr(ebp, DataDisp(dataBase, operand)));
                    break;
                case Opcode::POP_PRI:
                    EmitCheckRawStackCell(a, dataBase, stp, stackLow);
                    if (liveness.IsLiveOut(index, RegPri))
                        a.pop(edi);
                    else
                        a.add(esp, static_cast<int>(sizeof(cell)));
                    break;
                case Opcode::POP_ALT:
                    EmitCheckRawStackCell(a, dataBase, stp, stackLow);
                    if (liveness.IsLiveOut(index, RegAlt))
                        a.pop(ebx);
                    else
                        a.add(esp, static_cast<int>(sizeof(cell)));
                    break;
                case Opcode::STACK:
                    a.lea(eax, ptr(esp, NegDataDisp(dataBase)));
                    /*
                        pawn semantics: ALT receives the pre-adjustment STK.
                    */
                    a.mov(ebx, eax);
                    a.add(eax, operand);
                    a.jo(stackError);
                    a.test(eax, eax);
                    a.js(stackError);
                    a.mov(edx, Ctx(offsetof(JitContext, hea)));
                    EmitCheckHeapStackMargin(a, edx, eax, stackError);
                    a.cmp(eax, stp);
                    a.jg(stackLow);
                    a.lea(esp, ptr(eax, static_cast<int32_t>(dataBase)));
                    break;
                case Opcode::HEAP:
                    a.mov(eax, Ctx(offsetof(JitContext, hea)));
                    a.mov(ebx, eax);
                    a.add(eax, operand);
                    a.mov(Ctx(offsetof(JitContext, hea)), eax);
                    a.lea(edx, ptr(esp, NegDataDisp(dataBase)));
                    EmitCheckHeapStackMargin(a, eax, edx, stackError);
                    a.cmp(eax, hlw);
                    a.jl(heapLow);
                    break;
                case Opcode::SHL:
                    a.mov(ecx, ebx);
                    a.shl(edi, cl);
                    break;
                case Opcode::SHR:
                    a.mov(ecx, ebx);
                    a.shr(edi, cl);
                    break;
                case Opcode::SSHR:
                    a.mov(ecx, ebx);
                    a.sar(edi, cl);
                    break;
                case Opcode::SHL_C_PRI:
                {
                    const uint8_t shift = MaskedShiftCount(operand);
                    if (shift != 0)
                        a.shl(edi, shift);
                    break;
                }
                case Opcode::SHL_C_ALT:
                {
                    const uint8_t shift = MaskedShiftCount(operand);
                    if (shift != 0)
                        a.shl(ebx, shift);
                    break;
                }
                case Opcode::SHR_C_PRI:
                {
                    const uint8_t shift = MaskedShiftCount(operand);
                    if (shift != 0)
                        a.shr(edi, shift);
                    break;
                }
                case Opcode::SHR_C_ALT:
                {
                    const uint8_t shift = MaskedShiftCount(operand);
                    if (shift != 0)
                        a.shr(ebx, shift);
                    break;
                }
                case Opcode::SMUL:
                case Opcode::UMUL:
                    a.imul(edi, ebx);
                    break;
                case Opcode::SDIV:
                case Opcode::SDIV_ALT:
                    EmitSignedDivide(a, instruction.opcode == Opcode::SDIV_ALT, liveness.IsLiveOut(index, RegPri), liveness.IsLiveOut(index, RegAlt), divideError);
                    break;
                case Opcode::UDIV:
                case Opcode::UDIV_ALT:
                    EmitUnsignedDivide(a, instruction.opcode == Opcode::UDIV_ALT, liveness.IsLiveOut(index, RegPri), liveness.IsLiveOut(index, RegAlt), divideError);
                    break;
                case Opcode::ADD:
                    a.add(edi, ebx);
                    break;
                case Opcode::SUB:
                    a.sub(edi, ebx);
                    break;
                case Opcode::SUB_ALT:
                    a.neg(edi);
                    a.add(edi, ebx);
                    break;
                case Opcode::AND:
                    a.and_(edi, ebx);
                    break;
                case Opcode::OR:
                    a.or_(edi, ebx);
                    break;
                case Opcode::XOR:
                    a.xor_(edi, ebx);
                    break;
                case Opcode::NOT:
                    a.test(edi, edi);
                    a.sete(al);
                    a.movzx(edi, al);
                    break;
                case Opcode::NEG:
                    a.neg(edi);
                    break;
                case Opcode::INVERT:
                    a.not_(edi);
                    break;
                case Opcode::ADD_C:
                    if (operand == 1)
                        a.inc(edi);
                    else if (operand == -1)
                        a.dec(edi);
                    else if (operand != 0)
                        a.add(edi, operand);
                    break;
                case Opcode::SMUL_C:
                    if (operand == 0)
                        a.xor_(edi, edi);
                    else if (operand == -1)
                        a.neg(edi);
                    else if (operand != 1)
                    {
                        if (IsPositivePowerOfTwo(operand))
                            a.shl(edi, PowerOfTwoShift(operand));
                        else
                            a.imul(edi, edi, operand);
                    }
                    break;
                case Opcode::ZERO_PRI:
                    a.xor_(edi, edi);
                    break;
                case Opcode::ZERO_ALT:
                    a.xor_(ebx, ebx);
                    break;
                case Opcode::ZERO:
                    a.mov(DataAbs(dataBase, operand), 0);
                    break;
                case Opcode::ZERO_S:
                    a.mov(dword_ptr(ebp, DataDisp(dataBase, operand)), 0);
                    break;
                case Opcode::SIGN_PRI:
                    a.mov(eax, edi);
                    a.movsx(edi, al);
                    break;
                case Opcode::SIGN_ALT:
                    a.mov(eax, ebx);
                    a.movsx(ebx, al);
                    break;
                case Opcode::EQ:
                case Opcode::NEQ:
                case Opcode::LESS:
                case Opcode::LEQ:
                case Opcode::GRTR:
                case Opcode::GEQ:
                case Opcode::SLESS:
                case Opcode::SLEQ:
                case Opcode::SGRTR:
                case Opcode::SGEQ:
                case Opcode::EQ_C_PRI:
                case Opcode::EQ_C_ALT:
                {
                    const Instruction* branch = index + 1 <= info.endIndex ? &decoded.instructions[index + 1] : nullptr;
                    const bool fuseBranch = branch != nullptr && branch->cip == instruction.Next() && (branch->opcode == Opcode::JZER || branch->opcode == Opcode::JNZ);
                    std::size_t fusedTargetIndex = rangeSize;
                    std::size_t fusedFallthroughIndex = rangeSize;
                    if (fuseBranch)
                    {
                        fusedTargetIndex = cloneIndexForCip(ResolveBranchTarget(amx, *branch));
                        fusedFallthroughIndex = cloneIndexForCip(branch->Next());
                    }

                    if (fuseBranch && fusedTargetIndex < rangeSize && fusedFallthroughIndex < rangeSize)
                    {
                        const std::size_t branchIndex = index + 1;
                        const std::size_t branchLocal = branchIndex - info.beginIndex;
                        const bool takenWhenTrue = branch->opcode == Opcode::JNZ;

                        /*
                            A branch-over-jump diamond can be laid out with
                            both bytecode control-transfer instructions out of
                            line: CMP + inverse Jcc(false) + materialize(true)
                            + fallthrough. This removes the hot unconditional
                            JMP completely while preserving canonical entry
                            labels for the JZER/JNZ and the intermediate JUMP
                            in cold stubs below.
                        */
                        cell diamondJumpTarget = -1;
                        if (branchIndex + 1 <= info.endIndex && MatchBranchOverJumpDiamond(amx, decoded, branchIndex, diamondJumpTarget))
                        {
                            const std::size_t jumpLocal = branchLocal + 1;
                            const std::size_t falseTargetIndex = cloneIndexForCip(diamondJumpTarget);
                            const std::size_t takenTargetIndex = cloneIndexForCip(ResolveBranchTarget(amx, *branch));
                            if (jumpLocal < rangeSize && falseTargetIndex < rangeSize && takenTargetIndex < rangeSize)
                            {
                                deferredConditionalBranches[branchLocal] = 1;
                                deferredJumpTargets[jumpLocal] = diamondJumpTarget;

                                const auto falseIt = decoded.byCip.find(diamondJumpTarget);
                                const auto takenIt = decoded.byCip.find(ResolveBranchTarget(amx, *branch));
                                const bool falseNeedsPri = falseIt == decoded.byCip.end() || liveness.IsLiveIn(falseIt->second, RegPri);
                                const bool takenNeedsPri = takenIt == decoded.byCip.end() || liveness.IsLiveIn(takenIt->second, RegPri);

                                EmitComparisonFlags(a, instruction.opcode, operand);
                                if (falseNeedsPri)
                                    a.mov(edi, takenWhenTrue ? 0 : 1);
                                EmitComparisonJump(a, instruction.opcode, cloneLabels[falseTargetIndex], !takenWhenTrue);
                                if (takenNeedsPri)
                                {
                                    if (takenWhenTrue)
                                        a.mov(edi, 1);
                                    else
                                        a.xor_(edi, edi);
                                }
                                break;
                            }
                        }

                        /*
                            for a normal comparison followed by JZER/JNZ, move
                            the canonical branch body out of line. Skipping
                            that bytecode body makes the non-taken arm the
                            native fallthrough, so no explicit JMP is required
                            on the hot path.
                        */
                        if (branchLocal < rangeSize && fusedFallthroughIndex == branchLocal + 1)
                        {
                            deferredConditionalBranches[branchLocal] = 1;
                            const auto targetIt = decoded.byCip.find(ResolveBranchTarget(amx, *branch));
                            const auto fallthroughIt = decoded.byCip.find(branch->Next());
                            const bool targetNeedsPri = targetIt == decoded.byCip.end() || liveness.IsLiveIn(targetIt->second, RegPri);
                            const bool fallthroughNeedsPri = fallthroughIt == decoded.byCip.end() || liveness.IsLiveIn(fallthroughIt->second, RegPri);

                            EmitComparisonFlags(a, instruction.opcode, operand);
                            if (targetNeedsPri)
                                a.mov(edi, takenWhenTrue ? 1 : 0);
                            EmitComparisonJump(a, instruction.opcode, cloneLabels[fusedTargetIndex], takenWhenTrue);
                            if (fallthroughNeedsPri)
                            {
                                if (takenWhenTrue)
                                    a.xor_(edi, edi);
                                else
                                    a.mov(edi, 1);
                            }
                            break;
                        }

                        /*
                            conservative fallback when the apparent
                            fallthrough is itself jump-threaded or otherwise
                            not physically adjacent.
                        */
                        const auto targetIt = decoded.byCip.find(ResolveBranchTarget(amx, *branch));
                        const auto fallthroughIt = decoded.byCip.find(branch->Next());
                        const bool targetNeedsPri = targetIt == decoded.byCip.end() || liveness.IsLiveIn(targetIt->second, RegPri);
                        const bool fallthroughNeedsPri = fallthroughIt == decoded.byCip.end() || liveness.IsLiveIn(fallthroughIt->second, RegPri);
                        EmitComparisonFlags(a, instruction.opcode, operand);
                        if (targetNeedsPri)
                            a.mov(edi, takenWhenTrue ? 1 : 0);
                        EmitComparisonJump(a, instruction.opcode, cloneLabels[fusedTargetIndex], takenWhenTrue);
                        if (fallthroughNeedsPri)
                        {
                            if (takenWhenTrue)
                                a.xor_(edi, edi);
                            else
                                a.mov(edi, 1);
                        }
                        a.jmp(cloneLabels[fusedFallthroughIndex]);
                        break;
                    }

                    EmitComparisonFlags(a, instruction.opcode, operand);
                    switch (instruction.opcode)
                    {
                        case Opcode::EQ:
                        case Opcode::EQ_C_PRI:
                        case Opcode::EQ_C_ALT:
                            a.sete(al);
                            break;
                        case Opcode::NEQ:
                            a.setne(al);
                            break;
                        case Opcode::LESS:
                            a.setb(al);
                            break;
                        case Opcode::LEQ:
                            a.setbe(al);
                            break;
                        case Opcode::GRTR:
                            a.seta(al);
                            break;
                        case Opcode::GEQ:
                            a.setae(al);
                            break;
                        case Opcode::SLESS:
                            a.setl(al);
                            break;
                        case Opcode::SLEQ:
                            a.setle(al);
                            break;
                        case Opcode::SGRTR:
                            a.setg(al);
                            break;
                        case Opcode::SGEQ:
                            a.setge(al);
                            break;
                        default:
                            break;
                    }
                    a.movzx(edi, al);
                    break;
                }
                case Opcode::INC_PRI:
                    a.inc(edi);
                    break;
                case Opcode::INC_ALT:
                    a.inc(ebx);
                    break;
                case Opcode::INC:
                    a.inc(DataAbs(dataBase, operand));
                    break;
                case Opcode::INC_S:
                    a.inc(dword_ptr(ebp, DataDisp(dataBase, operand)));
                    break;
                case Opcode::INC_I:
                    EmitCheckRawDataRange(a, edi, sizeof(cell), stp, memoryError);
                    a.inc(DataAt(edi, dataBase));
                    break;
                case Opcode::DEC_PRI:
                    a.dec(edi);
                    break;
                case Opcode::DEC_ALT:
                    a.dec(ebx);
                    break;
                case Opcode::DEC:
                    a.dec(DataAbs(dataBase, operand));
                    break;
                case Opcode::DEC_S:
                    a.dec(dword_ptr(ebp, DataDisp(dataBase, operand)));
                    break;
                case Opcode::DEC_I:
                    EmitCheckRawDataRange(a, edi, sizeof(cell), stp, memoryError);
                    a.dec(DataAt(edi, dataBase));
                    break;
                case Opcode::MOVS:
                    /*
                        keep clone semantics tied to the same helper as the
                        main backend. EDI/EBX are callee-saved on the x86 host
                        ABI and JitMoveMemory does not modify context PRI/ALT,
                        so no reload is needed after a successful helper call.
                    */
                    EmitFlushPriAlt(a);
                    EmitEnterHostStack(a, dataBase, 8);
                    a.push(operand);
                    a.push(esi);
                    a.mov(eax, Pointer32(&JitMoveMemory));
                    a.call(eax);
                    a.add(esp, 8);
                    EmitLeaveHostStack(a, dataBase);
                    EmitHelperErrorCheck(a, exitLabel);
                    break;
                case Opcode::CMPS:
                    EmitFlushPriAlt(a);
                    EmitEnterHostStack(a, dataBase, 8);
                    a.push(operand);
                    a.push(esi);
                    a.mov(eax, Pointer32(&JitCompareMemory));
                    a.call(eax);
                    a.add(esp, 8);
                    EmitLeaveHostStack(a, dataBase);
                    EmitHelperErrorCheck(a, exitLabel);
                    if (liveness.IsLiveOut(index, RegPri))
                        a.mov(edi, Ctx(offsetof(JitContext, pri)));
                    break;
                case Opcode::FILL:
                    EmitFlushPriAlt(a);
                    EmitEnterHostStack(a, dataBase, 8);
                    a.push(operand);
                    a.push(esi);
                    a.mov(eax, Pointer32(&JitFillMemory));
                    a.call(eax);
                    a.add(esp, 8);
                    EmitLeaveHostStack(a, dataBase);
                    EmitHelperErrorCheck(a, exitLabel);
                    break;
                case Opcode::BOUNDS:
                    a.mov(Ctx(offsetof(JitContext, cip)), instruction.Next());
                    if (static_cast<ucell>(operand) != std::numeric_limits<ucell>::max())
                    {
                        a.cmp(edi, operand);
                        a.ja(boundsError);
                    }
                    break;
                case Opcode::SWAP_PRI:
                    EmitCheckRawStackCell(a, dataBase, stp, memoryError);
                    if (liveness.IsLiveOut(index, RegPri))
                    {
                        a.mov(eax, dword_ptr(esp));
                        a.mov(dword_ptr(esp), edi);
                        a.mov(edi, eax);
                    }
                    else
                    {
                        a.mov(dword_ptr(esp), edi);
                    }
                    break;
                case Opcode::SWAP_ALT:
                    EmitCheckRawStackCell(a, dataBase, stp, memoryError);
                    if (liveness.IsLiveOut(index, RegAlt))
                    {
                        a.mov(eax, dword_ptr(esp));
                        a.mov(dword_ptr(esp), ebx);
                        a.mov(ebx, eax);
                    }
                    else
                    {
                        a.mov(dword_ptr(esp), ebx);
                    }
                    break;
                case Opcode::PUSH_ADR:
                    a.lea(eax, ptr(ebp, operand));
                    EmitPushEax(a, dataBase, stackError);
                    break;
                case Opcode::BREAK:
                {
                    /*
                        BREAK is emitted by debug-info Pawn builds even for
                        otherwise tiny straight-line functions. If a debugger
                        is already installed, skip the redundant outer null
                        test: JitDebugBreak still reads AMX::debug live,
                        preserving same-exec debugger removal. When no hook is
                        installed at compile time, retain the live probe so a
                        native can install one before a later BREAK in the
                        same execution.
                    */
                    if (amx->debug != nullptr)
                    {
                        EmitDebugBreakCall(a, dataBase, amxAddress, instruction.Next(), exitLabel);
                    }
                    else
                    {
                        asmjit::Label skipDebug = a.new_label();
                        a.cmp(AmxAbs(amxAddress, offsetof(AMX, debug)), 0);
                        a.je(skipDebug);
                        EmitDebugBreakCall(a, dataBase, amxAddress, instruction.Next(), exitLabel);
                        a.bind(skipDebug);
                    }
                    break;
                }
                case Opcode::LINE:
                case Opcode::SYMBOL:
                case Opcode::SRANGE:
                case Opcode::SYMTAG:
                case Opcode::NOP:
                    break;
                default:
                    /*
                        eligibility/emission drift must never create a
                        silently truncated inline clone. Fail
                        deterministically instead.
                    */
                    a.jmp(invalidInstruction);
                    return;
            }
        }

        bool hasDeferredCold = false;
        for (std::size_t localIndex = 0; localIndex < rangeSize; ++localIndex)
        {
            if (deferredJumpTargets[localIndex] >= 0 || deferredConditionalBranches[localIndex] != 0)
            {
                hasDeferredCold = true;
                break;
            }
        }

        if (hasDeferredCold)
        {
            /*
                normal inline flow must not fall into out-of-line canonical
                control-flow stubs. Internal branches that target one of the
                original bytecode labels still execute its exact semantics
                here.
            */
            a.jmp(cloneDone);
            for (std::size_t localIndex = 0; localIndex < rangeSize; ++localIndex)
            {
                const cell jumpTarget = deferredJumpTargets[localIndex];
                if (jumpTarget >= 0)
                {
                    const std::size_t targetIndex = cloneIndexForCip(jumpTarget);
                    a.bind(cloneLabels[localIndex]);
                    if (targetIndex >= rangeSize)
                        a.jmp(invalidInstruction);
                    else
                        a.jmp(cloneLabels[targetIndex]);
                    continue;
                }

                if (deferredConditionalBranches[localIndex] == 0)
                    continue;

                const std::size_t instructionIndex = info.beginIndex + localIndex;
                const Instruction& branch = decoded.instructions[instructionIndex];
                const std::size_t targetIndex = cloneIndexForCip(ResolveBranchTarget(amx, branch));
                const std::size_t nextIndex = cloneIndexForCip(branch.Next());
                a.bind(cloneLabels[localIndex]);
                if (targetIndex >= rangeSize || nextIndex >= rangeSize)
                {
                    a.jmp(invalidInstruction);
                    continue;
                }
                if (targetIndex == nextIndex)
                {
                    a.jmp(cloneLabels[nextIndex]);
                    continue;
                }
                a.test(edi, edi);
                if (branch.opcode == Opcode::JZER)
                    a.jz(cloneLabels[targetIndex]);
                else
                    a.jnz(cloneLabels[targetIndex]);
                a.jmp(cloneLabels[nextIndex]);
            }
        }

        a.bind(cloneDone);
    }

    bool ValidateProgram(AMX* amx, const DecodedProgram& decoded)
    {
        for (const Instruction& instruction : decoded.instructions)
        {
            switch (instruction.opcode)
            {
                case Opcode::LOAD_PRI:
                case Opcode::LOAD_ALT:
                case Opcode::LREF_PRI:
                case Opcode::LREF_ALT:
                case Opcode::STOR_PRI:
                case Opcode::STOR_ALT:
                case Opcode::SREF_PRI:
                case Opcode::SREF_ALT:
                case Opcode::PUSH:
                case Opcode::ZERO:
                case Opcode::INC:
                case Opcode::DEC:
                    if (!StaticAddressValid(amx, instruction.operands[0], sizeof(cell)))
                    {
                        return false;
                    }
                    break;

                case Opcode::LOAD_S_PRI:
                case Opcode::LOAD_S_ALT:
                case Opcode::LREF_S_PRI:
                case Opcode::LREF_S_ALT:
                case Opcode::STOR_S_PRI:
                case Opcode::STOR_S_ALT:
                case Opcode::SREF_S_PRI:
                case Opcode::SREF_S_ALT:
                case Opcode::PUSH_S:
                case Opcode::ZERO_S:
                case Opcode::INC_S:
                case Opcode::DEC_S:
                    if (!FrameOperandValid(amx, instruction.operands[0]))
                    {
                        return false;
                    }
                    break;

                case Opcode::CALL:
                case Opcode::JUMP:
                case Opcode::JREL:
                case Opcode::JZER:
                case Opcode::JNZ:
                case Opcode::JEQ:
                case Opcode::JNEQ:
                case Opcode::JLESS:
                case Opcode::JLEQ:
                case Opcode::JGRTR:
                case Opcode::JGEQ:
                case Opcode::JSLESS:
                case Opcode::JSLEQ:
                case Opcode::JSGRTR:
                case Opcode::JSGEQ:
                {
                    const cell target = ResolveBranchTarget(amx, instruction);

                    if (!HasInstruction(decoded, target))
                    {
                        return false;
                    }

                    break;
                }

                case Opcode::SWITCH:
                {
                    const cell tableCip = NormalizeCodeTarget(amx, instruction.operands[0]);
                    const Instruction* table = FindInstruction(decoded, tableCip);

                    if (table == nullptr || table->opcode != Opcode::CASETBL || table->operands.size() < 2)
                    {
                        return false;
                    }

                    const cell count = table->operands[0];

                    if (count < 0 || table->operands.size() != static_cast<std::size_t>((count + 1) * 2))
                    {
                        return false;
                    }

                    if (!HasInstruction(decoded, NormalizeCodeTarget(amx, table->operands[1])))
                    {
                        return false;
                    }

                    for (cell i = 0; i < count; ++i)
                    {
                        const std::size_t targetIndex = 3 + static_cast<std::size_t>(i) * 2;

                        if (!HasInstruction(decoded, NormalizeCodeTarget(amx, table->operands[targetIndex])))
                        {
                            return false;
                        }
                    }

                    break;
                }

                case Opcode::PUSH_R:
                    if (instruction.operands[0] < 0 || instruction.operands[0] > std::numeric_limits<int>::max() / static_cast<cell>(sizeof(cell)))
                    {
                        return false;
                    }
                    break;

                case Opcode::FILE:
                case Opcode::LINE:
                case Opcode::SYMBOL:
                case Opcode::SRANGE:
                case Opcode::SYMTAG:
                    break;

                default:
                    break;
            }
        }

        return true;
    }
}

bool CompileProgram(AMX* amx, CompiledProgram& program)
{
    try
    {
        using namespace asmjit;
        using namespace asmjit::x86;

        DecodedProgram decoded;

        if (!DecodeProgram(amx, decoded))
            return false;

        if (!ValidateProgram(amx, decoded))
            return false;

        const ControlFlowGraph cfg = BuildControlFlowGraph(amx, decoded);
        CallGraphSummary callGraph = BuildCallGraphSummary(amx, decoded);
        AugmentCallGraphWithConstantIndirectCalls(amx, decoded, callGraph);
        const FunctionInliningSummary inlining = BuildFunctionInliningSummary(amx, decoded, callGraph);
        const ControlFlowGraph loopCfg = BuildLoopControlFlowGraph(amx, decoded);
        const LoopAnalysis loops = BuildLoopAnalysis(loopCfg);

        /*
        canonical callees never observe return addresses from call sites that
        codegen replaces with frame-preserving inline clones.  Remove those
        impossible PIC entries, then prioritize loop callers so the fixed-size
        return PIC spends its comparisons on the most frequently executed
        known sites.  Unknown/omitted callers still fall through to the
        generic return resolver, so this only changes specialization order and
        code size.
    */
        for (auto& entry : callGraph.directReturnCipsByFunction)
        {
            const cell functionEntry = entry.first;
            std::vector<cell>& returns = entry.second;

            returns.erase(std::remove_if(returns.begin(), returns.end(),
                                         [&](cell returnCip) {
                                             const auto nextIt = decoded.byCip.find(returnCip);
                                             if (nextIt == decoded.byCip.end() || nextIt->second == 0)
                                                 return false;
                                             const std::size_t transferIndex = nextIt->second - 1;
                                             if (transferIndex >= decoded.instructions.size() || decoded.instructions[transferIndex].Next() != returnCip ||
                                                 transferIndex >= inlining.targetByCallInstruction.size())
                                             {
                                                 return false;
                                             }
                                             return inlining.targetByCallInstruction[transferIndex] == functionEntry;
                                         }),
                          returns.end());

            auto loopPriority = [&](cell returnCip) {
                const auto nextIt = decoded.byCip.find(returnCip);
                if (nextIt == decoded.byCip.end() || nextIt->second == 0)
                    return 0;
                const std::size_t transferIndex = nextIt->second - 1;
                if (transferIndex >= decoded.instructions.size() || decoded.instructions[transferIndex].Next() != returnCip)
                {
                    return 0;
                }
                return loops.IsLoopInstruction(transferIndex) ? 1 : 0;
            };

            std::stable_sort(returns.begin(), returns.end(), [&](cell lhs, cell rhs) {
                return loopPriority(lhs) > loopPriority(rhs);
            });
        }

        const LoopMemorySummary loopMemory = BuildLoopMemorySummary(amx, decoded, loops);
        const LoopRangeAnalysis loopRanges = BuildLoopRangeAnalysis(amx, decoded, loopCfg, loops, loopMemory);
        const LocalLiveness liveness = BuildLocalLiveness(decoded, cfg);

        unsigned char* data = ResolveDataBase(amx);

        if (data == nullptr)
        {
            return false;
        }

        const uint32_t dataBase = Pointer32(data);
        const uint32_t amxAddress = Pointer32(amx);
        const cell hlw = amx->hlw;
        const cell stp = amx->stp;

        const std::size_t cellCount = static_cast<std::size_t>(decoded.codeSize / static_cast<cell>(sizeof(cell)));
        const uint32_t dispatchCount = static_cast<uint32_t>(cellCount);
        std::vector<void*> dispatchTable(cellCount, nullptr);
        const uint32_t dispatchAddress = Pointer32(dispatchTable.data());

        CodeHolder code;
        Error asmError = code.init(g_jitRuntime.environment());

        if (asmError != Error::kOk)
        {
            return false;
        }

        Assembler a(&code);

        std::vector<Label> labels;
        labels.reserve(cellCount);

        for (std::size_t i = 0; i < cellCount; ++i)
            labels.push_back(a.new_label());

        std::vector<std::size_t> nativeOffsets(cellCount, std::numeric_limits<std::size_t>::max());
        std::vector<cell> deferredJumpTargets(decoded.instructions.size(), static_cast<cell>(-1));
        std::vector<uint8_t> deferredConditionalBranches(decoded.instructions.size(), 0);

        Label dispatchLabel = a.new_label();
        Label exitLabel = a.new_label();
        Label invalidAddress = a.new_label();
        Label invalidInstruction = a.new_label();
        Label memoryError = a.new_label();
        Label stackError = a.new_label();
        Label stackLow = a.new_label();
        Label heapLow = a.new_label();
        Label boundsError = a.new_label();
        Label divideError = a.new_label();

        constexpr cell kUncachedThreadedTarget = std::numeric_limits<cell>::min();
        std::vector<cell> threadedCodegenTargetByInstruction(decoded.instructions.size(), kUncachedThreadedTarget);

        auto resolveCodegenTarget = [&](cell target) {
            const auto targetIt = decoded.byCip.find(target);
            if (targetIt == decoded.byCip.end() || targetIt->second >= threadedCodegenTargetByInstruction.size())
            {
                return ResolveThreadedDirectTarget(amx, decoded, target).target;
            }

            cell& cached = threadedCodegenTargetByInstruction[targetIt->second];
            if (cached == kUncachedThreadedTarget)
                cached = ResolveThreadedDirectTarget(amx, decoded, target).target;
            return cached;
        };

        a.push(ebx);
        a.push(edi);
        a.push(ebp);
        a.push(esi);
        a.mov(esi, dword_ptr(esp, 20));
        a.mov(Ctx(offsetof(JitContext, hostEsp)), esp);
        a.mov(eax, esp);
        a.and_(eax, -16);
        a.mov(Ctx(offsetof(JitContext, hostCallEsp16)), eax);
        a.lea(ecx, ptr(eax, -8));
        a.mov(Ctx(offsetof(JitContext, hostCallEsp8)), ecx);
        a.lea(ecx, ptr(eax, -12));
        a.mov(Ctx(offsetof(JitContext, hostCallEsp4)), ecx);
        EmitReloadVmRegisters(a, dataBase);

        /*
        fresh main/public/nested calls need the synthetic top-level return and
        parameter-byte cells. Write them only after entering generated code so
        malformed external AMX data pointers are covered by the CPU-fault
        guard.
    */
        Label noSyntheticFrame = a.new_label();
        a.cmp(Ctx(offsetof(JitContext, syntheticFrame)), 0);
        a.je(noSyntheticFrame);
        a.mov(dword_ptr(esp), kTopReturn);
        a.mov(eax, Ctx(offsetof(JitContext, entryParameterBytes)));
        a.mov(dword_ptr(esp, static_cast<int32_t>(sizeof(cell))), eax);
        a.bind(noSyntheticFrame);

        a.jmp(dispatchLabel);

        for (std::size_t instructionIndex = 0; instructionIndex < decoded.instructions.size(); ++instructionIndex)
        {
            if (deferredJumpTargets[instructionIndex] >= 0 || deferredConditionalBranches[instructionIndex] != 0)
            {
                continue;
            }

            const Instruction& instruction = decoded.instructions[instructionIndex];
            const std::size_t slot = static_cast<std::size_t>(instruction.cip / sizeof(cell));

            a.bind(labels[slot]);
            nativeOffsets[slot] = a.offset();
            if (NeedsCurrentCip(instruction.opcode))
                a.mov(Ctx(offsetof(JitContext, cip)), instruction.cip);

            const cell operand = instruction.operands.empty() ? 0 : instruction.operands[0];

            if (instructionIndex < callGraph.devirtualizedCallTargetByInstruction.size())
            {
                const cell devirtualizedTarget = callGraph.devirtualizedCallTargetByInstruction[instructionIndex];
                if (devirtualizedTarget >= 0 && instruction.opcode == Opcode::CONST_PRI && instructionIndex + 1 < decoded.instructions.size())
                {
                    const Instruction& call = decoded.instructions[instructionIndex + 1];
                    if (call.opcode == Opcode::CALL_PRI && call.cip == instruction.Next())
                    {
                        /*
                        preserve CALL.PRI's architectural PRI value while
                        replacing its dynamic dispatch with the already proven
                        static target.
                    */
                        if (operand == 0)
                            a.xor_(edi, edi);
                        else
                            a.mov(edi, operand);
                        /*
                        the fused path executes CALL.PRI's stack check, so
                        make its bytecode CIP visible on the same error path
                        as the unfused instruction. This matters with lazy-CIP
                        enabled.
                    */
                        a.mov(Ctx(offsetof(JitContext, cip)), call.cip);
                        EmitCheckPushSpace(a, dataBase, static_cast<int>(sizeof(cell)), stackError);
                        a.push(EncodeReturnAddress(call.Next()));

                        if (instructionIndex + 1 < inlining.targetByCallInstruction.size() && inlining.targetByCallInstruction[instructionIndex + 1] == devirtualizedTarget)
                        {
                            EmitInlineFunctionClone(a, amx, decoded, inlining, liveness, devirtualizedTarget, call.Next(), dataBase, dispatchAddress, invalidAddress,
                                                    invalidInstruction, memoryError, stackError, stackLow, heapLow, boundsError, divideError, exitLabel);
                            /*
                            the fused sequence starts at CONST.pri, so the
                            next native code label emitted by the outer loop
                            is CALL.pri itself. Skip that canonical cold-entry
                            label after a successful inline return or the CALL
                            would execute a second time.
                        */
                            a.jmp(labels[static_cast<std::size_t>(call.Next() / sizeof(cell))]);
                            continue;
                        }

                        a.jmp(labels[static_cast<std::size_t>(devirtualizedTarget / sizeof(cell))]);
                        continue;
                    }
                }
            }

            ConstantIndirectTransferBlock constantIndirectBlock;
            if (BuildConstantIndirectTransferBlock(amx, decoded, instructionIndex, constantIndirectBlock))
            {
                /*
                fold a straight-line constant PRI computation into a direct
                CALL.pri/JUMP.pri target.  Intermediate bytecode labels are
                still emitted normally later, so dynamic/cold entry into the
                middle of the sequence retains canonical semantics.
            */
                if (constantIndirectBlock.state.priKnown)
                    EmitKnownConstant(a, edi, constantIndirectBlock.state.pri);
                if (constantIndirectBlock.state.altKnown)
                    EmitKnownConstant(a, ebx, constantIndirectBlock.state.alt);

                const Instruction& transfer = decoded.instructions[constantIndirectBlock.transferIndex];

                if (constantIndirectBlock.kind == ConstantIndirectTransferKind::Native)
                {
                    /*
                    SYSREQ.pri with a proven constant index is equivalent to
                    the immediate-index callback path once PRI itself has been
                    materialized. This removes dynamic-index spill/reload
                    traffic.
                */
                    EmitCallbackNativeCall(a, dataBase, amxAddress, stp, false, constantIndirectBlock.state.pri, transfer.Next(), false,
                                           liveness.IsLiveOut(constantIndirectBlock.transferIndex, RegPri), memoryError, exitLabel);
                    /*
                    this fused path can consume multiple bytecode
                    instructions. Continue at the instruction after SYSREQ.pri
                    instead of falling into the canonical labels of the
                    consumed sequence.
                */
                    a.jmp(labels[static_cast<std::size_t>(transfer.Next() / sizeof(cell))]);
                    continue;
                }

                const std::size_t targetSlot = static_cast<std::size_t>(constantIndirectBlock.targetCip / sizeof(cell));

                if (constantIndirectBlock.kind == ConstantIndirectTransferKind::Call)
                {
                    /*
                    match CALL.pri's error-visible CIP before its stack check.
                */
                    a.mov(Ctx(offsetof(JitContext, cip)), transfer.cip);
                    EmitCheckPushSpace(a, dataBase, static_cast<int>(sizeof(cell)), stackError);
                    a.push(EncodeReturnAddress(transfer.Next()));

                    /*
                    once constant-flow analysis has proven the CALL.pri
                    target, it can use the exact same frame-preserving inline
                    clone as a direct CALL. PRI/ALT were materialized above,
                    so architectural register state at callee entry stays
                    identical to bytecode.
                */
                    if (constantIndirectBlock.transferIndex < inlining.targetByCallInstruction.size() &&
                        inlining.targetByCallInstruction[constantIndirectBlock.transferIndex] == constantIndirectBlock.targetCip)
                    {
                        EmitInlineFunctionClone(a, amx, decoded, inlining, liveness, constantIndirectBlock.targetCip, transfer.Next(), dataBase, dispatchAddress, invalidAddress,
                                                invalidInstruction, memoryError, stackError, stackLow, heapLow, boundsError, divideError, exitLabel);
                        /*
                        as above, skip every canonical label consumed by the
                        fused constant-flow sequence after the inline callee
                        returns.
                    */
                        a.jmp(labels[static_cast<std::size_t>(transfer.Next() / sizeof(cell))]);
                        continue;
                    }
                }

                a.jmp(labels[targetSlot]);
                continue;
            }

            LoopInductionCompareBranchBlock loopInductionBlock;
            if (BuildLoopInductionCompareBranchBlock(amx, decoded, loops, liveness, instructionIndex, loopInductionBlock))
            {
                const cell taken = resolveCodegenTarget(loopInductionBlock.takenCip);
                const cell fallthrough = resolveCodegenTarget(loopInductionBlock.fallthroughCip);
                EmitLoopInductionCompareBranch(a, loopInductionBlock, dataBase, labels[static_cast<std::size_t>(taken / sizeof(cell))],
                                               labels[static_cast<std::size_t>(fallthrough / sizeof(cell))]);
                continue;
            }

            LoopMemoryCompareBranchBlock loopMemoryCompareBlock;
            if (BuildLoopMemoryCompareBranchBlock(amx, decoded, loops, liveness, instructionIndex, loopMemoryCompareBlock))
            {
                const cell taken = resolveCodegenTarget(loopMemoryCompareBlock.takenCip);
                const cell fallthrough = resolveCodegenTarget(loopMemoryCompareBlock.fallthroughCip);
                EmitLoopMemoryCompareBranch(a, loopMemoryCompareBlock, dataBase, labels[static_cast<std::size_t>(taken / sizeof(cell))],
                                            labels[static_cast<std::size_t>(fallthrough / sizeof(cell))]);
                continue;
            }

            LoopBoundsIndexBlock loopBoundsIndexBlock;
            if (BuildLoopBoundsIndexBlock(decoded, loops, loopRanges, liveness, instructionIndex, loopBoundsIndexBlock))
            {
                EmitLoopBoundsIndex(a, loopBoundsIndexBlock, dataBase, stp, boundsError, memoryError);
                const cell target = resolveCodegenTarget(loopBoundsIndexBlock.targetCip);
                a.jmp(labels[static_cast<std::size_t>(target / sizeof(cell))]);
                continue;
            }

            LoopBoundsAddressBlock loopBoundsAddressBlock;
            if (BuildLoopBoundsAddressBlock(decoded, loops, loopRanges, liveness, instructionIndex, loopBoundsAddressBlock))
            {
                EmitLoopBoundsAddress(a, loopBoundsAddressBlock, dataBase, boundsError);
                const cell target = resolveCodegenTarget(loopBoundsAddressBlock.targetCip);
                a.jmp(labels[static_cast<std::size_t>(target / sizeof(cell))]);
                continue;
            }

            LoopCounterBranchBlock loopCounterBlock;
            if (BuildLoopCounterBranchBlock(amx, decoded, loops, liveness, instructionIndex, loopCounterBlock))
            {
                const cell taken = resolveCodegenTarget(loopCounterBlock.takenCip);
                const cell fallthrough = resolveCodegenTarget(loopCounterBlock.fallthroughCip);
                EmitLoopCounterBranch(a, loopCounterBlock, dataBase, labels[static_cast<std::size_t>(taken / sizeof(cell))],
                                      labels[static_cast<std::size_t>(fallthrough / sizeof(cell))]);
                continue;
            }

            LoopCompareBranchBlock loopCompareBlock;
            if (BuildLoopCompareBranchBlock(amx, decoded, loops, liveness, instructionIndex, loopCompareBlock))
            {
                const cell taken = resolveCodegenTarget(loopCompareBlock.takenCip);
                const cell fallthrough = resolveCodegenTarget(loopCompareBlock.fallthroughCip);
                EmitLoopCompareBranch(a, loopCompareBlock, dataBase, labels[static_cast<std::size_t>(taken / sizeof(cell))],
                                      labels[static_cast<std::size_t>(fallthrough / sizeof(cell))]);
                continue;
            }

            LoopRmwBlock loopRmwBlock;
            if (BuildLoopRmwBlock(decoded, loops, liveness, instructionIndex, loopRmwBlock))
            {
                EmitLoopRmwOperation(a, loopRmwBlock, dataBase);
                const cell target = resolveCodegenTarget(loopRmwBlock.targetCip);
                a.jmp(labels[static_cast<std::size_t>(target / sizeof(cell))]);
                continue;
            }

            ConstantSuperblock constantBlock;
            if (BuildConstantSuperblock(amx, decoded, cfg, liveness, instructionIndex, constantBlock))
            {
                if (constantBlock.state.priKnown && constantBlock.materializePri)
                    EmitKnownConstant(a, edi, constantBlock.state.pri);

                if (constantBlock.state.altKnown && constantBlock.materializeAlt)
                    EmitKnownConstant(a, ebx, constantBlock.state.alt);

                const std::size_t targetSlot = static_cast<std::size_t>(constantBlock.targetCip / sizeof(cell));
                a.jmp(labels[targetSlot]);

                continue;
            }

            ValueSuperblock valueBlock;
            if (BuildValueSuperblock(amx, decoded, cfg, liveness, instructionIndex, valueBlock))
            {
                EmitSymbolicState(a, valueBlock);

                const std::size_t targetSlot = static_cast<std::size_t>(valueBlock.targetCip / sizeof(cell));
                a.jmp(labels[targetSlot]);

                continue;
            }

            MemoryMergeBlock memoryMergeBlock;
            if (BuildMemoryMergeBlock(amx, decoded, cfg, liveness, instructionIndex, memoryMergeBlock))
            {
                if (memoryMergeBlock.preserveBranch)
                {
                    const asmjit::Label taken = a.new_label();
                    const asmjit::Label merged = a.new_label();
                    EmitMemoryMergeBranch(a, instruction.opcode, taken);

                    for (const MemoryValueStoreAction& action : memoryMergeBlock.fallthroughStores)
                        EmitMemoryValueStore(a, dataBase, action, memoryError);
                    a.jmp(merged);

                    a.bind(taken);
                    for (const MemoryValueStoreAction& action : memoryMergeBlock.takenStores)
                        EmitMemoryValueStore(a, dataBase, action, memoryError);
                    a.bind(merged);
                }

                for (const MemoryValueStoreAction& action : memoryMergeBlock.stores)
                    EmitMemoryValueStore(a, dataBase, action, memoryError);

                EmitMemoryMergeState(a, memoryMergeBlock);
                const std::size_t targetSlot = static_cast<std::size_t>(memoryMergeBlock.targetCip / sizeof(cell));
                a.jmp(labels[targetSlot]);

                continue;
            }

            if (IsRedundantDirectStore(decoded, cfg, instructionIndex))
            {
                continue;
            }

            MemoryValueBlock memoryValueBlock;
            if (BuildMemoryValueBlock(amx, decoded, cfg, liveness, instructionIndex, memoryValueBlock))
            {
                for (const MemoryValueStoreAction& action : memoryValueBlock.stores)
                    EmitMemoryValueStore(a, dataBase, action, memoryError);

                EmitMemoryValueState(a, memoryValueBlock);
                const std::size_t targetSlot = static_cast<std::size_t>(memoryValueBlock.targetCip / sizeof(cell));
                a.jmp(labels[targetSlot]);

                continue;
            }

            StoreForwardBlock storeForwardBlock;
            if (BuildStoreForwardBlock(amx, decoded, cfg, liveness, instructionIndex, storeForwardBlock))
            {
                switch (instruction.opcode)
                {
                    case Opcode::STOR_PRI:
                        a.mov(DataAbs(dataBase, operand), edi);
                        break;
                    case Opcode::STOR_ALT:
                        a.mov(DataAbs(dataBase, operand), ebx);
                        break;
                    case Opcode::STOR_S_PRI:
                        a.mov(dword_ptr(ebp, DataDisp(dataBase, operand)), edi);
                        break;
                    case Opcode::STOR_S_ALT:
                        a.mov(dword_ptr(ebp, DataDisp(dataBase, operand)), ebx);
                        break;
                    case Opcode::ZERO:
                        a.mov(DataAbs(dataBase, operand), 0);
                        break;
                    case Opcode::ZERO_S:
                        a.mov(dword_ptr(ebp, DataDisp(dataBase, operand)), 0);
                        break;
                    default:
                        break;
                }

                EmitStoreForwardState(a, storeForwardBlock);
                const std::size_t targetSlot = static_cast<std::size_t>(storeForwardBlock.targetCip / sizeof(cell));
                a.jmp(labels[targetSlot]);

                continue;
            }

            if (liveness.IsDeadPure(instructionIndex))
            {
                continue;
            }

            switch (instruction.opcode)
            {
                case Opcode::LOAD_PRI:
                    a.mov(edi, DataAbs(dataBase, operand));
                    break;

                case Opcode::LOAD_ALT:
                    a.mov(ebx, DataAbs(dataBase, operand));
                    break;

                case Opcode::LOAD_S_PRI:
                    a.mov(edi, dword_ptr(ebp, DataDisp(dataBase, operand)));
                    break;

                case Opcode::LOAD_S_ALT:
                    a.mov(ebx, dword_ptr(ebp, DataDisp(dataBase, operand)));
                    break;

                case Opcode::LOAD_BOTH:
                {
                    const bool priLive = liveness.IsLiveOut(instructionIndex, RegPri);
                    const bool altLive = liveness.IsLiveOut(instructionIndex, RegAlt);
                    if (priLive && altLive && instruction.operands[0] == instruction.operands[1])
                    {
                        a.mov(edi, DataAbs(dataBase, instruction.operands[0]));
                        a.mov(ebx, edi);
                    }
                    else
                    {
                        if (priLive)
                            a.mov(edi, DataAbs(dataBase, instruction.operands[0]));
                        if (altLive)
                            a.mov(ebx, DataAbs(dataBase, instruction.operands[1]));
                    }
                    break;
                }

                case Opcode::LOAD_S_BOTH:
                {
                    const bool priLive = liveness.IsLiveOut(instructionIndex, RegPri);
                    const bool altLive = liveness.IsLiveOut(instructionIndex, RegAlt);
                    if (priLive && altLive && instruction.operands[0] == instruction.operands[1])
                    {
                        a.mov(edi, dword_ptr(ebp, DataDisp(dataBase, instruction.operands[0])));
                        a.mov(ebx, edi);
                    }
                    else
                    {
                        if (priLive)
                            a.mov(edi, dword_ptr(ebp, DataDisp(dataBase, instruction.operands[0])));
                        if (altLive)
                            a.mov(ebx, dword_ptr(ebp, DataDisp(dataBase, instruction.operands[1])));
                    }
                    break;
                }

                case Opcode::LREF_PRI:
                    a.mov(edx, DataAbs(dataBase, operand));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(edi, DataAt(edx, dataBase));
                    break;

                case Opcode::LREF_ALT:
                    a.mov(edx, DataAbs(dataBase, operand));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(ebx, DataAt(edx, dataBase));
                    break;

                case Opcode::LREF_S_PRI:
                    a.lea(edx, ptr(ebp, operand));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(edx, DataAt(edx, dataBase));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(edi, DataAt(edx, dataBase));
                    break;

                case Opcode::LREF_S_ALT:
                    a.lea(edx, ptr(ebp, operand));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(edx, DataAt(edx, dataBase));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(ebx, DataAt(edx, dataBase));
                    break;

                case Opcode::LOAD_I:
                    EmitCheckAddress(a, dataBase, edi, sizeof(cell), stp, memoryError);
                    a.mov(edi, DataAt(edi, dataBase));
                    break;

                case Opcode::LODB_I:
                    /*
                    LOAD.I/LODB.I validate only the starting AMX address.
                    Portable C uses a strict 1/2/4 switch with a no-op
                    default.
                */
                    EmitCheckAddress(a, dataBase, edi, 1, stp, memoryError);

                    if (operand == 1)
                    {
                        a.movzx(edi, byte_ptr(edi, DataDisp(dataBase)));
                    }
                    else if (operand == 2)
                    {
                        a.movzx(edi, word_ptr(edi, DataDisp(dataBase)));
                    }
                    else if (operand == 4)
                    {
                        a.mov(edi, DataAt(edi, dataBase));
                    }
                    break;

                case Opcode::CONST_PRI:
                    if (operand == 0)
                    {
                        a.xor_(edi, edi);
                    }
                    else
                    {
                        a.mov(edi, operand);
                    }
                    break;

                case Opcode::CONST_ALT:
                    if (operand == 0)
                    {
                        a.xor_(ebx, ebx);
                    }
                    else
                    {
                        a.mov(ebx, operand);
                    }
                    break;

                case Opcode::ADDR_PRI:
                    a.lea(edi, ptr(ebp, operand));
                    break;

                case Opcode::ADDR_ALT:
                    a.lea(ebx, ptr(ebp, operand));
                    break;

                case Opcode::STOR_PRI:
                    a.mov(DataAbs(dataBase, operand), edi);
                    break;

                case Opcode::STOR_ALT:
                    a.mov(DataAbs(dataBase, operand), ebx);
                    break;

                case Opcode::STOR_S_PRI:
                    a.mov(dword_ptr(ebp, DataDisp(dataBase, operand)), edi);
                    break;

                case Opcode::STOR_S_ALT:
                    a.mov(dword_ptr(ebp, DataDisp(dataBase, operand)), ebx);
                    break;

                case Opcode::CONST_OP:
                    a.mov(DataAbs(dataBase, instruction.operands[0]), instruction.operands[1]);
                    break;

                case Opcode::CONST_S:
                    a.mov(dword_ptr(ebp, DataDisp(dataBase, instruction.operands[0])), instruction.operands[1]);
                    break;

                case Opcode::SREF_PRI:
                    a.mov(edx, DataAbs(dataBase, operand));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(DataAt(edx, dataBase), edi);
                    break;

                case Opcode::SREF_ALT:
                    a.mov(edx, DataAbs(dataBase, operand));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(DataAt(edx, dataBase), ebx);
                    break;

                case Opcode::SREF_S_PRI:
                    a.lea(edx, ptr(ebp, operand));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(edx, DataAt(edx, dataBase));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(DataAt(edx, dataBase), edi);
                    break;

                case Opcode::SREF_S_ALT:
                    a.lea(edx, ptr(ebp, operand));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(edx, DataAt(edx, dataBase));
                    EmitCheckRawDataRange(a, edx, sizeof(cell), stp, memoryError);
                    a.mov(DataAt(edx, dataBase), ebx);
                    break;

                case Opcode::STOR_I:
                    EmitCheckAddress(a, dataBase, ebx, sizeof(cell), stp, memoryError);
                    a.mov(DataAt(ebx, dataBase), edi);
                    break;

                case Opcode::STRB_I:
                    EmitCheckAddress(a, dataBase, ebx, 1, stp, memoryError);

                    /*
                    current portable C runtime has a strict 1/2/4 switch and a
                    no-op default after validating ALT.
                */
                    if (operand == 1)
                    {
                        a.mov(eax, edi);
                        a.mov(byte_ptr(ebx, DataDisp(dataBase)), al);
                    }
                    else if (operand == 2)
                    {
                        a.mov(eax, edi);
                        a.mov(word_ptr(ebx, DataDisp(dataBase)), ax);
                    }
                    else if (operand == 4)
                    {
                        a.mov(DataAt(ebx, dataBase), edi);
                    }

                    break;

                case Opcode::LIDX:
                    a.lea(edi, ptr(ebx, edi, 2));
                    EmitCheckAddress(a, dataBase, edi, sizeof(cell), stp, memoryError);
                    a.mov(edi, DataAt(edi, dataBase));
                    break;

                case Opcode::LIDX_B:
                {
                    const uint32_t shift = static_cast<uint32_t>(static_cast<ucell>(operand)) & 31u;
                    if (shift <= 3u)
                    {
                        a.lea(edi, ptr(ebx, edi, shift));
                    }
                    else
                    {
                        a.shl(edi, static_cast<uint8_t>(shift));
                        a.add(edi, ebx);
                    }
                    EmitCheckAddress(a, dataBase, edi, sizeof(cell), stp, memoryError);
                    a.mov(edi, DataAt(edi, dataBase));
                    break;
                }

                case Opcode::IDXADDR:
                    a.lea(edi, ptr(ebx, edi, 2));
                    break;

                case Opcode::IDXADDR_B:
                {
                    const uint32_t shift = static_cast<uint32_t>(static_cast<ucell>(operand)) & 31u;
                    if (shift <= 3u)
                        a.lea(edi, ptr(ebx, edi, shift));
                    else
                    {
                        a.shl(edi, static_cast<uint8_t>(shift));
                        a.add(edi, ebx);
                    }
                    break;
                }

                case Opcode::ALIGN_PRI:
                    if (AlignApplies(decoded, operand))
                    {
                        const cell mask = static_cast<cell>(sizeof(cell)) - operand;
                        if (mask != 0)
                            a.xor_(edi, mask);
                    }
                    break;

                case Opcode::ALIGN_ALT:
                    if (AlignApplies(decoded, operand))
                    {
                        const cell mask = static_cast<cell>(sizeof(cell)) - operand;
                        if (mask != 0)
                            a.xor_(ebx, mask);
                    }
                    break;

                case Opcode::LCTRL:
                    switch (operand)
                    {
                        case 0:
                        {
                            auto* hdr = reinterpret_cast<AMX_HEADER*>(amx->base);
                            a.mov(edi, hdr->cod);
                            break;
                        }
                        case 1:
                        {
                            auto* hdr = reinterpret_cast<AMX_HEADER*>(amx->base);
                            a.mov(edi, hdr->dat);
                            break;
                        }
                        case 2:
                            a.mov(edi, Ctx(offsetof(JitContext, hea)));
                            break;
                        case 3:
                            a.mov(edi, stp);
                            break;
                        case 4:
                            a.lea(edi, ptr(esp, NegDataDisp(dataBase)));
                            break;
                        case 5:
                            a.mov(edi, ebp);
                            break;
                        case 6:
                            a.mov(edi, instruction.Next());
                            break;
                        case 9:
                            a.mov(edi, AmxAbs(amxAddress, offsetof(AMX, flags)));
                            break;
                        default:
                            /*
                            portable C leaves PRI unchanged for unsupported
                            control-register selectors (including 7/8).
                        */
                            break;
                    }
                    break;

                case Opcode::SCTRL:
                    switch (operand)
                    {
                        case 2:
                            a.mov(Ctx(offsetof(JitContext, hea)), edi);
                            break;
                        case 4:
                            /*
                            exact Pawn semantics: SCTRL 4 assigns STK from PRI
                            without validating it. A following
                            STACK/POP/RET/etc. performs the architectural
                            check.
                        */
                            a.lea(esp, ptr(edi, static_cast<int32_t>(dataBase)));
                            break;
                        case 5:
                            a.mov(ebp, edi);
                            break;
                        case 6:
                            a.mov(eax, edi);
                            EmitDispatchRelativeEax(a, dispatchCount, dispatchAddress, invalidAddress);
                            break;
                        default:
                            break;
                    }
                    break;

                case Opcode::MOVE_PRI:
                    a.mov(edi, ebx);
                    break;

                case Opcode::MOVE_ALT:
                    a.mov(ebx, edi);
                    break;

                case Opcode::XCHG:
                {
                    const bool priLive = liveness.IsLiveOut(instructionIndex, RegPri);
                    const bool altLive = liveness.IsLiveOut(instructionIndex, RegAlt);
                    if (priLive && altLive)
                        a.xchg(edi, ebx);
                    else if (priLive)
                        a.mov(edi, ebx);
                    else if (altLive)
                        a.mov(ebx, edi);
                    break;
                }

                case Opcode::PUSH_PRI:
                    EmitCheckPushSpace(a, dataBase, static_cast<int>(sizeof(cell)), stackError);
                    a.push(edi);
                    break;

                case Opcode::PUSH_ALT:
                    EmitCheckPushSpace(a, dataBase, static_cast<int>(sizeof(cell)), stackError);
                    a.push(ebx);
                    break;

                case Opcode::PUSH_R:
                    if (operand > 0)
                    {
                        const cell bytes = operand * static_cast<cell>(sizeof(cell));
                        EmitCheckPushSpace(a, dataBase, bytes, stackError);
                        if (operand <= 4)
                        {
                            for (cell i = 0; i < operand; ++i)
                                a.push(edi);
                        }
                        else
                        {
                            Label loop = a.new_label();
                            a.mov(ecx, operand);
                            a.bind(loop);
                            a.push(edi);
                            a.dec(ecx);
                            a.jnz(loop);
                        }
                    }
                    break;

                case Opcode::PUSH_C:
                    EmitCheckPushSpace(a, dataBase, static_cast<int>(sizeof(cell)), stackError);
                    a.push(operand);
                    break;

                case Opcode::PUSH:
                    EmitCheckPushSpace(a, dataBase, static_cast<int>(sizeof(cell)), stackError);
                    a.push(DataAbs(dataBase, operand));
                    break;

                case Opcode::PUSH_S:
                    EmitCheckPushSpace(a, dataBase, static_cast<int>(sizeof(cell)), stackError);
                    a.push(dword_ptr(ebp, DataDisp(dataBase, operand)));
                    break;

                case Opcode::PUSH2_C:
                case Opcode::PUSH3_C:
                case Opcode::PUSH4_C:
                case Opcode::PUSH5_C:
                    EmitCheckPushSpace(a, dataBase, static_cast<int>(instruction.operands.size() * sizeof(cell)), stackError);
                    for (cell value : instruction.operands)
                        a.push(value);
                    break;

                case Opcode::PUSH2:
                case Opcode::PUSH3:
                case Opcode::PUSH4:
                case Opcode::PUSH5:
                    EmitCheckPushSpace(a, dataBase, static_cast<int>(instruction.operands.size() * sizeof(cell)), stackError);
                    for (cell address : instruction.operands)
                        a.push(DataAbs(dataBase, address));
                    break;

                case Opcode::PUSH2_S:
                case Opcode::PUSH3_S:
                case Opcode::PUSH4_S:
                case Opcode::PUSH5_S:
                    EmitCheckPushSpace(a, dataBase, static_cast<int>(instruction.operands.size() * sizeof(cell)), stackError);
                    for (cell offset : instruction.operands)
                    {
                        a.push(dword_ptr(ebp, DataDisp(dataBase, offset)));
                    }
                    break;

                case Opcode::PUSH2_ADR:
                case Opcode::PUSH3_ADR:
                case Opcode::PUSH4_ADR:
                case Opcode::PUSH5_ADR:
                    EmitCheckPushSpace(a, dataBase, static_cast<int>(instruction.operands.size() * sizeof(cell)), stackError);
                    for (cell offset : instruction.operands)
                    {
                        a.lea(eax, ptr(ebp, offset));
                        a.push(eax);
                    }
                    break;

                case Opcode::POP_PRI:
                    EmitCheckRawStackCell(a, dataBase, stp, stackLow);
                    if (liveness.IsLiveOut(instructionIndex, RegPri))
                        a.pop(edi);
                    else
                        a.add(esp, static_cast<int>(sizeof(cell)));
                    break;

                case Opcode::POP_ALT:
                    EmitCheckRawStackCell(a, dataBase, stp, stackLow);
                    if (liveness.IsLiveOut(instructionIndex, RegAlt))
                        a.pop(ebx);
                    else
                        a.add(esp, static_cast<int>(sizeof(cell)));
                    break;

                case Opcode::STACK:
                    a.lea(eax, ptr(esp, NegDataDisp(dataBase)));
                    /*
                    pawn semantics: ALT receives the pre-adjustment STK.
                */
                    a.mov(ebx, eax);
                    a.add(eax, operand);
                    a.jo(stackError);
                    a.test(eax, eax);
                    a.js(stackError);
                    a.mov(edx, Ctx(offsetof(JitContext, hea)));
                    EmitCheckHeapStackMargin(a, edx, eax, stackError);
                    a.cmp(eax, stp);
                    a.jg(stackLow);
                    a.lea(esp, ptr(eax, static_cast<int32_t>(dataBase)));
                    break;

                case Opcode::HEAP:
                    a.mov(eax, Ctx(offsetof(JitContext, hea)));
                    a.mov(ebx, eax);
                    a.add(eax, operand);
                    a.mov(Ctx(offsetof(JitContext, hea)), eax);
                    a.lea(edx, ptr(esp, NegDataDisp(dataBase)));
                    EmitCheckHeapStackMargin(a, eax, edx, stackError);
                    a.cmp(eax, hlw);
                    a.jl(heapLow);
                    break;

                case Opcode::PROC:
                    EmitCheckPushSpace(a, dataBase, static_cast<int>(sizeof(cell)), stackError);
                    a.push(ebp);
                    a.lea(ebp, ptr(esp, NegDataDisp(dataBase)));
                    a.mov(edx, Ctx(offsetof(JitContext, hea)));
                    EmitCheckHeapStackMargin(a, edx, ebp, stackError);
                    break;

                case Opcode::RET:
                {
                    EmitPopFrameAndReturn(a, dataBase, stp, stackLow);
                    const std::vector<cell>* knownReturns = GetReturnSpecializationTargets(callGraph, instructionIndex);
                    EmitSpecializedReturnDispatchEax(a, amx, dispatchAddress, invalidAddress, knownReturns, labels);
                    break;
                }

                case Opcode::RETN:
                {
                    EmitPopFrameAndReturn(a, dataBase, stp, stackLow);
                    /*
                    EAX is the return CIP. params[0] is at the physical Pawn
                    ESP.
                */
                    EmitRetnParameterCleanup(a, dataBase, stp, memoryError, stackLow);
                    if (PortableRetnPublishesStack(decoded))
                        EmitPublishPortableRetnStack(a, dataBase, amxAddress);
                    const std::vector<cell>* knownReturns = GetReturnSpecializationTargets(callGraph, instructionIndex);
                    EmitSpecializedReturnDispatchEax(a, amx, dispatchAddress, invalidAddress, knownReturns, labels);
                    break;
                }

                case Opcode::CALL:
                {
                    if (instructionIndex < inlining.targetByCallInstruction.size())
                    {
                        const cell inlineTarget = inlining.targetByCallInstruction[instructionIndex];
                        if (inlineTarget >= 0)
                        {
                            /*
                            preserve the bytecode return slot and callee frame
                            so LOAD.S/ADDR.S and frame introspection keep the
                            normal Pawn layout.  Only the control
                            transfer/RETN dispatch is replaced by a native
                            fallthrough clone.
                        */
                            EmitCheckPushSpace(a, dataBase, static_cast<int>(sizeof(cell)), stackError);
                            a.push(EncodeReturnAddress(instruction.Next()));
                            EmitInlineFunctionClone(a, amx, decoded, inlining, liveness, inlineTarget, instruction.Next(), dataBase, dispatchAddress, invalidAddress,
                                                    invalidInstruction, memoryError, stackError, stackLow, heapLow, boundsError, divideError, exitLabel);
                            break;
                        }
                    }
                    const cell target = resolveCodegenTarget(ResolveBranchTarget(amx, instruction));
                    EmitCheckPushSpace(a, dataBase, static_cast<int>(sizeof(cell)), stackError);
                    a.push(EncodeReturnAddress(instruction.Next()));
                    /*
                    A direct CALL to the immediately following P-code cell
                    needs the return slot but no native control transfer:
                    sequential generated-code fallthrough enters the exact
                    same callee CIP.
                */
                    if (target != instruction.Next())
                        a.jmp(labels[static_cast<std::size_t>(target / sizeof(cell))]);
                    break;
                }

                case Opcode::CALL_PRI:
                    EmitCheckPushSpace(a, dataBase, static_cast<int>(sizeof(cell)), stackError);
                    a.push(EncodeReturnAddress(instruction.Next()));
                    a.mov(eax, edi);
                    /*
                    CALL.PRI is relative-to-COD in both target host VMs.
                */
                    EmitDispatchRelativeEax(a, dispatchCount, dispatchAddress, invalidAddress);
                    break;

                case Opcode::JUMP:
                case Opcode::JREL:
                {
                    const cell target = resolveCodegenTarget(ResolveBranchTarget(amx, instruction));
                    if (target == instruction.Next())
                    {
                        break;
                    }
                    a.jmp(labels[static_cast<std::size_t>(target / sizeof(cell))]);
                    break;
                }

                case Opcode::JZER:
                case Opcode::JNZ:
                {
                    cell diamondJumpTarget = -1;
                    if (MatchBranchOverJumpDiamond(amx, decoded, instructionIndex, diamondJumpTarget))
                    {
                        const std::size_t jumpIndex = instructionIndex + 1;
                        const cell falseTarget = resolveCodegenTarget(diamondJumpTarget);
                        deferredJumpTargets[jumpIndex] = diamondJumpTarget;
                        /*
                        if both arms canonicalize to the same block, the
                        condition has no control-flow effect.  Keep the
                        skipped JUMP's cold entry label for dynamic-CIP
                        compatibility, but emit no TEST/Jcc on the hot
                        sequential path.
                    */
                        if (falseTarget == instruction.Next())
                            break;
                        a.test(edi, edi);
                        EmitInverseConditionalBranch(a, instruction.opcode, labels[static_cast<std::size_t>(falseTarget / sizeof(cell))]);
                        break;
                    }

                    const cell target = resolveCodegenTarget(ResolveBranchTarget(amx, instruction));
                    if (target == instruction.Next())
                    {
                        break;
                    }
                    a.test(edi, edi);

                    if (instruction.opcode == Opcode::JZER)
                        a.jz(labels[static_cast<std::size_t>(target / sizeof(cell))]);
                    else
                        a.jnz(labels[static_cast<std::size_t>(target / sizeof(cell))]);

                    break;
                }

                case Opcode::JEQ:
                case Opcode::JNEQ:
                case Opcode::JLESS:
                case Opcode::JLEQ:
                case Opcode::JGRTR:
                case Opcode::JGEQ:
                case Opcode::JSLESS:
                case Opcode::JSLEQ:
                case Opcode::JSGRTR:
                case Opcode::JSGEQ:
                {
                    cell diamondJumpTarget = -1;
                    if (MatchBranchOverJumpDiamond(amx, decoded, instructionIndex, diamondJumpTarget))
                    {
                        const std::size_t jumpIndex = instructionIndex + 1;
                        const cell falseTarget = resolveCodegenTarget(diamondJumpTarget);
                        deferredJumpTargets[jumpIndex] = diamondJumpTarget;
                        if (falseTarget == instruction.Next())
                            break;
                        a.cmp(edi, ebx);
                        EmitInverseConditionalBranch(a, instruction.opcode, labels[static_cast<std::size_t>(falseTarget / sizeof(cell))]);
                        break;
                    }

                    const cell target = resolveCodegenTarget(ResolveBranchTarget(amx, instruction));
                    if (target == instruction.Next())
                    {
                        break;
                    }
                    const Label& label = labels[static_cast<std::size_t>(target / sizeof(cell))];

                    a.cmp(edi, ebx);

                    switch (instruction.opcode)
                    {
                        case Opcode::JEQ:
                            a.je(label);
                            break;
                        case Opcode::JNEQ:
                            a.jne(label);
                            break;
                        case Opcode::JLESS:
                            a.jb(label);
                            break;
                        case Opcode::JLEQ:
                            a.jbe(label);
                            break;
                        case Opcode::JGRTR:
                            a.ja(label);
                            break;
                        case Opcode::JGEQ:
                            a.jae(label);
                            break;
                        case Opcode::JSLESS:
                            a.jl(label);
                            break;
                        case Opcode::JSLEQ:
                            a.jle(label);
                            break;
                        case Opcode::JSGRTR:
                            a.jg(label);
                            break;
                        case Opcode::JSGEQ:
                            a.jge(label);
                            break;
                        default:
                            break;
                    }

                    break;
                }

                case Opcode::SHL:
                    a.mov(ecx, ebx);
                    a.shl(edi, cl);
                    break;

                case Opcode::SHR:
                    a.mov(ecx, ebx);
                    a.shr(edi, cl);
                    break;

                case Opcode::SSHR:
                    a.mov(ecx, ebx);
                    a.sar(edi, cl);
                    break;

                case Opcode::SHL_C_PRI:
                {
                    const uint8_t shift = MaskedShiftCount(operand);
                    if (shift != 0)
                        a.shl(edi, shift);
                    break;
                }

                case Opcode::SHL_C_ALT:
                {
                    const uint8_t shift = MaskedShiftCount(operand);
                    if (shift != 0)
                        a.shl(ebx, shift);
                    break;
                }

                case Opcode::SHR_C_PRI:
                {
                    const uint8_t shift = MaskedShiftCount(operand);
                    if (shift != 0)
                        a.shr(edi, shift);
                    break;
                }

                case Opcode::SHR_C_ALT:
                {
                    const uint8_t shift = MaskedShiftCount(operand);
                    if (shift != 0)
                        a.shr(ebx, shift);
                    break;
                }

                case Opcode::SMUL:
                case Opcode::UMUL:
                    a.imul(edi, ebx);
                    break;

                case Opcode::SDIV:
                case Opcode::SDIV_ALT:
                    EmitSignedDivide(a, instruction.opcode == Opcode::SDIV_ALT, liveness.IsLiveOut(instructionIndex, RegPri), liveness.IsLiveOut(instructionIndex, RegAlt),
                                     divideError);
                    break;

                case Opcode::UDIV:
                case Opcode::UDIV_ALT:
                    EmitUnsignedDivide(a, instruction.opcode == Opcode::UDIV_ALT, liveness.IsLiveOut(instructionIndex, RegPri), liveness.IsLiveOut(instructionIndex, RegAlt),
                                       divideError);
                    break;

                case Opcode::ADD:
                    a.add(edi, ebx);
                    break;

                case Opcode::SUB:
                    a.sub(edi, ebx);
                    break;

                case Opcode::SUB_ALT:
                    a.neg(edi);
                    a.add(edi, ebx);
                    break;

                case Opcode::AND:
                    a.and_(edi, ebx);
                    break;

                case Opcode::OR:
                    a.or_(edi, ebx);
                    break;

                case Opcode::XOR:
                    a.xor_(edi, ebx);
                    break;

                case Opcode::NOT:
                    a.test(edi, edi);
                    a.sete(al);
                    a.movzx(edi, al);
                    break;

                case Opcode::NEG:
                    a.neg(edi);
                    break;

                case Opcode::INVERT:
                    a.not_(edi);
                    break;

                case Opcode::ADD_C:
                    if (operand == 1)
                        a.inc(edi);
                    else if (operand == -1)
                        a.dec(edi);
                    else if (operand != 0)
                        a.add(edi, operand);
                    break;

                case Opcode::SMUL_C:
                    if (operand == 0)
                    {
                        a.xor_(edi, edi);
                    }
                    else if (operand == 1)
                    {
                    }
                    else if (operand == -1)
                    {
                        a.neg(edi);
                    }
                    else if (IsPositivePowerOfTwo(operand))
                    {
                        a.shl(edi, PowerOfTwoShift(operand));
                    }
                    else
                    {
                        a.imul(edi, edi, operand);
                    }
                    break;

                case Opcode::ZERO_PRI:
                    a.xor_(edi, edi);
                    break;

                case Opcode::ZERO_ALT:
                    a.xor_(ebx, ebx);
                    break;

                case Opcode::ZERO:
                    a.mov(DataAbs(dataBase, operand), 0);
                    break;

                case Opcode::ZERO_S:
                    a.mov(dword_ptr(ebp, DataDisp(dataBase, operand)), 0);
                    break;

                case Opcode::SIGN_PRI:
                    a.mov(eax, edi);
                    a.movsx(edi, al);
                    break;

                case Opcode::SIGN_ALT:
                    a.mov(eax, ebx);
                    a.movsx(ebx, al);
                    break;

                case Opcode::EQ:
                case Opcode::NEQ:
                case Opcode::LESS:
                case Opcode::LEQ:
                case Opcode::GRTR:
                case Opcode::GEQ:
                case Opcode::SLESS:
                case Opcode::SLEQ:
                case Opcode::SGRTR:
                case Opcode::SGEQ:
                case Opcode::EQ_C_PRI:
                case Opcode::EQ_C_ALT:
                {
                    const Instruction* branch = FindInstruction(decoded, instruction.Next());

                    if (branch != nullptr && (branch->opcode == Opcode::JZER || branch->opcode == Opcode::JNZ))
                    {
                        const auto branchIt = decoded.byCip.find(branch->cip);
                        const Instruction* afterBranch = FindInstruction(decoded, branch->Next());

                        if (branchIt != decoded.byCip.end() && afterBranch != nullptr)
                        {
                            const std::size_t branchIndex = branchIt->second;
                            const bool takenWhenTrue = branch->opcode == Opcode::JNZ;

                            /*
                            canonical Pawn if/else shape: comparison; Jcc
                            taken; JUMP other; taken: Move both bytecode
                            transfers out of line and invert the comparison
                            edge so the physically adjacent taken block is the
                            native fallthrough. Hot path: CMP + Jcc +
                            fallthrough.
                        */
                            cell diamondJumpTarget = -1;
                            if (MatchBranchOverJumpDiamond(amx, decoded, branchIndex, diamondJumpTarget))
                            {
                                const std::size_t jumpIndex = branchIndex + 1;
                                const cell falseTarget = resolveCodegenTarget(diamondJumpTarget);
                                const cell takenTarget = ResolveBranchTarget(amx, *branch);
                                const auto falseIt = decoded.byCip.find(falseTarget);
                                const auto takenIt = decoded.byCip.find(takenTarget);
                                if (jumpIndex < deferredJumpTargets.size() && falseIt != decoded.byCip.end() && takenIt != decoded.byCip.end())
                                {
                                    deferredConditionalBranches[branchIndex] = 1;
                                    deferredJumpTargets[jumpIndex] = diamondJumpTarget;
                                    const bool falseNeedsPri = liveness.IsLiveIn(falseIt->second, RegPri);
                                    const bool takenNeedsPri = liveness.IsLiveIn(takenIt->second, RegPri);

                                    EmitComparisonFlags(a, instruction.opcode, operand);
                                    if (falseNeedsPri)
                                        a.mov(edi, takenWhenTrue ? 0 : 1);
                                    EmitComparisonJump(a, instruction.opcode, labels[static_cast<std::size_t>(falseTarget / sizeof(cell))], !takenWhenTrue);
                                    if (takenNeedsPri)
                                    {
                                        if (takenWhenTrue)
                                            a.mov(edi, 1);
                                        else
                                            a.xor_(edi, edi);
                                    }
                                    break;
                                }
                            }

                            const cell target = resolveCodegenTarget(ResolveBranchTarget(amx, *branch));
                            const cell fallthrough = resolveCodegenTarget(afterBranch->cip);
                            const Label& targetLabel = labels[static_cast<std::size_t>(target / sizeof(cell))];
                            const Label& afterLabel = labels[static_cast<std::size_t>(fallthrough / sizeof(cell))];
                            const auto targetIt = decoded.byCip.find(target);
                            const auto afterIt = decoded.byCip.find(fallthrough);
                            const bool targetNeedsPri = targetIt == decoded.byCip.end() || liveness.IsLiveIn(targetIt->second, RegPri);
                            const bool afterNeedsPri = afterIt == decoded.byCip.end() || liveness.IsLiveIn(afterIt->second, RegPri);

                            /*
                            if the branch's bytecode fallthrough is already
                            the resolved target, move only JZER/JNZ out of
                            line. The next generated body is then the native
                            fallthrough and no JMP is needed after
                            materializing its boolean value.
                        */
                            if (fallthrough == branch->Next())
                            {
                                deferredConditionalBranches[branchIndex] = 1;
                                EmitComparisonFlags(a, instruction.opcode, operand);
                                if (targetNeedsPri)
                                    a.mov(edi, takenWhenTrue ? 1 : 0);
                                EmitComparisonJump(a, instruction.opcode, targetLabel, takenWhenTrue);
                                if (afterNeedsPri)
                                {
                                    if (takenWhenTrue)
                                        a.xor_(edi, edi);
                                    else
                                        a.mov(edi, 1);
                                }
                                break;
                            }

                            /*
                            conservative fallback for a jump-threaded/non-
                            adjacent fallthrough. Preserve the previous
                            explicit transfer.
                        */
                            EmitComparisonFlags(a, instruction.opcode, operand);
                            if (targetNeedsPri)
                                a.mov(edi, takenWhenTrue ? 1 : 0);
                            EmitComparisonJump(a, instruction.opcode, targetLabel, takenWhenTrue);
                            if (afterNeedsPri)
                            {
                                if (takenWhenTrue)
                                    a.xor_(edi, edi);
                                else
                                    a.mov(edi, 1);
                            }
                            a.jmp(afterLabel);
                            break;
                        }
                    }

                    EmitComparisonFlags(a, instruction.opcode, operand);

                    switch (instruction.opcode)
                    {
                        case Opcode::EQ:
                        case Opcode::EQ_C_PRI:
                        case Opcode::EQ_C_ALT:
                            a.sete(al);
                            break;

                        case Opcode::NEQ:
                            a.setne(al);
                            break;
                        case Opcode::LESS:
                            a.setb(al);
                            break;
                        case Opcode::LEQ:
                            a.setbe(al);
                            break;
                        case Opcode::GRTR:
                            a.seta(al);
                            break;
                        case Opcode::GEQ:
                            a.setae(al);
                            break;
                        case Opcode::SLESS:
                            a.setl(al);
                            break;
                        case Opcode::SLEQ:
                            a.setle(al);
                            break;
                        case Opcode::SGRTR:
                            a.setg(al);
                            break;
                        case Opcode::SGEQ:
                            a.setge(al);
                            break;
                        default:
                            break;
                    }

                    a.movzx(edi, al);
                    break;
                }

                case Opcode::INC_PRI:
                    a.inc(edi);
                    break;

                case Opcode::INC_ALT:
                    a.inc(ebx);
                    break;

                case Opcode::INC:
                    a.inc(DataAbs(dataBase, operand));
                    break;

                case Opcode::INC_S:
                    a.inc(dword_ptr(ebp, DataDisp(dataBase, operand)));
                    break;

                case Opcode::INC_I:
                    EmitCheckRawDataRange(a, edi, sizeof(cell), stp, memoryError);
                    a.inc(DataAt(edi, dataBase));
                    break;

                case Opcode::DEC_PRI:
                    a.dec(edi);
                    break;

                case Opcode::DEC_ALT:
                    a.dec(ebx);
                    break;

                case Opcode::DEC:
                    a.dec(DataAbs(dataBase, operand));
                    break;

                case Opcode::DEC_S:
                    a.dec(dword_ptr(ebp, DataDisp(dataBase, operand)));
                    break;

                case Opcode::DEC_I:
                    EmitCheckRawDataRange(a, edi, sizeof(cell), stp, memoryError);
                    a.dec(DataAt(edi, dataBase));
                    break;

                case Opcode::MOVS:
                    EmitFlushPriAlt(a);
                    EmitEnterHostStack(a, dataBase, 8);
                    a.push(operand);
                    a.push(esi);
                    a.mov(eax, Pointer32(&JitMoveMemory));
                    a.call(eax);
                    a.add(esp, 8);
                    EmitLeaveHostStack(a, dataBase);
                    EmitHelperErrorCheck(a, exitLabel);
                    break;

                case Opcode::CMPS:
                    EmitFlushPriAlt(a);
                    EmitEnterHostStack(a, dataBase, 8);
                    a.push(operand);
                    a.push(esi);
                    a.mov(eax, Pointer32(&JitCompareMemory));
                    a.call(eax);
                    a.add(esp, 8);
                    EmitLeaveHostStack(a, dataBase);
                    EmitHelperErrorCheck(a, exitLabel);
                    if (liveness.IsLiveOut(instructionIndex, RegPri))
                        a.mov(edi, Ctx(offsetof(JitContext, pri)));
                    break;

                case Opcode::FILL:
                    EmitFlushPriAlt(a);
                    EmitEnterHostStack(a, dataBase, 8);
                    a.push(operand);
                    a.push(esi);
                    a.mov(eax, Pointer32(&JitFillMemory));
                    a.call(eax);
                    a.add(esp, 8);
                    EmitLeaveHostStack(a, dataBase);
                    EmitHelperErrorCheck(a, exitLabel);
                    break;

                case Opcode::HALT:
                    a.mov(Ctx(offsetof(JitContext, cip)), instruction.Next());
                    a.mov(edx, instruction.Next());
                    a.mov(AmxAbs(amxAddress, offsetof(AMX, cip)), edx);
                    a.mov(Ctx(offsetof(JitContext, error)), operand);
                    a.mov(Ctx(offsetof(JitContext, exitKind)), static_cast<int>(JitExitKind::Halt));
                    a.mov(Ctx(offsetof(JitContext, hasRetval)), 1);
                    a.jmp(exitLabel);
                    break;

                case Opcode::BOUNDS:
                    a.mov(Ctx(offsetof(JitContext, cip)), instruction.Next());
                    if (static_cast<ucell>(operand) != std::numeric_limits<ucell>::max())
                    {
                        a.cmp(edi, operand);
                        a.ja(boundsError);
                    }
                    break;

                case Opcode::SYSREQ_PRI:
                    EmitCallbackNativeCall(a, dataBase, amxAddress, stp, true, 0, instruction.Next(), false, liveness.IsLiveOut(instructionIndex, RegPri), memoryError, exitLabel);
                    break;

                case Opcode::SYSREQ_N:
                {
                    const cell nativeIndex = instruction.operands[0];
                    const cell parameterBytes = instruction.operands[1];

                    /*
                    SYSREQ.N is equivalent to PUSH.C <parameter-bytes>,
                    SYSREQ.C <native>, STACK <parameter-bytes + cell>.  Do the
                    cleanup only after a successful callback: on SLEEP the
                    reference VM leaves STK pointing at params[0]. Reference
                    SYSREQ.N pushes params[0] without a CHKMARGIN/CHKSTACK
                    check. Do not invent an early STACKERR here; normal VM
                    stack checks happen at the same explicit opcodes as the
                    interpreter.
                */
                    a.push(parameterBytes);

                    EmitCallbackNativeCall(a, dataBase, amxAddress, stp, false, nativeIndex, instruction.Next(), true, liveness.IsLiveOut(instructionIndex, RegPri), memoryError,
                                           exitLabel);

                    a.add(esp, parameterBytes + static_cast<cell>(sizeof(cell)));
                    break;
                }

                case Opcode::SYSREQ_C:
                    EmitCallbackNativeCall(a, dataBase, amxAddress, stp, false, operand, instruction.Next(), false, liveness.IsLiveOut(instructionIndex, RegPri), memoryError,
                                           exitLabel);
                    break;

                case Opcode::FILE:
                    a.jmp(invalidInstruction);
                    break;

                case Opcode::LINE:
                case Opcode::SYMBOL:
                case Opcode::SRANGE:
                case Opcode::SYMTAG:
                case Opcode::NOP:
                    break;

                case Opcode::JUMP_PRI:
                    a.mov(eax, edi);
                    EmitDispatchRelativeEax(a, dispatchCount, dispatchAddress, invalidAddress);
                    break;

                case Opcode::SWITCH:
                {
                    const cell tableCip = NormalizeCodeTarget(amx, operand);
                    const Instruction* table = FindInstruction(decoded, tableCip);
                    const cell count = table->operands[0];
                    const cell defaultTarget = resolveCodegenTarget(NormalizeCodeTarget(amx, table->operands[1]));
                    const Label& defaultLabel = labels[static_cast<std::size_t>(defaultTarget / sizeof(cell))];

                    struct SwitchCase
                    {
                        cell value;
                        cell target;
                    };

                    std::vector<SwitchCase> cases;
                    cases.reserve(static_cast<std::size_t>(count));
                    bool sorted = true;

                    for (cell i = 0; i < count; ++i)
                    {
                        const std::size_t valueIndex = 2 + static_cast<std::size_t>(i) * 2;
                        const std::size_t targetIndex = valueIndex + 1;
                        const cell caseValue = table->operands[valueIndex];
                        const cell caseTarget = resolveCodegenTarget(NormalizeCodeTarget(amx, table->operands[targetIndex]));

                        if (!cases.empty() && caseValue <= cases.back().value)
                            sorted = false;

                        cases.push_back({caseValue, caseTarget});
                    }

                    /*
                    strictly sorted Pawn case tables have unique values.  Any
                    case that branches to the default target is therefore
                    observationally identical to not matching a case at all;
                    remove it from the generated compare tree.  Keep malformed
                    or unsorted tables byte-for-byte on the conservative path,
                    where duplicate-value ordering can be observable.
                */
                    if (sorted)
                    {
                        cases.erase(std::remove_if(cases.begin(), cases.end(),
                                                   [defaultTarget](const SwitchCase& item) {
                                                       return item.target == defaultTarget;
                                                   }),
                                    cases.end());
                    }

                    if (sorted && cases.empty())
                    {
                        a.jmp(defaultLabel);
                        break;
                    }

                    struct SwitchRange
                    {
                        cell low;
                        cell high;
                        cell target;
                    };

                    std::vector<SwitchRange> ranges;
                    if (sorted)
                    {
                        ranges.reserve(cases.size());
                        for (const SwitchCase& item : cases)
                        {
                            if (!ranges.empty() && ranges.back().target == item.target && static_cast<int64_t>(ranges.back().high) + 1 == static_cast<int64_t>(item.value))
                            {
                                ranges.back().high = item.value;
                            }
                            else
                            {
                                ranges.push_back({item.value, item.value, item.target});
                            }
                        }
                    }

                    if (sorted && cases.size() >= 8)
                    {
                        /*
                        large normal Pawn switches use a balanced tree over
                        compressed value ranges. Consecutive case values that
                        share one destination therefore cost one range node
                        instead of one equality node each.
                    */
                        auto emitRangeTree = [&](auto&& self, std::size_t lo, std::size_t hi) -> void {
                            if (lo >= hi)
                            {
                                a.jmp(defaultLabel);
                                return;
                            }

                            const std::size_t mid = lo + (hi - lo) / 2;
                            const SwitchRange& item = ranges[mid];
                            const Label& targetLabel = labels[static_cast<std::size_t>(item.target / sizeof(cell))];
                            const bool hasLeft = lo < mid;
                            const bool hasRight = mid + 1 < hi;

                            a.cmp(edi, item.low);
                            if (hasLeft)
                            {
                                Label atOrAbove = a.new_label();
                                a.jge(atOrAbove);
                                self(self, lo, mid);
                                a.bind(atOrAbove);
                            }
                            else
                            {
                                a.jl(defaultLabel);
                            }

                            if (item.low == item.high)
                            {
                                a.je(targetLabel);
                            }
                            else
                            {
                                a.cmp(edi, item.high);
                                a.jle(targetLabel);
                            }

                            if (hasRight)
                                self(self, mid + 1, hi);
                            else
                                a.jmp(defaultLabel);
                        };

                        emitRangeTree(emitRangeTree, 0, ranges.size());
                    }
                    else
                    {
                        /*
                        small normal Pawn switches are emitted as a linear
                        chain. Runs of at least three consecutive values
                        sharing one target become a signed range test; one-
                        and two-value runs retain equality compares because
                        they are smaller/faster there.
                    */
                        if (sorted)
                        {
                            for (const SwitchRange& range : ranges)
                            {
                                const Label& targetLabel = labels[static_cast<std::size_t>(range.target / sizeof(cell))];
                                const int64_t rangeLength = static_cast<int64_t>(range.high) - static_cast<int64_t>(range.low) + 1;

                                if (rangeLength >= 3)
                                {
                                    Label nextRange = a.new_label();
                                    a.cmp(edi, range.low);
                                    a.jl(nextRange);
                                    a.cmp(edi, range.high);
                                    a.jle(targetLabel);
                                    a.bind(nextRange);
                                }
                                else
                                {
                                    for (int64_t value = static_cast<int64_t>(range.low); value <= static_cast<int64_t>(range.high); ++value)
                                    {
                                        a.cmp(edi, static_cast<cell>(value));
                                        a.je(targetLabel);
                                    }
                                }
                            }
                        }
                        else
                        {
                            for (const SwitchCase& item : cases)
                            {
                                a.cmp(edi, item.value);
                                a.je(labels[static_cast<std::size_t>(item.target / sizeof(cell))]);
                            }
                        }

                        a.jmp(defaultLabel);
                    }

                    break;
                }

                case Opcode::CASETBL:
                    a.jmp(invalidInstruction);
                    break;

                case Opcode::SWAP_PRI:
                    EmitCheckRawStackCell(a, dataBase, stp, memoryError);
                    if (liveness.IsLiveOut(instructionIndex, RegPri))
                    {
                        a.mov(eax, dword_ptr(esp));
                        a.mov(dword_ptr(esp), edi);
                        a.mov(edi, eax);
                    }
                    else
                    {
                        a.mov(dword_ptr(esp), edi);
                    }
                    break;

                case Opcode::SWAP_ALT:
                    EmitCheckRawStackCell(a, dataBase, stp, memoryError);
                    if (liveness.IsLiveOut(instructionIndex, RegAlt))
                    {
                        a.mov(eax, dword_ptr(esp));
                        a.mov(dword_ptr(esp), ebx);
                        a.mov(ebx, eax);
                    }
                    else
                    {
                        a.mov(dword_ptr(esp), ebx);
                    }
                    break;

                case Opcode::PUSH_ADR:
                    a.lea(eax, ptr(ebp, operand));
                    EmitPushEax(a, dataBase, stackError);
                    break;

                case Opcode::BREAK:
                {
                    if (amx->debug != nullptr)
                    {
                        EmitDebugBreakCall(a, dataBase, amxAddress, instruction.Next(), exitLabel);
                    }
                    else
                    {
                        Label skipDebug = a.new_label();
                        a.cmp(AmxAbs(amxAddress, offsetof(AMX, debug)), 0);
                        a.je(skipDebug);
                        EmitDebugBreakCall(a, dataBase, amxAddress, instruction.Next(), exitLabel);
                        a.bind(skipDebug);
                    }
                    break;
                }

                default:
                    a.jmp(invalidInstruction);
                    break;
            }
        }

        a.bind(dispatchLabel);
        EmitDispatch(a, dispatchCount, dispatchAddress, invalidAddress);

        a.bind(invalidAddress);
        a.mov(Ctx(offsetof(JitContext, error)), AMX_ERR_MEMACCESS);
        a.mov(Ctx(offsetof(JitContext, exitKind)), static_cast<int>(JitExitKind::Abort));
        a.jmp(exitLabel);

        a.bind(invalidInstruction);
        a.mov(Ctx(offsetof(JitContext, error)), AMX_ERR_INVINSTR);
        a.mov(Ctx(offsetof(JitContext, exitKind)), static_cast<int>(JitExitKind::Abort));
        a.jmp(exitLabel);

        a.bind(memoryError);
        a.mov(Ctx(offsetof(JitContext, error)), AMX_ERR_MEMACCESS);
        a.mov(Ctx(offsetof(JitContext, exitKind)), static_cast<int>(JitExitKind::Abort));
        a.jmp(exitLabel);

        a.bind(stackError);
        a.mov(Ctx(offsetof(JitContext, error)), AMX_ERR_STACKERR);
        a.mov(Ctx(offsetof(JitContext, exitKind)), static_cast<int>(JitExitKind::DirectCheck));
        a.jmp(exitLabel);

        a.bind(stackLow);
        a.mov(Ctx(offsetof(JitContext, error)), AMX_ERR_STACKLOW);
        a.mov(Ctx(offsetof(JitContext, exitKind)), static_cast<int>(JitExitKind::DirectCheck));
        a.jmp(exitLabel);

        a.bind(heapLow);
        a.mov(Ctx(offsetof(JitContext, error)), AMX_ERR_HEAPLOW);
        a.mov(Ctx(offsetof(JitContext, exitKind)), static_cast<int>(JitExitKind::DirectCheck));
        a.jmp(exitLabel);

        a.bind(boundsError);
        /*
        portable Pawn publishes the post-BOUNDS CIP before ABORT().
    */
        a.mov(edx, Ctx(offsetof(JitContext, cip)));
        a.mov(AmxAbs(amxAddress, offsetof(AMX, cip)), edx);
        a.mov(Ctx(offsetof(JitContext, error)), AMX_ERR_BOUNDS);
        a.mov(Ctx(offsetof(JitContext, exitKind)), static_cast<int>(JitExitKind::Abort));
        a.jmp(exitLabel);

        a.bind(divideError);
        a.mov(Ctx(offsetof(JitContext, error)), AMX_ERR_DIVIDE);
        a.mov(Ctx(offsetof(JitContext, exitKind)), static_cast<int>(JitExitKind::Abort));
        a.jmp(exitLabel);

        a.bind(exitLabel);
        EmitFlushVmRegisters(a, dataBase);
        a.mov(eax, Ctx(offsetof(JitContext, error)));
        a.mov(esp, Ctx(offsetof(JitContext, hostEsp)));
        a.pop(esi);
        a.pop(ebp);
        a.pop(edi);
        a.pop(ebx);
        a.ret();

        /*
        keep canonical entry points for JZER/JNZ bodies removed from fused
        comparison hot paths. A direct/dynamic entry to the original branch
        CIP still performs the architectural PRI test and chooses between its
        taken target and exact bytecode fallthrough.
    */
        for (std::size_t instructionIndex = 0; instructionIndex < deferredConditionalBranches.size(); ++instructionIndex)
        {
            if (deferredConditionalBranches[instructionIndex] == 0)
                continue;

            const Instruction& branch = decoded.instructions[instructionIndex];
            const std::size_t slot = static_cast<std::size_t>(branch.cip / sizeof(cell));
            a.bind(labels[slot]);
            nativeOffsets[slot] = a.offset();

            const cell target = resolveCodegenTarget(ResolveBranchTarget(amx, branch));
            const std::size_t targetSlot = static_cast<std::size_t>(target / sizeof(cell));
            const std::size_t nextSlot = static_cast<std::size_t>(branch.Next() / sizeof(cell));
            if (targetSlot == nextSlot)
            {
                a.jmp(labels[nextSlot]);
                continue;
            }
            a.test(edi, edi);
            if (branch.opcode == Opcode::JZER)
                a.jz(labels[targetSlot]);
            else
                a.jnz(labels[targetSlot]);
            a.jmp(labels[nextSlot]);
        }

        /*
        keep canonical entry points for unconditional jumps removed from the
        hot if/else layout. Dynamic/cold dispatch to the original P-code cell
        lands here and performs the same jump, while normal sequential flow
        uses one inverted Jcc and falls directly into the adjacent arm.
    */
        for (std::size_t instructionIndex = 0; instructionIndex < deferredJumpTargets.size(); ++instructionIndex)
        {
            const cell target = deferredJumpTargets[instructionIndex];
            if (target < 0)
                continue;

            const Instruction& instruction = decoded.instructions[instructionIndex];
            const std::size_t slot = static_cast<std::size_t>(instruction.cip / sizeof(cell));
            a.bind(labels[slot]);
            nativeOffsets[slot] = a.offset();
            const cell threadedTarget = resolveCodegenTarget(target);
            a.jmp(labels[static_cast<std::size_t>(threadedTarget / sizeof(cell))]);
        }

        const std::size_t nativeCodeSize = a.offset();

        JitEntryAllocationGuard allocation;
        asmError = g_jitRuntime.add(&allocation.entry, &code);

        if (asmError != Error::kOk || allocation.entry == nullptr)
            return false;

        /*
        jitRuntime::add() writes the executable address directly to our typed
        function pointer. Convert that address through uintptr_t instead of
        relying on AsmJit helper APIs that have changed between releases.
    */
        auto* base = reinterpret_cast<unsigned char*>(reinterpret_cast<std::uintptr_t>(allocation.entry));

        /*
        from this point until the final move, every allocation is owned by
        local temporaries. If reserve/assign/sort throws,
        JitEntryAllocationGuard releases executable memory and the caller's
        CompiledProgram is untouched.
    */
        CompiledProgram candidate;
        candidate.base = amx->base;
        candidate.data = data;
        candidate.debugHook = amx->debug;
        candidate.entry = allocation.entry;
        auto* hdr = reinterpret_cast<AMX_HEADER*>(amx->base);
        candidate.code = amx->base + hdr->cod;
        candidate.codeSize = decoded.codeSize;
        candidate.nativeCode = base;
        candidate.nativeCodeSize = nativeCodeSize;
        candidate.hlw = amx->hlw;
        candidate.stp = amx->stp;

        for (const Instruction& instruction : decoded.instructions)
        {
            const std::size_t slot = static_cast<std::size_t>(instruction.cip / sizeof(cell));
            const std::size_t offset = nativeOffsets[slot];

            if (offset != std::numeric_limits<std::size_t>::max())
                dispatchTable[slot] = base + offset;
        }

        /*
        generated indirect-dispatch code embeds dispatchAddress. swap()
        transfers the exact preallocated buffer into CompiledProgram without
        changing that address, so the immediate remains valid for the
        program's full lifetime.
    */
        candidate.dispatch.swap(dispatchTable);

        /*
        open.mp's portable VM uses a relative zero top-level return sentinel.
    */
        candidate.topReturn = kTopReturn;

        /*
        public definitions are immutable for a loaded open.mp AMX.  Cache
        their bytecode CIPs so recursive CallLocalFunction / amx_Exec calls do
        not have to rescan the public table on every re-entry. The generated
        entry trampoline still uses the proven common dispatcher for safety.
    */
        if (hdr->defsize >= static_cast<int>(sizeof(uint32_t)) && hdr->publics >= 0 && hdr->natives >= hdr->publics && hdr->natives <= hdr->cod &&
            ((hdr->natives - hdr->publics) % hdr->defsize) == 0)
        {
            const std::size_t publicCount = static_cast<std::size_t>((hdr->natives - hdr->publics) / hdr->defsize);
            candidate.publicCips.assign(publicCount, static_cast<cell>(-1));

            for (std::size_t i = 0; i < publicCount; ++i)
            {
                auto* publicEntry = reinterpret_cast<AMX_FUNCSTUB*>(amx->base + hdr->publics + i * static_cast<std::size_t>(hdr->defsize));
                const cell publicCip = static_cast<cell>(publicEntry->address);
                if (publicCip < 0 || (publicCip % static_cast<cell>(sizeof(cell))) != 0)
                    continue;

                const std::size_t slot = static_cast<std::size_t>(publicCip / sizeof(cell));
                if (slot >= candidate.dispatch.size() || candidate.dispatch[slot] == nullptr)
                    continue;

                candidate.publicCips[i] = publicCip;
            }
        }

        /*
        keep any previous output alive until the new program has been fully
        constructed. The static_assert above guarantees both moves are
        noexcept, so this is a real commit point even if CompileProgram is
        reused outside the current EnsureCompiled() empty-output path.
    */
        CompiledProgram previous = std::move(program);
        program = std::move(candidate);
        allocation.Commit();
        ReleaseProgram(previous);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void ReleaseProgram(CompiledProgram& program) noexcept
{
    if (program.entry != nullptr)
        g_jitRuntime.release(program.entry);

    program = {};
}
