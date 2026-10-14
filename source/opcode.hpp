/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The original code is copyright (c) 2026, 1therealcloud.
 */

#pragma once

#if defined(_MSC_VER)
/*
    UCRT's malloc.h defines alloca before the classic Pawn header gets a
    chance to provide its own compatibility macro. Including it first
    avoids a macro redefinition warning on modern MSVC/Windows SDK
    combinations.
*/
#   include <malloc.h>
#endif

#include <amx/amx.h>

enum class Opcode : cell
{
    NONE = 0,
    LOAD_PRI,
    LOAD_ALT,
    LOAD_S_PRI,
    LOAD_S_ALT,
    LREF_PRI,
    LREF_ALT,
    LREF_S_PRI,
    LREF_S_ALT,
    LOAD_I,
    LODB_I,
    CONST_PRI,
    CONST_ALT,
    ADDR_PRI,
    ADDR_ALT,
    STOR_PRI,
    STOR_ALT,
    STOR_S_PRI,
    STOR_S_ALT,
    SREF_PRI,
    SREF_ALT,
    SREF_S_PRI,
    SREF_S_ALT,
    STOR_I,
    STRB_I,
    LIDX,
    LIDX_B,
    IDXADDR,
    IDXADDR_B,
    ALIGN_PRI,
    ALIGN_ALT,
    LCTRL,
    SCTRL,
    MOVE_PRI,
    MOVE_ALT,
    XCHG,
    PUSH_PRI,
    PUSH_ALT,
    PUSH_R,
    PUSH_C,
    PUSH,
    PUSH_S,
    POP_PRI,
    POP_ALT,
    STACK,
    HEAP,
    PROC,
    RET,
    RETN,
    CALL,
    CALL_PRI,
    JUMP,
    JREL,
    JZER,
    JNZ,
    JEQ,
    JNEQ,
    JLESS,
    JLEQ,
    JGRTR,
    JGEQ,
    JSLESS,
    JSLEQ,
    JSGRTR,
    JSGEQ,
    SHL,
    SHR,
    SSHR,
    SHL_C_PRI,
    SHL_C_ALT,
    SHR_C_PRI,
    SHR_C_ALT,
    SMUL,
    SDIV,
    SDIV_ALT,
    UMUL,
    UDIV,
    UDIV_ALT,
    ADD,
    SUB,
    SUB_ALT,
    AND,
    OR,
    XOR,
    NOT,
    NEG,
    INVERT,
    ADD_C,
    SMUL_C,
    ZERO_PRI,
    ZERO_ALT,
    ZERO,
    ZERO_S,
    SIGN_PRI,
    SIGN_ALT,
    EQ,
    NEQ,
    LESS,
    LEQ,
    GRTR,
    GEQ,
    SLESS,
    SLEQ,
    SGRTR,
    SGEQ,
    EQ_C_PRI,
    EQ_C_ALT,
    INC_PRI,
    INC_ALT,
    INC,
    INC_S,
    INC_I,
    DEC_PRI,
    DEC_ALT,
    DEC,
    DEC_S,
    DEC_I,
    MOVS,
    CMPS,
    FILL,
    HALT,
    BOUNDS,
    SYSREQ_PRI,
    SYSREQ_C,
    FILE,
    LINE,
    SYMBOL,
    SRANGE,
    JUMP_PRI,
    SWITCH,
    CASETBL,
    SWAP_PRI,
    SWAP_ALT,
    PUSH_ADR,
    NOP,
    /* openmultiplayer/compiler file opcode 135 */
    SYSREQ_N = 135,
    SYMTAG = 136,
    BREAK = 137,

    /* file-format v9 macro opcodes */
    PUSH2_C = 138,
    PUSH2,
    PUSH2_S,
    PUSH2_ADR,
    PUSH3_C,
    PUSH3,
    PUSH3_S,
    PUSH3_ADR,
    PUSH4_C,
    PUSH4,
    PUSH4_S,
    PUSH4_ADR,
    PUSH5_C,
    PUSH5,
    PUSH5_S,
    PUSH5_ADR,
    LOAD_BOTH,
    LOAD_S_BOTH,
    CONST_OP,
    CONST_S,
};
static_assert(static_cast<cell>(Opcode::PUSH_PRI) == 36, "unexpected Pawn opcode ABI");
static_assert(static_cast<cell>(Opcode::PROC) == 46, "unexpected Pawn opcode ABI");
static_assert(static_cast<cell>(Opcode::SHL) == 65, "unexpected Pawn opcode ABI");
static_assert(static_cast<cell>(Opcode::SMUL) == 72, "unexpected Pawn opcode ABI");
static_assert(static_cast<cell>(Opcode::EQ) == 95, "unexpected Pawn opcode ABI");
static_assert(static_cast<cell>(Opcode::INC_PRI) == 107, "unexpected Pawn opcode ABI");
static_assert(static_cast<cell>(Opcode::MOVS) == 117, "unexpected Pawn opcode ABI");
static_assert(static_cast<cell>(Opcode::SYMBOL) == 126, "unexpected Pawn opcode ABI");
static_assert(static_cast<cell>(Opcode::SYSREQ_N) == 135, "unexpected Pawn opcode ABI");
static_assert(static_cast<cell>(Opcode::BREAK) == 137, "unexpected Pawn opcode ABI");
static_assert(static_cast<cell>(Opcode::PUSH2_C) == 138, "unexpected Pawn opcode ABI");
static_assert(static_cast<cell>(Opcode::CONST_S) == 157, "unexpected Pawn opcode ABI");
