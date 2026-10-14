/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The original code is copyright (c) 2026, 1therealcloud.
 */

#pragma once

#include "opcode.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

struct AmxImageInfo
{
    const AMX_HEADER* header = nullptr;
    cell codeSize = 0;
    std::size_t publicCount = 0;
    std::size_t nativeCount = 0;
};

/*
    the portable C interpreter has two observable execution flavours in the
    classic Pawn source. GCC/ICC i386 uses a relocated indirect-threaded
    opcode stream, while the ANSI/switch implementation (notably
    MSVC/Windows) keeps numeric opcodes. a few historical edge cases differ
    between the two.
*/
enum class PortableVmFlavor : uint8_t
{
    AnsiSwitch,
    GnuThreaded,
};

bool ValidateAmxImage(const AMX* amx, AmxImageInfo* info = nullptr) noexcept;
PortableVmFlavor DetectPortableVmFlavor();
void MarkHostOpcodeStreamNumeric() noexcept;
void ConfigureHostOpcodeTable(const cell* table, std::size_t count) noexcept;
void ClearHostOpcodeTable() noexcept;

struct Instruction
{
    cell cip = 0;
    Opcode opcode = Opcode::NONE;
    std::vector<cell> operands;
    cell byteSize = 0;

    cell Size() const
    {
        return byteSize != 0 ? byteSize : static_cast<cell>((1 + operands.size()) * sizeof(cell));
    }

    cell Next() const
    {
        return cip + Size();
    }
};

struct DecodedProgram
{
    PortableVmFlavor portableFlavor = PortableVmFlavor::AnsiSwitch;
    cell codeSize = 0;
    std::vector<Instruction> instructions;
    std::unordered_map<cell, std::size_t> byCip;
};

bool DecodeProgram(AMX* amx, DecodedProgram& program);
cell NormalizeCodeTarget(AMX* amx, cell value);
