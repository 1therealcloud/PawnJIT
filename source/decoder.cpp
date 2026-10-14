/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The original code is copyright (c) 2026, 1therealcloud.
 */

#include "decoder.hpp"

#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace
{
    enum class HostOpcodeEncoding : uint8_t
    {
        Unknown,
        Numeric,
        RelocatedHandlers,
    };

    std::vector<cell> g_hostOpcodeTable;
    HostOpcodeEncoding g_hostOpcodeEncoding = HostOpcodeEncoding::Unknown;

    cell NormalizeOpcodeCell(cell rawOpcode)
    {
        if (rawOpcode > 0 && rawOpcode <= 157)
            return rawOpcode;

        for (std::size_t i = 1; i < g_hostOpcodeTable.size(); ++i)
        {
            if (g_hostOpcodeTable[i] == rawOpcode)
                return static_cast<cell>(i);
        }

        return -1;
    }

    bool IsSingleOperand(Opcode opcode)
    {
        switch (opcode)
        {
            case Opcode::LOAD_PRI:
            case Opcode::LOAD_ALT:
            case Opcode::LOAD_S_PRI:
            case Opcode::LOAD_S_ALT:
            case Opcode::LREF_PRI:
            case Opcode::LREF_ALT:
            case Opcode::LREF_S_PRI:
            case Opcode::LREF_S_ALT:
            case Opcode::LODB_I:
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
            case Opcode::STRB_I:
            case Opcode::LIDX_B:
            case Opcode::IDXADDR_B:
            case Opcode::ALIGN_PRI:
            case Opcode::ALIGN_ALT:
            case Opcode::LCTRL:
            case Opcode::SCTRL:
            case Opcode::PUSH_R:
            case Opcode::PUSH_C:
            case Opcode::PUSH:
            case Opcode::PUSH_S:
            case Opcode::STACK:
            case Opcode::HEAP:
            case Opcode::JREL:
            case Opcode::JUMP:
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
            case Opcode::SHL_C_PRI:
            case Opcode::SHL_C_ALT:
            case Opcode::SHR_C_PRI:
            case Opcode::SHR_C_ALT:
            case Opcode::ADD_C:
            case Opcode::SMUL_C:
            case Opcode::ZERO:
            case Opcode::ZERO_S:
            case Opcode::EQ_C_PRI:
            case Opcode::EQ_C_ALT:
            case Opcode::INC:
            case Opcode::INC_S:
            case Opcode::DEC:
            case Opcode::DEC_S:
            case Opcode::MOVS:
            case Opcode::CMPS:
            case Opcode::FILL:
            case Opcode::HALT:
            case Opcode::BOUNDS:
            case Opcode::CALL:
            case Opcode::SYSREQ_C:
            case Opcode::PUSH_ADR:
            case Opcode::SWITCH:
            case Opcode::SYMTAG:
                return true;

            default:
                return false;
        }
    }

    bool IsNoOperand(Opcode opcode)
    {
        switch (opcode)
        {
            case Opcode::LOAD_I:
            case Opcode::STOR_I:
            case Opcode::LIDX:
            case Opcode::IDXADDR:
            case Opcode::MOVE_PRI:
            case Opcode::MOVE_ALT:
            case Opcode::XCHG:
            case Opcode::PUSH_PRI:
            case Opcode::PUSH_ALT:
            case Opcode::POP_PRI:
            case Opcode::POP_ALT:
            case Opcode::PROC:
            case Opcode::RET:
            case Opcode::RETN:
            case Opcode::CALL_PRI:
            case Opcode::SHL:
            case Opcode::SHR:
            case Opcode::SSHR:
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
            case Opcode::ZERO_PRI:
            case Opcode::ZERO_ALT:
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
            case Opcode::INC_PRI:
            case Opcode::INC_ALT:
            case Opcode::INC_I:
            case Opcode::DEC_PRI:
            case Opcode::DEC_ALT:
            case Opcode::DEC_I:
            case Opcode::SYSREQ_PRI:
            case Opcode::JUMP_PRI:
            case Opcode::SWAP_PRI:
            case Opcode::SWAP_ALT:
            case Opcode::NOP:
            case Opcode::BREAK:
                return true;

            default:
                return false;
        }
    }

    int FixedOperandCount(Opcode opcode)
    {
        if (IsNoOperand(opcode))
            return 0;
        if (IsSingleOperand(opcode))
            return 1;

        switch (opcode)
        {
            case Opcode::SYSREQ_N:
            case Opcode::PUSH2_C:
            case Opcode::PUSH2:
            case Opcode::PUSH2_S:
            case Opcode::PUSH2_ADR:
            case Opcode::LOAD_BOTH:
            case Opcode::LOAD_S_BOTH:
            case Opcode::CONST_OP:
            case Opcode::CONST_S:
                return 2;

            case Opcode::PUSH3_C:
            case Opcode::PUSH3:
            case Opcode::PUSH3_S:
            case Opcode::PUSH3_ADR:
                return 3;

            case Opcode::PUSH4_C:
            case Opcode::PUSH4:
            case Opcode::PUSH4_S:
            case Opcode::PUSH4_ADR:
                return 4;

            case Opcode::PUSH5_C:
            case Opcode::PUSH5:
            case Opcode::PUSH5_S:
            case Opcode::PUSH5_ADR:
                return 5;

            default:
                return -1;
        }
    }

    bool IsFileV9MacroOpcode(cell rawOpcode)
    {
        return rawOpcode >= 138 && rawOpcode <= 157;
    }
}

void MarkHostOpcodeStreamNumeric() noexcept
{
    g_hostOpcodeTable.clear();
    g_hostOpcodeEncoding = HostOpcodeEncoding::Numeric;
}

void ConfigureHostOpcodeTable(const cell* table, std::size_t count) noexcept
{
    g_hostOpcodeTable.clear();
    g_hostOpcodeEncoding = HostOpcodeEncoding::Unknown;
    if (table == nullptr || count == 0)
        return;

    try
    {
        g_hostOpcodeTable.assign(table, table + count);
        g_hostOpcodeEncoding = HostOpcodeEncoding::RelocatedHandlers;
    }
    catch (...)
    {
        g_hostOpcodeTable.clear();
        g_hostOpcodeEncoding = HostOpcodeEncoding::Unknown;
    }
}

void ClearHostOpcodeTable() noexcept
{
    g_hostOpcodeTable.clear();
    g_hostOpcodeEncoding = HostOpcodeEncoding::Unknown;
}

bool ValidateAmxImage(const AMX* amx, AmxImageInfo* info) noexcept
{
    if (info != nullptr)
        *info = {};

    if (amx == nullptr || amx->base == nullptr)
        return false;

    const auto* hdr = reinterpret_cast<const AMX_HEADER*>(amx->base);
    if (hdr->magic != AMX_MAGIC)
        return false;

    /*
        match openmultiplayer/compiler's supported file/runtime versions.
        the historical file-v9/amx-v9 -O2 format is explicitly rejected by
        the reference VM; current macro-opcode files are file v9 / AMX v10.
    */
    if (hdr->file_version < 6 || hdr->file_version > 9 || hdr->amx_version < 6 || (hdr->file_version == 9 && hdr->amx_version == 9))
    {
        return false;
    }

    if (hdr->cod < 0 || hdr->dat <= hdr->cod || hdr->hea < hdr->dat || hdr->stp < hdr->hea)
    {
        return false;
    }

    if ((hdr->cod % static_cast<int32_t>(sizeof(cell))) != 0 || (hdr->dat % static_cast<int32_t>(sizeof(cell))) != 0)
    {
        return false;
    }

    const int64_t codeSize64 = static_cast<int64_t>(hdr->dat) - static_cast<int64_t>(hdr->cod);
    if (codeSize64 <= 0 || codeSize64 > static_cast<int64_t>(std::numeric_limits<cell>::max()) || (codeSize64 % static_cast<int64_t>(sizeof(cell))) != 0)
    {
        return false;
    }

    const int32_t headerSize = static_cast<int32_t>(sizeof(AMX_HEADER));
    const int32_t defsize = static_cast<int32_t>(hdr->defsize);
    const bool usesNameTable = defsize == static_cast<int32_t>(sizeof(AMX_FUNCSTUBNT));
    if (defsize != static_cast<int32_t>(sizeof(AMX_FUNCSTUB)) && !usesNameTable)
    {
        return false;
    }

    const bool orderedTables =
        hdr->publics >= headerSize && hdr->publics <= hdr->natives && hdr->natives <= hdr->libraries && hdr->libraries <= hdr->pubvars && hdr->pubvars <= hdr->tags;
    if (!orderedTables)
        return false;

    const auto tableMultiple = [defsize](int32_t begin, int32_t end) noexcept {
        return end >= begin && ((end - begin) % defsize) == 0;
    };

    if (!tableMultiple(hdr->publics, hdr->natives) || !tableMultiple(hdr->natives, hdr->libraries) || !tableMultiple(hdr->libraries, hdr->pubvars) ||
        !tableMultiple(hdr->pubvars, hdr->tags))
    {
        return false;
    }

    if (!usesNameTable)
    {
        if (hdr->tags > hdr->cod || !tableMultiple(hdr->tags, hdr->cod))
            return false;
    }
    else
    {
        if (hdr->tags > hdr->nametable || hdr->nametable > hdr->cod || !tableMultiple(hdr->tags, hdr->nametable))
        {
            return false;
        }

        /*
            amx_Init reads the leading uint16_t maximum-name-length field
            when USENAMETABLE(hdr) is true. prove that field remains inside
            the immutable pre-code area before reading it here as well.
        */
        const int64_t nameLengthEnd = static_cast<int64_t>(hdr->nametable) + static_cast<int64_t>(sizeof(uint16_t));
        if (nameLengthEnd > hdr->cod)
            return false;

        uint16_t maxNameLength = 0;
        std::memcpy(&maxNameLength, amx->base + static_cast<std::size_t>(hdr->nametable), sizeof(maxNameLength));
        constexpr uint16_t kMaxPawnNameLength = 31;
        if (maxNameLength > kMaxPawnNameLength)
            return false;
    }

    /*
        the reference runtime expects an uncompressed image to end exactly
        at HEA. compact images retain the compressed file size in hdr->size
        after in-place expansion, so only require that compressed payload
        to stay between COD and the expanded HEA boundary.
    */
    if ((hdr->flags & AMX_FLAG_COMPACT) == 0)
    {
        if (hdr->size != hdr->hea)
            return false;
    }
    else if (hdr->size < hdr->cod || hdr->size > hdr->hea)
    {
        return false;
    }

    const int64_t publicCount64 = static_cast<int64_t>(hdr->natives - hdr->publics) / defsize;
    const int64_t nativeCount64 = static_cast<int64_t>(hdr->libraries - hdr->natives) / defsize;
    if (publicCount64 < 0 || nativeCount64 < 0 || static_cast<uint64_t>(publicCount64) > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max()) ||
        static_cast<uint64_t>(nativeCount64) > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max()))
    {
        return false;
    }

    if (info != nullptr)
    {
        info->header = hdr;
        info->codeSize = static_cast<cell>(codeSize64);
        info->publicCount = static_cast<std::size_t>(publicCount64);
        info->nativeCount = static_cast<std::size_t>(nativeCount64);
    }

    return true;
}

PortableVmFlavor DetectPortableVmFlavor()
{
    /*
        the classic i386 GNU/ICC interpreter executes a computed-goto
        stream, so amx_Init relocates opcodes to handler addresses. the
        ANSI/switch C implementation keeps numeric opcodes. the loaded
        stream therefore tells us which observable C-VM semantics the host
        actually uses.
    */
    return g_hostOpcodeEncoding == HostOpcodeEncoding::RelocatedHandlers ? PortableVmFlavor::GnuThreaded : PortableVmFlavor::AnsiSwitch;
}

cell NormalizeCodeTarget(AMX* amx, cell value)
{
    if (amx == nullptr || amx->base == nullptr)
        return -1;

    auto* hdr = reinterpret_cast<AMX_HEADER*>(amx->base);
    const int64_t codeSize64 = static_cast<int64_t>(hdr->dat) - static_cast<int64_t>(hdr->cod);

    if (hdr->cod < 0 || codeSize64 <= 0 || codeSize64 > std::numeric_limits<cell>::max())
        return -1;

    const cell codeSize = static_cast<cell>(codeSize64);
    const uintptr_t base = reinterpret_cast<uintptr_t>(amx->base);
    const uintptr_t codeOffset = static_cast<uintptr_t>(static_cast<uint32_t>(hdr->cod));

    if (codeOffset > std::numeric_limits<uintptr_t>::max() - base)
        return -1;

    const uintptr_t codeBase = base + codeOffset;
    const uintptr_t raw = static_cast<uintptr_t>(static_cast<ucell>(value));

    /*
        use subtraction after proving raw >= codeBase. avoid
        codeBase+codeSize, which can wrap for malformed external/native
        addresses.
    */
    if (raw >= codeBase)
    {
        const uintptr_t offset = raw - codeBase;

        if (offset < static_cast<uintptr_t>(static_cast<ucell>(codeSize)) && (offset % sizeof(cell)) == 0)
        {
            return static_cast<cell>(offset);
        }
    }

    if (value >= 0 && value < codeSize && (value % sizeof(cell)) == 0)
        return value;

    return -1;
}

bool DecodeProgram(AMX* amx, DecodedProgram& program)
{
    program = {};

    if (amx == nullptr || amx->base == nullptr)
    {
        return false;
    }

    AmxImageInfo image;
    if (!ValidateAmxImage(amx, &image))
        return false;

    auto* hdr = const_cast<AMX_HEADER*>(image.header);
    program.portableFlavor = DetectPortableVmFlavor();
    program.codeSize = image.codeSize;

    auto* code = reinterpret_cast<const cell*>(amx->base + hdr->cod);
    cell cip = 0;

    while (cip < program.codeSize)
    {
        const std::size_t index = static_cast<std::size_t>(cip / sizeof(cell));
        const cell rawOpcodeCell = code[index];
        cell rawOpcode = -1;

        rawOpcode = NormalizeOpcodeCell(rawOpcodeCell);

        if (rawOpcode <= 0 || rawOpcode > 157)
        {
            return false;
        }

        Instruction instruction;
        instruction.cip = cip;

        if (rawOpcode == 135)
        {
            instruction.opcode = Opcode::SYSREQ_N;
        }
        else
        {
            if (IsFileV9MacroOpcode(rawOpcode) && hdr->file_version < 9)
            {
                return false;
            }
            instruction.opcode = static_cast<Opcode>(rawOpcode);
        }

        if (instruction.opcode == Opcode::FILE || instruction.opcode == Opcode::SYMBOL)
        {
            if (cip + static_cast<cell>(sizeof(cell) * 2) > program.codeSize)
            {
                return false;
            }

            const cell payloadBytes = code[index + 1];

            if (payloadBytes < 0)
            {
                return false;
            }

            const uint64_t totalBytes = static_cast<uint64_t>(sizeof(cell) * 2) + static_cast<uint64_t>(static_cast<ucell>(payloadBytes));

            if (totalBytes > static_cast<uint64_t>(program.codeSize - cip) || (totalBytes % sizeof(cell)) != 0 ||
                totalBytes > static_cast<uint64_t>(std::numeric_limits<cell>::max()))
            {
                return false;
            }

            instruction.operands.push_back(payloadBytes);
            instruction.byteSize = static_cast<cell>(totalBytes);
        }
        else if (instruction.opcode == Opcode::LINE || instruction.opcode == Opcode::SRANGE)
        {
            if (cip + static_cast<cell>(sizeof(cell) * 3) > program.codeSize)
            {
                return false;
            }

            instruction.operands.push_back(code[index + 1]);
            instruction.operands.push_back(code[index + 2]);
        }
        else if (instruction.opcode == Opcode::CASETBL)
        {
            if (cip + static_cast<cell>(sizeof(cell) * 3) > program.codeSize)
            {
                return false;
            }

            const cell count = code[index + 1];

            if (count < 0 || count > 0x100000)
            {
                return false;
            }

            const std::size_t operandCount = static_cast<std::size_t>((count + 1) * 2);
            const std::size_t totalCells = 1 + operandCount;

            if (static_cast<uint64_t>(cip) + totalCells * sizeof(cell) > static_cast<uint64_t>(program.codeSize))
            {
                return false;
            }

            for (std::size_t i = 0; i < operandCount; ++i)
                instruction.operands.push_back(code[index + 1 + i]);
        }
        else
        {
            const int operandCount = FixedOperandCount(instruction.opcode);
            if (operandCount < 0)
            {
                return false;
            }

            const uint64_t totalBytes = static_cast<uint64_t>(1 + operandCount) * sizeof(cell);
            if (totalBytes > static_cast<uint64_t>(program.codeSize - cip))
            {
                return false;
            }

            for (int i = 0; i < operandCount; ++i)
                instruction.operands.push_back(code[index + 1 + static_cast<std::size_t>(i)]);

            /*
                SYSREQ.N's second operand is the parameter byte count.
                normal compiler output always uses a non-negative
                cell-aligned value. malformed values are delegated to the
                original interpreter instead of turning x86 ESP into an
                unsafe native pointer.
            */
            if (instruction.opcode == Opcode::SYSREQ_N && (instruction.operands[1] < 0 || (instruction.operands[1] % static_cast<cell>(sizeof(cell))) != 0))
            {
                return false;
            }

            if ((instruction.opcode == Opcode::MOVS || instruction.opcode == Opcode::CMPS || instruction.opcode == Opcode::FILL) && instruction.operands[0] < 0)
            {
                /*
                    negative bulk sizes enter implementation-specific/
                    undefined paths in the original VMs (including enormous
                    REP/memcpy counts). preserve strict host behavior
                    through interpreter fallback instead of manufacturing a
                    JIT MEMACCESS result.
                */
                return false;
            }

            if (instruction.opcode == Opcode::PUSH_R)
            {
                const cell count = instruction.operands[0];
                const cell maxSafeCount = std::numeric_limits<cell>::max() / static_cast<cell>(sizeof(cell));

                /*
                    PUSH.R is obsolete and malformed counts expose host-VM
                    quirks. portable C treats zero as a no-op. negative/huge
                    counts are delegated to the original VM instead of
                    overflowing the native-stack byte calculation used by
                    codegen.
                */
                if (count < 0 || count > maxSafeCount)
                {
                    return false;
                }
            }
        }

        program.byCip.emplace(instruction.cip, program.instructions.size());
        cip += instruction.Size();
        program.instructions.push_back(std::move(instruction));
    }

    if (cip != program.codeSize)
    {
        return false;
    }

    return true;
}
