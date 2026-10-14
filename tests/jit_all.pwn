/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The original code is copyright (c) 2026, 1therealcloud.
 */

// PawnJIT standalone open.mp AMX/JIT torture suite.

#include <open.mp>

#pragma dynamic 20000


new gPassed;
new gFailed;
new bool:gStarted;
new gLoopEffectSide;
new gGlobal = 17;
new gArray[8] = {3, 5, 7, 11, 13, 17, 19, 23};
new gNestedMarker;
new gDirectValue = 123;
new gSavedControlFrame;
new gObservedControlFrame;
new gSavedPopStack;
new gObservedPopSentinel;
new gObservedSwapPriSentinel;
new gObservedSwapAltSentinel;
new gRawSentinelAddress;
new gRawSentinelObserved;
new gRetnOddMarker;
new gRetnNegativeMarker;
new gRawAltDirect;
new gRawReferenceSlot;
new gRawPushDirect = 0x10203040;
new gDifferentialSeed;

stock Check(const name[], bool:condition)
{
    if (condition)
    {
        ++gPassed;
        printf("[PawnJIT] %-32s PASS", name);
    }
    else
    {
        ++gFailed;
        printf("[PawnJIT] %-32s FAIL", name);
    }

    return condition;
}

stock Add(a, b)
{
    return a + b;
}

stock Nested(a, b, c)
{
    return Add(Add(a, b), c);
}

stock CallStressLeaf(value)
{
    return value + 1;
}

stock CallStressMid(value)
{
    return CallStressLeaf(CallStressLeaf(value));
}

stock CallStress()
{
    new value;

    for (new i; i < 1000; ++i)
        value = CallStressMid(value);

    return value == 2000;
}

stock ReturnMonoLeaf(value)
{
    return value * 3 + 1;
}

stock ReturnMonoCaller(value)
{
    return ReturnMonoLeaf(value);
}

stock ReturnPicLeaf(value)
{
    return value * 5 + 7;
}

stock ReturnPicCallerA(value)
{
    return ReturnPicLeaf(value);
}

stock ReturnPicCallerB(value)
{
    return ReturnPicLeaf(value + 1);
}

stock ReturnPicCallerC(value)
{
    return ReturnPicLeaf(value + 2);
}

stock ReturnSpecializationOpcodes()
{
    return ReturnMonoCaller(4) == 13 &&
        ReturnPicCallerA(2) == 17 &&
        ReturnPicCallerB(2) == 22 &&
        ReturnPicCallerC(2) == 27;
}

stock Factorial(value)
{
    if (value <= 1)
        return 1;

    return value * Factorial(value - 1);
}

stock Sum(const values[], size)
{
    new result;

    for (new i; i < size; ++i)
        result += values[i];

    return result;
}

stock ChangeRef(&value, amount)
{
    value += amount;
    return value;
}

stock ReadCell(&value)
{
    return value;
}

stock ReadArrayCell(const values[], index)
{
    return values[index];
}

stock ReadDirectGlobal()
{
    return gDirectValue;
}

stock SwitchTest(value)
{
    switch (value)
    {
        case -5: return 50;
        case 0: return 100;
        case 7: return 700;
        case 100: return 1000;
    }

    return -1;
}

stock SwitchRangeTargetTest(value)
{
    switch (value)
    {
        case 1, 2, 3, 4: return 77;
        case 10: return 100;
    }
    return -1;
}

stock SwitchLargeTest(value)
{
    switch (value)
    {
        case -100: return 1;
        case -50: return 2;
        case -10: return 3;
        case 0: return 4;
        case 7: return 5;
        case 20: return 6;
        case 50: return 7;
        case 100: return 8;
        case 500, 501, 502, 503, 504: return 9;
        case 1000: return 10;
    }

    return -1;
}

stock ForLoop(limit)
{
    new result;

    for (new i; i < limit; ++i)
    {
        if ((i & 1) == 0)
            result += i;
        else
            result -= i;
    }

    return result;
}

stock WhileLoop(value)
{
    new result;

    while (value > 0)
    {
        result += value;
        --value;
    }

    return result;
}

stock LocalArray()
{
    new values[12];

    for (new i; i < sizeof(values); ++i)
        values[i] = i * i + 1;

    return values[0] + values[3] + values[7] + values[11];
}

stock VarArgs(...)
{
    return numargs();
}

stock Arithmetic(a = 10, b = 3, c = 7)
{
    if (a + b != 13)
        return 0;

    if (a - b != 7)
        return 0;

    if (a * b != 30)
        return 0;

    if (c / b != 2 || c % b != 1)
        return 0;

    if (-c / b != -3 || -c % b != 2)
        return 0;

    if (c / -b != -3 || c % -b != -2)
        return 0;

    if (-c / -b != 2 || -c % -b != -1)
        return 0;

    return 1;
}

stock UnsignedOps(value = -1, shifted = 0x40000000)
{

    if ((value >>> 1) != 0x7FFFFFFF)
        return 0;

    if ((shifted >>> 2) != 0x10000000)
        return 0;

    return 1;
}

stock Bitwise(value = 0x55AA33CC, one = 1, eight = 8, twofiftysix = 256, four = 4, zero = 0)
{

    if ((value & 0xFF) != 0xCC)
        return 0;

    if ((value | 0xF) != 0x55AA33CF)
        return 0;

    if ((value ^ 0xFF) != 0x55AA3333)
        return 0;

    if ((one << eight) != 256)
        return 0;

    if ((twofiftysix >> four) != 16)
        return 0;

    if (~zero != -1)
        return 0;

    return 1;
}

stock CmpLT(a, b)
{
    return a < b;
}

stock CmpLE(a, b)
{
    return a <= b;
}

stock CmpGT(a, b)
{
    return a > b;
}

stock CmpGE(a, b)
{
    return a >= b;
}

stock CmpEQ(a, b)
{
    return a == b;
}

stock CmpNE(a, b)
{
    return a != b;
}

stock LogicalNot(value)
{
    return !value;
}

stock Globals()
{
    new old = gGlobal;
    gGlobal += 25;

    return old == 17 &&
        gGlobal == 42 &&
        Sum(gArray, sizeof(gArray)) == 98;
}

stock References()
{
    new value = 15;
    new result = ChangeRef(value, 27);
    return value == 42 && result == 42;
}

stock Strings()
{
    new unpacked[32] = "PawnJIT";
    new copy[32];

    format(copy, sizeof(copy), "%s", unpacked);

    if (strcmp(unpacked, copy, false) != 0)
        return 0;

    if (strlen(unpacked) != 7)
        return 0;

    strcat(copy, " test");

    return strcmp(copy, "PawnJIT test", false) == 0;
}

stock Floats()
{
    new Float:a = 2.5;
    new Float:b = 3.0;
    new Float:c = a * b + 1.25;

    return floatabs(c - 8.75) < 0.0001;
}

stock NativeCalls()
{
    new maxPlayers = GetMaxPlayers();
    new tick = GetTickCount();
    return maxPlayers > 0 && tick >= 0;
}

stock SysreqPreserveAlt()
{
    new value;

    #emit CONST.alt 0x12345678
    #emit PUSH.C 0
    #emit SYSREQ.C GetTickCount
    #emit STOR.S.alt value
    #emit STACK 4

    return value == 0x12345678;
}

stock SwapPri()
{
    new result;

    #emit PUSH.C 0x11223344
    #emit CONST.pri 0x55667788
    #emit SWAP.pri
    #emit STOR.S.pri result
    #emit POP.pri

    return result == 0x11223344;
}

stock SwapAlt()
{
    new result;

    #emit PUSH.C 0x11223344
    #emit CONST.alt 0x55667788
    #emit SWAP.alt
    #emit STOR.S.alt result
    #emit POP.alt

    return result == 0x11223344;
}

stock HeapSpace()
{
    new before;
    new after;

    #emit HEAP 0
    #emit STOR.S.alt before
    #emit HEAP 16
    #emit LCTRL 2
    #emit STOR.S.pri after
    #emit HEAP -16

    return after == before + 16 && heapspace() > 0;
}

stock IndirectJump()
{
    new result;

    #emit LCTRL 6
    #emit ADD.C 28
    #emit JUMP.pri

    #emit CONST.pri 0
    #emit STOR.S.pri result

    #emit CONST.pri 1
    #emit STOR.S.pri result

    return result == 1;
}

stock Lctrl8Call()
{
    new result;

    // Historical JIT ABI path #1: AMX target through SCTRL 6, but a native
    // return address generated by LCTRL 8 is left on the Pawn stack.
    #emit PUSH.C 0
    #emit CONST.pri Lctrl8Target
    #emit MOVE.alt
    #emit LCTRL 6
    #emit ADD.C 32
    #emit LCTRL 8
    #emit PUSH.pri
    #emit MOVE.pri
    #emit SCTRL 6
    #emit STOR.S.pri result

    if (result != 12345)
        return 0;

    result = 0;

    // Historical JIT ABI path #2: both target and return address are translated
    // with LCTRL 8 and control is transferred through SCTRL 8.
    #emit PUSH.C 0
    #emit CONST.pri Lctrl8Target
    #emit LCTRL 8
    #emit MOVE.alt
    #emit LCTRL 6
    #emit ADD.C 32
    #emit LCTRL 8
    #emit PUSH.pri
    #emit MOVE.pri
    #emit SCTRL 8
    #emit STOR.S.pri result

    return result == 12345;
}

stock Lctrl8Target()
{
    return 12345;
}

stock MovsOpcode()
{
    new source[4] = {10, 20, 30, 40};
    new dest[4] = {0, 0, 0, 0};

    #emit ADDR.pri source
    #emit ADDR.alt dest
    #emit MOVS 16

    return ReadArrayCell(dest, 0) == 10 &&
        ReadArrayCell(dest, 1) == 20 &&
        ReadArrayCell(dest, 2) == 30 &&
        ReadArrayCell(dest, 3) == 40;
}

stock CmpsOpcode()
{
    new first[4] = {10, 20, 30, 40};
    new second[4] = {10, 20, 30, 40};
    new result;

    #emit ADDR.pri first
    #emit ADDR.alt second
    #emit CMPS 16
    #emit STOR.S.pri result

    return ReadCell(result) == 0;
}

stock CmpsDifferentOpcode()
{
    new first[4] = {10, 20, 30, 40};
    new second[4] = {10, 21, 30, 40};
    new result;

    #emit ADDR.pri first
    #emit ADDR.alt second
    #emit CMPS 16
    #emit STOR.S.pri result

    return ReadCell(result) != 0;
}

stock CmpsLargeOpcode()
{
    new first[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    new second[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    new result;

    #emit ADDR.pri first
    #emit ADDR.alt second
    #emit CMPS 48
    #emit STOR.S.pri result

    return ReadCell(result) == 0;
}

stock MovsOverlapLargeOpcode()
{
    new values[24];
    for (new i = 0; i < sizeof values; ++i)
        values[i] = i + 1;

    #emit ADDR.alt values
    #emit CONST.pri 4
    #emit ADD
    #emit MOVE.alt
    #emit ADDR.pri values
    #emit MOVS 80

    // Overlapping MOVS is VM-profile-specific (legacy REP MOVS vs portable
    // C memcpy). Exact memory parity is checked by PawnJIT_BulkParityTarget;
    // this smoke test only asserts profile-independent effects.
    return values[0] == 1 && values[1] == 1 && values[21] == 22;
}

stock MovsOverlapUnalignedOpcode()
{
    new values[4] = {0x04030201, 0x08070605, 0x0C0B0A09, 0x100F0E0D};

    // Destination overlaps source by one byte. Legacy ASM executes MOVSD
    // chunks before the byte tail, so a byte-at-a-time forward loop produces
    // different data. Differential comparison checks the exact data segment.
    #emit ADDR.alt values
    #emit CONST.pri 1
    #emit ADD
    #emit MOVE.alt
    #emit ADDR.pri values
    #emit MOVS 8

    return values[0] != 0;
}

stock MovsOverlapShortOpcode()
{
    new values[8];
    for (new i = 0; i < sizeof values; ++i)
        values[i] = i + 1;

    #emit ADDR.alt values
    #emit CONST.pri 4
    #emit ADD
    #emit MOVE.alt
    #emit ADDR.pri values
    #emit MOVS 16

    return values[0] == 1 && values[1] == 1 && values[5] == 6;
}

stock MovsOverlap64Opcode()
{
    new values[24];
    for (new i = 0; i < sizeof values; ++i)
        values[i] = i + 1;

    #emit ADDR.alt values
    #emit CONST.pri 4
    #emit ADD
    #emit MOVE.alt
    #emit ADDR.pri values
    #emit MOVS 64

    return values[0] == 1 && values[1] == 1 && values[17] == 18;
}

stock MovsOverlap65Opcode()
{
    new values[24];
    for (new i = 0; i < sizeof values; ++i)
        values[i] = i + 1;

    #emit ADDR.alt values
    #emit CONST.pri 4
    #emit ADD
    #emit MOVE.alt
    #emit ADDR.pri values
    #emit MOVS 65

    return values[0] == 1 && values[1] == 1 && values[23] == 24;
}

stock CmpsExactOpcode()
{
    new low[1] = {1};
    new high[1] = {255};
    new positive;
    new negative;

    #emit ADDR.pri low
    #emit ADDR.alt high
    #emit CMPS 1
    #emit STOR.S.pri positive
    #emit ADDR.pri high
    #emit ADDR.alt low
    #emit CMPS 1
    #emit STOR.S.pri negative

    return positive > 0 && negative < 0;
}

stock CmpsLargeExactOpcode()
{
    new low[20];
    new high[20];
    low[0] = 1;
    high[0] = 255;
    new positive;
    new negative;

    #emit ADDR.pri low
    #emit ADDR.alt high
    #emit CMPS 80
    #emit STOR.S.pri positive
    #emit ADDR.pri high
    #emit ADDR.alt low
    #emit CMPS 80
    #emit STOR.S.pri negative

    return positive > 0 && negative < 0;
}

stock CmpsMediumExactOpcode()
{
    new low[12];
    new high[12];
    low[0] = 1;
    high[0] = 255;
    new positive;
    new negative;

    #emit ADDR.pri low
    #emit ADDR.alt high
    #emit CMPS 48
    #emit STOR.S.pri positive
    #emit ADDR.pri high
    #emit ADDR.alt low
    #emit CMPS 48
    #emit STOR.S.pri negative

    return positive > 0 && negative < 0;
}

stock RawProfileParityProbe()
{
    new low[1] = {1};
    new high[1] = {255};
    new cmpPositive;
    new cmpNegative;

    #emit ADDR.pri low
    #emit ADDR.alt high
    #emit CMPS 1
    #emit STOR.S.pri cmpPositive

    #emit ADDR.pri high
    #emit ADDR.alt low
    #emit CMPS 1
    #emit STOR.S.pri cmpNegative

    new values[4] = {0x04030201, 0x08070605, 0x0C0B0A09, 0x100F0E0D};
    #emit ADDR.alt values
    #emit CONST.pri 1
    #emit ADD
    #emit MOVE.alt
    #emit ADDR.pri values
    #emit MOVS 8

    printf("[PROFILEPROBE] cmps1 pos=%d neg=%d", cmpPositive, cmpNegative);
    printf("[PROFILEPROBE] movs-overlap %d %d %d %d",
        values[0], values[1], values[2], values[3]);
    return 1;
}

stock RareArithmeticOpcodes()
{
    new sshrResult;
    new umulResult;
    new subResult;
    new subAltResult;
    new andResult;
    new orResult;
    new signAltResult;

    #emit CONST.pri -16
    #emit CONST.alt 2
    #emit SSHR
    #emit STOR.S.pri sshrResult
    #emit CONST.pri -1
    #emit CONST.alt 2
    #emit UMUL
    #emit STOR.S.pri umulResult
    #emit CONST.pri 20
    #emit CONST.alt 3
    #emit SUB
    #emit STOR.S.pri subResult
    #emit CONST.pri 20
    #emit CONST.alt 3
    #emit SUB.alt
    #emit STOR.S.pri subAltResult
    #emit CONST.pri 0x5A
    #emit CONST.alt 0x3C
    #emit AND
    #emit STOR.S.pri andResult
    #emit CONST.pri 0x50
    #emit CONST.alt 0x0F
    #emit OR
    #emit STOR.S.pri orResult
    #emit CONST.alt 0x80
    #emit SIGN.alt
    #emit MOVE.pri
    #emit STOR.S.pri signAltResult

    return sshrResult == -4 && umulResult == -2 &&
        subResult == 17 && subAltResult == -17 &&
        andResult == 0x18 && orResult == 0x5F && signAltResult == -128;
}

stock RareRegisterAndStackOpcodes()
{
    new xchgPri;
    new xchgAlt;
    new pushedAlt;
    new pushedLocal = 0x13572468;
    new pushedLocalResult;
    new loadedAlt;
    new nopResult;

    #emit CONST.pri 111
    #emit CONST.alt 222
    #emit XCHG
    #emit STOR.S.pri xchgPri
    #emit MOVE.pri
    #emit STOR.S.pri xchgAlt
    #emit CONST.alt 0x24681357
    #emit PUSH.alt
    #emit POP.pri
    #emit STOR.S.pri pushedAlt
    #emit PUSH.S pushedLocal
    #emit POP.pri
    #emit STOR.S.pri pushedLocalResult
    #emit CONST.alt 7654
    #emit STOR.alt gRawAltDirect
    #emit ZERO.alt
    #emit LOAD.alt gRawAltDirect
    #emit MOVE.pri
    #emit STOR.S.pri loadedAlt
    #emit CONST.pri 9876
    #emit NOP
    #emit STOR.S.pri nopResult

    return xchgPri == 222 && xchgAlt == 111 &&
        pushedAlt == 0x24681357 && pushedLocalResult == pushedLocal &&
        loadedAlt == 7654 && gRawAltDirect == 7654 && nopResult == 9876;
}

stock RareReferenceOpcodes()
{
    new target = 10;
    new readAlt;
    new readPri;

    #emit ADDR.pri target
    #emit STOR.pri gRawReferenceSlot
    #emit CONST.pri 4321
    #emit SREF.pri gRawReferenceSlot
    #emit LREF.alt gRawReferenceSlot
    #emit MOVE.pri
    #emit STOR.S.pri readAlt
    #emit CONST.alt 8765
    #emit SREF.alt gRawReferenceSlot
    #emit LREF.pri gRawReferenceSlot
    #emit STOR.S.pri readPri

    return target == 8765 && readAlt == 4321 && readPri == 8765;
}

stock RareComparisonOpcodes()
{
    new lessUnsigned;
    new greaterUnsigned;
    new lessSigned;
    new greaterSigned;
    new leqUnsigned;
    new geqUnsigned;
    new leqSigned;
    new geqSigned;

    #emit CONST.pri -1
    #emit CONST.alt 1
    #emit LESS
    #emit STOR.S.pri lessUnsigned
    #emit CONST.pri -1
    #emit CONST.alt 1
    #emit GRTR
    #emit STOR.S.pri greaterUnsigned
    #emit CONST.pri -1
    #emit CONST.alt 1
    #emit SLESS
    #emit STOR.S.pri lessSigned
    #emit CONST.pri -1
    #emit CONST.alt 1
    #emit SGRTR
    #emit STOR.S.pri greaterSigned
    #emit CONST.pri 7
    #emit CONST.alt 7
    #emit LEQ
    #emit STOR.S.pri leqUnsigned
    #emit CONST.pri 7
    #emit CONST.alt 7
    #emit GEQ
    #emit STOR.S.pri geqUnsigned
    #emit CONST.pri -7
    #emit CONST.alt -7
    #emit SLEQ
    #emit STOR.S.pri leqSigned
    #emit CONST.pri -7
    #emit CONST.alt -7
    #emit SGEQ
    #emit STOR.S.pri geqSigned

    return lessUnsigned == 0 && greaterUnsigned == 1 &&
        lessSigned == 1 && greaterSigned == 0 &&
        leqUnsigned == 1 && geqUnsigned == 1 &&
        leqSigned == 1 && geqSigned == 1;
}

stock RemainingDataOpcodes()
{
    new shlResult;
    new smulResult;
    new eqResult;
    new neqResult;
    new pushedDirect;
    new target = 10;
    new localReference;
    new localReferenceRead;

    #emit CONST.pri 3
    #emit CONST.alt 2
    #emit SHL
    #emit STOR.S.pri shlResult
    #emit CONST.pri -7
    #emit CONST.alt 6
    #emit SMUL
    #emit STOR.S.pri smulResult
    #emit CONST.pri 99
    #emit CONST.alt 99
    #emit EQ
    #emit STOR.S.pri eqResult
    #emit CONST.pri 99
    #emit CONST.alt 100
    #emit NEQ
    #emit STOR.S.pri neqResult
    #emit PUSH gRawPushDirect
    #emit POP.pri
    #emit STOR.S.pri pushedDirect

    #emit ADDR.pri target
    #emit STOR.S.pri localReference
    #emit CONST.alt 2468
    #emit SREF.S.alt localReference
    #emit LREF.S.alt localReference
    #emit MOVE.pri
    #emit STOR.S.pri localReferenceRead

    return shlResult == 12 && smulResult == -42 &&
        eqResult == 1 && neqResult == 1 && pushedDirect == gRawPushDirect &&
        target == 2468 && localReferenceRead == 2468;
}

stock MaskedShiftCountOpcodes()
{
    new a, b, c, d, e, f, g, h, i, j, k;

    #emit CONST.pri 1
    #emit CONST.alt 32
    #emit SHL
    #emit STOR.S.pri a
    #emit CONST.pri 1
    #emit CONST.alt 33
    #emit SHL
    #emit STOR.S.pri b
    #emit CONST.pri -1
    #emit CONST.alt -1
    #emit SHR
    #emit STOR.S.pri c
    #emit CONST.pri 0x80000000
    #emit CONST.alt 31
    #emit SSHR
    #emit STOR.S.pri d
    #emit CONST.pri 0x80000000
    #emit CONST.alt 32
    #emit SSHR
    #emit STOR.S.pri e
    #emit CONST.pri 0x80000000
    #emit CONST.alt 33
    #emit SSHR
    #emit STOR.S.pri f
    #emit CONST.pri 0x80000000
    #emit CONST.alt -1
    #emit SSHR
    #emit STOR.S.pri g
    #emit CONST.pri 1
    #emit SHL.C.pri 32
    #emit STOR.S.pri h
    #emit CONST.pri 1
    #emit SHL.C.pri 33
    #emit STOR.S.pri i
    #emit CONST.pri -1
    #emit SHR.C.pri -1
    #emit STOR.S.pri j
    #emit CONST.alt 0x80000000
    #emit SHR.C.alt 32
    #emit MOVE.pri
    #emit STOR.S.pri k

    return a == 1 && b == 2 && c == 1 && d == -1 &&
        e == 0x80000000 && f == 0xC0000000 && g == -1 &&
        h == 1 && i == 2 && j == 1 && k == 0x80000000;
}

stock FillOpcode()
{
    new values[4] = {1, 2, 3, 4};

    #emit CONST.pri 77
    #emit ADDR.alt values
    #emit FILL 16

    return ReadArrayCell(values, 0) == 77 &&
        ReadArrayCell(values, 1) == 77 &&
        ReadArrayCell(values, 2) == 77 &&
        ReadArrayCell(values, 3) == 77;
}

stock FillOddAndZeroOpcodes()
{
    new zero[2] = {0x11223344, 0x55667788};
    new one[2] = {0x11223344, 0x55667788};
    new three[2] = {0x11223344, 0x55667788};
    new five[2] = {0x11223344, 0x55667788};
    new seven[2] = {0x11223344, 0x55667788};

    #emit CONST.pri 0x7A6B5C4D
    #emit ADDR.alt zero
    #emit FILL 0
    #emit ADDR.alt one
    #emit FILL 1
    #emit ADDR.alt three
    #emit FILL 3
    #emit ADDR.alt five
    #emit FILL 5
    #emit ADDR.alt seven
    #emit FILL 7

    return zero[0] == 0x11223344 && zero[1] == 0x55667788 &&
        one[0] == 0x11223344 && one[1] == 0x55667788 &&
        three[0] == 0x11223344 && three[1] == 0x55667788 &&
        five[0] == 0x7A6B5C4D && five[1] == 0x55667788 &&
        seven[0] == 0x7A6B5C4D && seven[1] == 0x55667788;
}

stock ZeroLengthBulkOpcodes()
{
    new source[1] = {0x12345678};
    new dest[1] = {0x76543210};
    new compareResult = 99;

    #emit ADDR.pri source
    #emit ADDR.alt dest
    #emit MOVS 0
    #emit ADDR.pri source
    #emit ADDR.alt dest
    #emit CMPS 0
    #emit STOR.S.pri compareResult

    return source[0] == 0x12345678 && dest[0] == 0x76543210 &&
        compareResult == 0;
}

// Small branch-free helpers below are intentionally shaped to satisfy the
// V37 inliner policy. They cover the exact clone paths added in V37.2.
stock InlineV373Movs(const source[], dest[])
{
    #emit LOAD.S.pri source
    #emit LOAD.S.alt dest
    #emit MOVS 3
    return 1;
}

stock InlineV373Cmps(const first[], const second[])
{
    new result;

    #emit LOAD.S.pri first
    #emit LOAD.S.alt second
    #emit CMPS 4
    #emit STOR.S.pri result

    return result;
}

stock InlineV373Fill(values[])
{
    #emit CONST.pri 0x12345678
    #emit LOAD.S.alt values
    #emit FILL 4
    return 1;
}

stock InlineV373Reference(&value)
{
    new reference;
    new result;

    #emit LOAD.S.pri value
    #emit STOR.S.pri reference
    #emit CONST.pri 5678
    #emit SREF.S.pri reference
    #emit LREF.S.pri reference
    #emit STOR.S.pri result

    return result;
}

stock InlineV373IncDecI(&value)
{
    #emit LOAD.S.pri value
    #emit INC.I
    #emit DEC.I
    #emit INC.I
    return 1;
}

stock InlineV373Divide()
{
    new result;

    #emit CONST.pri -7
    #emit CONST.alt 3
    #emit SDIV
    #emit STOR.S.pri result

    return result;
}

stock InlineV373Heap()
{
    new before;
    new after;

    #emit HEAP 0
    #emit STOR.S.alt before
    #emit HEAP 4
    #emit HEAP -4
    #emit STOR.S.alt after

    return after - before;
}

stock InlineV373StackAlt()
{
    new before;
    new after;

    #emit LCTRL 4
    #emit STOR.S.pri before
    #emit STACK -8
    #emit STOR.S.alt after
    #emit STACK 8

    return after - before;
}

// Overwrite RETN's params[0]. A JIT must reject the huge cleanup before it can
// wrap the physical Pawn ESP outside the VM stack.
stock InlineV373BadRetnCleanup()
{
    #emit CONST.pri 0x7FFFFFFC
    #emit STOR.S.pri 8
    return 1;
}

stock RetnOddCleanupTarget()
{
    gRetnOddMarker = 111;
    #emit CONST.pri 1
    #emit STOR.S.pri 8
    return 111;
}

stock RetnNegativeCleanupTarget()
{
    gRetnNegativeMarker = 222;
    #emit CONST.pri -4
    #emit STOR.S.pri 8
    return 222;
}

stock RetnUnalignedSignedCleanup()
{
    RetnOddCleanupTarget();
    #emit STACK -1

    RetnNegativeCleanupTarget();
    #emit STACK 4

    return gRetnOddMarker == 111 && gRetnNegativeMarker == 222;
}

stock InlineV373CmpsZeroInvalid()
{
    #emit CONST.pri -1
    #emit CONST.alt -1
    #emit CMPS 0
    return 1;
}

// V38: small acyclic CFG helpers. These deliberately contain forward
// conditional/unconditional branches and should now be cloned at the CALL
// site instead of being rejected by the straight-line V37 policy.
stock InlineV38IfElse(value)
{
    new result;

    if (value == 7)
        result = 100;
    else
        result = 200;

    return result + value;
}

stock InlineV38SignedBranch(value)
{
    new result = value;

    if (result < 0)
        result = -result;

    return result + 3;
}

stock InlineV38NestedLeaf(value)
{
    if (value == 0)
        value = 9;

    return value + 1;
}

stock InlineV38Nested(value)
{
    return InlineV38NestedLeaf(value) + 5;
}

stock InlineV38EarlyReturn(value)
{
    if (value == 1)
        return 11;
    if (value == 2)
        return 22;
    return 33;
}

stock IncDec()
{
    new value = 10;

    #emit INC.S value
    #emit INC.S value
    #emit DEC.S value

    return value == 11;
}

stock BoundsSafe()
{
    new value = 5;

    #emit LOAD.S.pri value
    #emit BOUNDS 5

    return 1;
}

stock LrefOpcode()
{
    new value = 1234;
    new reference;
    new readback;

    #emit ADDR.pri value
    #emit STOR.S.pri reference
    #emit LREF.S.pri reference
    #emit STOR.S.pri readback

    return ReadCell(readback) == 1234;
}

stock SrefOpcode()
{
    new value = 1234;
    new reference;

    #emit ADDR.pri value
    #emit STOR.S.pri reference
    #emit CONST.pri 5678
    #emit SREF.S.pri reference

    return ReadCell(value) == 5678;
}

stock LoadIOpcode()
{
    new value = 0x11223344;
    new readback;

    #emit ADDR.pri value
    #emit LOAD.I
    #emit STOR.S.pri readback

    return ReadCell(readback) == 0x11223344;
}

stock StorIOpcode()
{
    new target;

    #emit ADDR.alt target
    #emit CONST.pri 0x55667788
    #emit STOR.I

    return ReadCell(target) == 0x55667788;
}

stock LodbOpcode()
{
    new value = 0x11223344;
    new readback;

    #emit ADDR.pri value
    #emit LODB.I 1
    #emit STOR.S.pri readback

    return ReadCell(readback) == 0x44;
}

stock StrbOpcode()
{
    new target;

    #emit ADDR.alt target
    #emit CONST.pri 0xAA
    #emit STRB.I 1

    return ReadCell(target) == 0xAA;
}

stock InvalidByteWidthOpcodes()
{
    new source = 0x11223344;
    new target = 0x55667788;
    new expected;
    new readback;

    #emit ADDR.pri source
    #emit STOR.S.pri expected
    #emit LODB.I 3
    #emit STOR.S.pri readback

    #emit ADDR.alt target
    #emit CONST.pri 0xAABBCCDD
    #emit STRB.I 3

    // Portable C treats unsupported widths as no-ops. Legacy ASM32 maps
    // LODB.I 3 through its mask table (zero) and STRB.I 3 to a 16-bit store.
    printf("[PROFILEPROBE] bytewidth lodb3=%d strb3=%d", ReadCell(readback), ReadCell(target));
    return (ReadCell(readback) == ReadCell(expected) && ReadCell(target) == 0x55667788) ||
        (ReadCell(readback) == 0 && ReadCell(target) == 0x5566CCDD);
}

stock ByteWidthParityProbe()
{
    new source = 0x11223344;
    new target = 0x55667788;
    new readback;

    #emit ADDR.pri source
    #emit LODB.I 3
    #emit STOR.S.pri readback

    #emit ADDR.alt target
    #emit CONST.pri 0xAABBCCDD
    #emit STRB.I 3

    // Return the raw profile-specific state. Differential testing compares it
    // against the host interpreter instead of accepting either profile branch.
    return ReadCell(readback) ^ ReadCell(target);
}

stock InvalidControlRegisters()
{
    new first;
    new second;

    #emit CONST.pri 1234
    #emit LCTRL 12345
    #emit STOR.S.pri first

    #emit CONST.pri 5678
    #emit SCTRL 12345
    #emit STOR.S.pri second

    // Portable C leaves PRI unchanged for an unknown LCTRL selector.
    // Legacy ASM32 falls through to LCTRL 6 and returns the current CIP. Do
    // not print the raw legacy pointer because ASLR makes it process-specific;
    // print only the stable profile classification for differential logs.
    printf("[PROFILEPROBE] invalid-lctrl profile=%d sctrl=%d",
        ReadCell(first) == 1234 ? 2 : 1, ReadCell(second));
    return ReadCell(first) > 0 && ReadCell(second) == 5678;
}

stock UnalignedControlRegisters()
{
    new originalStack;
    new observedStack;

    #emit LCTRL 4
    #emit STOR.S.pri originalStack
    #emit ADD.C -1
    #emit SCTRL 4
    #emit LCTRL 4
    #emit STOR.S.pri observedStack
    #emit LOAD.S.pri originalStack
    #emit SCTRL 4

    #emit LCTRL 5
    #emit STOR.pri gSavedControlFrame
    #emit ADD.C 1
    #emit SCTRL 5
    #emit LCTRL 5
    #emit STOR.pri gObservedControlFrame
    #emit LOAD.pri gSavedControlFrame
    #emit SCTRL 5

    return originalStack - observedStack == 1 &&
        gObservedControlFrame - gSavedControlFrame == 1;
}

stock InvalidAlignWidths()
{
    new first;
    new second;

    #emit CONST.pri 0x12345678
    #emit ALIGN.pri -1
    #emit STOR.S.pri first

    #emit CONST.alt 0x55667788
    #emit ALIGN.alt -1
    #emit STOR.S.alt second

    printf("[PROFILEPROBE] align-1 pri=%d alt=%d", ReadCell(first), ReadCell(second));

    // Legacy ASM32 and GNU-threaded portable apply XOR(4 - operand). The
    // ANSI/switch portable interpreter casts the operand to size_t first, so
    // a negative width is ignored. Both are real reference-VM behaviours.
    new bool:applied = ReadCell(first) == 0x1234567D && ReadCell(second) == 0x5566778D;
    new bool:ignored = ReadCell(first) == 0x12345678 && ReadCell(second) == 0x55667788;
    return applied || ignored;
}

stock IndexOpcodes()
{
    new values[4] = {10, 20, 30, 40};
    new first;
    new second;
    new third;

    #emit CONST.pri 2
    #emit ADDR.alt values
    #emit LIDX
    #emit STOR.S.pri first

    #emit CONST.pri 3
    #emit ADDR.alt values
    #emit LIDX.B 2
    #emit STOR.S.pri second

    #emit CONST.pri 1
    #emit ADDR.alt values
    #emit IDXADDR
    #emit LOAD.I
    #emit STOR.S.pri third

    return first == 30 && second == 40 && third == 20;
}

stock WideIndexShiftOpcodes()
{
    new values[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    new first;
    new second;

    #emit CONST.pri 1
    #emit ADDR.alt values
    #emit LIDX.B 4
    #emit STOR.S.pri first

    #emit CONST.pri 1
    #emit ADDR.alt values
    #emit IDXADDR.B 4
    #emit LOAD.I
    #emit STOR.S.pri second

    return first == 50 && second == 50;
}

stock MaskedIndexShiftOpcodes()
{
    new values[2] = {111, 222};
    new lidx32;
    new lidx33;
    new address32;
    new address33;
    new base;

    // The x86 AMX core masks shift counts just like SHL r32,CL/immediate.
    #emit CONST.pri 4
    #emit ADDR.alt values
    #emit LIDX.B 32
    #emit STOR.S.pri lidx32

    #emit CONST.pri 2
    #emit ADDR.alt values
    #emit LIDX.B 33
    #emit STOR.S.pri lidx33

    #emit ADDR.pri values
    #emit STOR.S.pri base

    #emit CONST.pri 4
    #emit ADDR.alt values
    #emit IDXADDR.B 32
    #emit STOR.S.pri address32

    #emit CONST.pri 2
    #emit ADDR.alt values
    #emit IDXADDR.B 33
    #emit STOR.S.pri address33

    return lidx32 == 222 && lidx33 == 222 &&
        address32 - base == 4 && address33 - base == 4;
}

stock PushROpcode()
{
    new a;
    new b;
    new c;

    #emit CONST.pri 321
    #emit PUSH.R 3
    #emit POP.pri
    #emit STOR.S.pri a
    #emit POP.pri
    #emit STOR.S.pri b
    #emit POP.pri
    #emit STOR.S.pri c

    return a == 321 && b == 321 && c == 321;
}

stock LargePushROpcode()
{
    new before;
    new after;
    new first;

    #emit LCTRL 4
    #emit STOR.S.pri before
    #emit CONST.pri 654321
    #emit PUSH.R 4097
    #emit LCTRL 4
    #emit STOR.S.pri after
    #emit POP.pri
    #emit STOR.S.pri first
    #emit STACK 16384

    return before - after == 16388 && first == 654321;
}

stock ScalarHeapBoundaryAccess()
{
    new base;

    #emit HEAP 4
    #emit STOR.S.alt base
    #emit CONST.pri 0x11223344
    #emit STOR.I
    #emit LOAD.S.pri base
    #emit ADD.C 3
    #emit LOAD.I
    #emit HEAP -4

    // LOAD.I begins in the final allocated heap byte and intentionally spans
    // into the heap/stack gap. Legacy AMX validates only the start address.
    return 1;
}

stock StackOpcode()
{
    new before;
    new after;

    #emit LCTRL 4
    #emit STOR.S.pri before
    #emit STACK -8
    #emit LCTRL 4
    #emit STOR.S.pri after
    #emit STACK 8

    return after == before - 8;
}

stock StackAltOpcode()
{
    new before;
    new after;

    #emit LCTRL 4
    #emit STOR.S.pri before
    #emit STACK -8
    #emit STOR.S.alt after
    #emit STACK 8

    // STACK writes the pre-adjustment STK to ALT.
    return after == before;
}

stock PopTopSentinelOpcode()
{
    #emit LCTRL 4
    #emit STOR.pri gSavedPopStack
    #emit LCTRL 3
    #emit SCTRL 4
    #emit POP.pri
    #emit STOR.pri gObservedPopSentinel
    #emit LOAD.pri gSavedPopStack
    #emit SCTRL 4

    return gObservedPopSentinel == 0;
}

stock SwapTopSentinelOpcodes()
{
    // SWAP.pri/alt use raw [STK] access in both reference VMs. STP has the
    // same zero sentinel already exercised by PopTopSentinelOpcode(). Restore
    // the cell after each exchange so the test leaves the VM unchanged.
    #emit LCTRL 4
    #emit STOR.pri gSavedPopStack
    #emit LCTRL 3
    #emit SCTRL 4

    #emit CONST.pri 0x13572468
    #emit SWAP.pri
    #emit STOR.pri gObservedSwapPriSentinel
    #emit SWAP.pri

    #emit CONST.alt 0x24681357
    #emit SWAP.alt
    #emit STOR.alt gObservedSwapAltSentinel
    #emit SWAP.alt

    #emit LOAD.pri gSavedPopStack
    #emit SCTRL 4

    return gObservedSwapPriSentinel == 0 && gObservedSwapAltSentinel == 0;
}

stock RawTopSentinelOpcodes()
{
    // LREF/SREF and INC.I/DEC.I are raw data-block accesses in the reference
    // VM: unlike LOAD.I/STOR.I, they may legally touch the dedicated cell at
    // AMX::stp. Exercise that exact top boundary and restore the sentinel to 0.
    #emit LCTRL 3
    #emit STOR.pri gRawSentinelAddress

    #emit CONST.pri 0x31415926
    #emit SREF.pri gRawSentinelAddress
    #emit LREF.pri gRawSentinelAddress
    #emit STOR.pri gRawSentinelObserved

    #emit CONST.pri 0
    #emit SREF.pri gRawSentinelAddress

    #emit LCTRL 3
    #emit INC.I
    #emit LCTRL 3
    #emit DEC.I

    #emit LREF.pri gRawSentinelAddress
    #emit ADD.C 0x31415926
    #emit STOR.pri gRawSentinelAddress

    return gRawSentinelObserved == 0x31415926 && gRawSentinelAddress == 0x31415926;
}

stock ControlRegisters()
{
    new hea;
    new stk;
    new frm;
    new flags;

    #emit LCTRL 2
    #emit STOR.S.pri hea
    #emit LCTRL 4
    #emit STOR.S.pri stk
    #emit LCTRL 5
    #emit STOR.S.pri frm
    #emit LCTRL 9
    #emit STOR.S.pri flags

    #emit LOAD.S.pri hea
    #emit SCTRL 2
    #emit LOAD.S.pri stk
    #emit SCTRL 4
    #emit LOAD.S.pri frm
    #emit SCTRL 5

    return hea > 0 && stk > hea && frm > 0 && flags != 0;
}

stock ShiftImmediateOpcodes()
{
    new a;
    new b;
    new c;
    new d;

    #emit CONST.pri 3
    #emit SHL.C.pri 4
    #emit STOR.S.pri a

    #emit CONST.alt 48
    #emit SHR.C.alt 4
    #emit STOR.S.alt b

    #emit CONST.alt 5
    #emit SHL.C.alt 3
    #emit STOR.S.alt c

    #emit CONST.pri 64
    #emit SHR.C.pri 3
    #emit STOR.S.pri d

    return a == 48 && b == 3 && c == 40 && d == 8;
}

stock PeepholeOpcodes()
{
    new a;
    new b;
    new c;

    #emit CONST.pri 123
    #emit ADD.C 0
    #emit SMUL.C 1
    #emit SHL.C.pri 0
    #emit SHR.C.pri 0
    #emit STOR.S.pri a

    #emit CONST.pri 77
    #emit SMUL.C 0
    #emit STOR.S.pri b

    #emit CONST.pri 9
    #emit SMUL.C -1
    #emit STOR.S.pri c

    return a == 123 && b == 0 && c == -9;
}

stock DivideOpcodes()
{
    new q1;
    new r1;
    new q2;
    new r2;
    new q3;
    new r3;
    new q4;
    new r4;

    // SDIV: PRI / ALT.
    #emit CONST.pri -7
    #emit CONST.alt 3
    #emit SDIV
    #emit STOR.S.pri q1
    #emit STOR.S.alt r1

    // SDIV.alt: ALT / PRI.
    #emit CONST.pri 3
    #emit CONST.alt -7
    #emit SDIV.alt
    #emit STOR.S.pri q2
    #emit STOR.S.alt r2

    // UDIV: PRI / ALT.
    #emit CONST.pri 10
    #emit CONST.alt 3
    #emit UDIV
    #emit STOR.S.pri q3
    #emit STOR.S.alt r3

    // UDIV.alt: ALT / PRI.
    #emit CONST.pri 3
    #emit CONST.alt 10
    #emit UDIV.alt
    #emit STOR.S.pri q4
    #emit STOR.S.alt r4

    printf("[DIVPROBE] SDIV     -7 /  3 -> PRI=%d ALT=%d", q1, r1);
    printf("[DIVPROBE] SDIV.alt -7 /  3 -> PRI=%d ALT=%d", q2, r2);
    printf("[DIVPROBE] UDIV     10 /  3 -> PRI=%d ALT=%d", q3, r3);
    printf("[DIVPROBE] UDIV.alt 10 /  3 -> PRI=%d ALT=%d", q4, r4);

    // There are two intentional reference-VM profiles. Legacy ASM32 uses
    // the canonical floored remainder for SDIV. Current portable amx.c has a
    // historical OP_SDIV quirk where its preliminary remainder is computed
    // from the already-overwritten quotient; SDIV.ALT is canonical in both.
    new bool:legacy =
        q1 == -3 && r1 == 2 &&
        q2 == -3 && r2 == 2 &&
        q3 == 3 && r3 == 1 &&
        q4 == 3 && r4 == 1;
    new bool:portable =
        q1 == -3 && r1 == 1 &&
        q2 == -3 && r2 == 2 &&
        q3 == 3 && r3 == 1 &&
        q4 == 3 && r4 == 1;

    return legacy || portable;
}

stock DivideSafeEdgeOpcodes()
{
    new q1;
    new r1;
    new q2;
    new r2;

    // Both reference VMs finish with Pawn's floor-correction step, but
    // portable OP_SDIV starts from its quotient-based remainder quirk.
    #emit CONST.pri 7
    #emit CONST.alt -3
    #emit SDIV
    #emit STOR.S.pri q1
    #emit STOR.S.alt r1

    #emit CONST.pri -7
    #emit CONST.alt -3
    #emit SDIV
    #emit STOR.S.pri q2
    #emit STOR.S.alt r2

    printf("[DIVPROBE] SDIV      7 / -3 -> PRI=%d ALT=%d", q1, r1);
    printf("[DIVPROBE] SDIV     -7 / -3 -> PRI=%d ALT=%d", q2, r2);

    new bool:legacy =
        q1 == -3 && r1 == -2 &&
        q2 == 2 && r2 == -1;
    new bool:portable =
        q1 == -2 && r1 == -2 &&
        q2 == 1 && r2 == -1;

    return legacy || portable;
}

stock DivideProfileQuirkOpcodes()
{
    new q1;
    new r1;
    new q2;
    new r2;
    new q3;
    new r3;

    // Positive values make the portable OP_SDIV remainder quirk especially
    // obvious: 10/3 becomes q=3,r=0 and 7/3 becomes q=2,r=2 there, while
    // legacy ASM32 produces the mathematical remainders 1 and 1.
    #emit CONST.pri 10
    #emit CONST.alt 3
    #emit SDIV
    #emit STOR.S.pri q1
    #emit STOR.S.alt r1

    #emit CONST.pri 7
    #emit CONST.alt 3
    #emit SDIV
    #emit STOR.S.pri q2
    #emit STOR.S.alt r2

    // SDIV.ALT is canonical in both profiles and acts as a control.
    #emit CONST.pri 3
    #emit CONST.alt 10
    #emit SDIV.alt
    #emit STOR.S.pri q3
    #emit STOR.S.alt r3

    printf("[DIVPROFILE] SDIV 10/3=%d r=%d, SDIV 7/3=%d r=%d, SDIV.alt 10/3=%d r=%d",
        q1, r1, q2, r2, q3, r3);

    new bool:legacy =
        q1 == 3 && r1 == 1 &&
        q2 == 2 && r2 == 1 &&
        q3 == 3 && r3 == 1;
    new bool:portable =
        q1 == 3 && r1 == 0 &&
        q2 == 2 && r2 == 2 &&
        q3 == 3 && r3 == 1;

    return legacy || portable;
}

// Deliberately not called by the automatic suite.
// INT_MIN / -1 can trap in some x86 AMX interpreter implementations,
// which would abort the process before the remaining differential tests
// and benchmark can run.
stock DivideOverflowEdgeOpcode()
{
    new q;
    new r;

    #emit CONST.pri 0x80000000
    #emit CONST.alt -1
    #emit SDIV
    #emit STOR.S.pri q
    #emit STOR.S.alt r

    printf("[DIVPROBE] SDIV INT_MIN / -1 -> PRI=%d ALT=%d", q, r);
    return q == 0x80000000 && r == 0;
}

stock SignAndEqOpcodes()
{
    new sign;
    new eq1;
    new eq2;

    #emit CONST.pri 0x80
    #emit SIGN.pri
    #emit STOR.S.pri sign

    #emit CONST.pri 123
    #emit EQ.C.pri 123
    #emit STOR.S.pri eq1

    #emit CONST.alt 456
    #emit EQ.C.alt 456
    #emit STOR.S.pri eq2

    return sign == -128 && eq1 == 1 && eq2 == 1;
}

stock DirectIncOpcode()
{
    new old = ReadDirectGlobal();

    #emit INC gDirectValue

    new ok = ReadDirectGlobal() == old + 1;
    gDirectValue = old;
    return ok;
}

stock DirectDecOpcode()
{
    new old = ReadDirectGlobal();

    #emit DEC gDirectValue

    new ok = ReadDirectGlobal() == old - 1;
    gDirectValue = old;
    return ok;
}

stock DirectZeroOpcode()
{
    new old = ReadDirectGlobal();

    #emit ZERO gDirectValue

    new ok = ReadDirectGlobal() == 0;
    gDirectValue = old;
    return ok;
}

stock PushAdrOpcode()
{
    new value;
    new fromPush;
    new fromAddr;

    #emit PUSH.ADR value
    #emit POP.pri
    #emit STOR.S.pri fromPush

    #emit ADDR.pri value
    #emit STOR.S.pri fromAddr

    return fromPush == fromAddr;
}

#if 0
// pawncc resolves these raw cross-function operands to a function entry. A
// branch there is not a valid same-frame control-flow test because it executes
// the callee PROC without a CALL return slot. Keep the experiment excluded.
stock RawBranchTarget()
{
    // A raw branch resolves to the target function's PROC. Compensate for
    // that extra frame so RETN returns as a tail branch from the source test.
    #emit LCTRL 5
    #emit ADD.C 4
    #emit SCTRL 5
    #emit SCTRL 4
    #emit CONST.pri 1
    #emit RETN
}

stock RawRetTarget()
{
    #emit CONST.pri 24680
    #emit RET
}

stock RawJumpTaken()
{
    #emit JUMP RawBranchTarget
    return 0;
}

stock RawJeqTaken()
{
    #emit CONST.pri 7
    #emit CONST.alt 7
    #emit JEQ RawBranchTarget
    return 0;
}

stock RawJeqFallthrough()
{
    #emit CONST.pri 7
    #emit CONST.alt 8
    #emit JEQ RawBranchTarget
    return 1;
}

stock RawJneqTaken()
{
    #emit CONST.pri 7
    #emit CONST.alt 8
    #emit JNEQ RawBranchTarget
    return 0;
}

stock RawJneqFallthrough()
{
    #emit CONST.pri 7
    #emit CONST.alt 7
    #emit JNEQ RawBranchTarget
    return 1;
}

stock RawJlessTaken()
{
    #emit CONST.pri 1
    #emit CONST.alt -1
    #emit JLESS RawBranchTarget
    return 0;
}

stock RawJlessFallthrough()
{
    #emit CONST.pri -1
    #emit CONST.alt 1
    #emit JLESS RawBranchTarget
    return 1;
}

stock RawJleqTaken()
{
    #emit CONST.pri -1
    #emit CONST.alt -1
    #emit JLEQ RawBranchTarget
    return 0;
}

stock RawJleqFallthrough()
{
    #emit CONST.pri -1
    #emit CONST.alt 1
    #emit JLEQ RawBranchTarget
    return 1;
}

stock RawJgrtrTaken()
{
    #emit CONST.pri -1
    #emit CONST.alt 1
    #emit JGRTR RawBranchTarget
    return 0;
}

stock RawJgrtrFallthrough()
{
    #emit CONST.pri 1
    #emit CONST.alt -1
    #emit JGRTR RawBranchTarget
    return 1;
}

stock RawJgeqTaken()
{
    #emit CONST.pri -1
    #emit CONST.alt -1
    #emit JGEQ RawBranchTarget
    return 0;
}

stock RawJgeqFallthrough()
{
    #emit CONST.pri 1
    #emit CONST.alt -1
    #emit JGEQ RawBranchTarget
    return 1;
}

stock RawJslessTaken()
{
    #emit CONST.pri -1
    #emit CONST.alt 1
    #emit JSLESS RawBranchTarget
    return 0;
}

stock RawJslessFallthrough()
{
    #emit CONST.pri 1
    #emit CONST.alt -1
    #emit JSLESS RawBranchTarget
    return 1;
}

stock RawJsleqTaken()
{
    #emit CONST.pri -1
    #emit CONST.alt -1
    #emit JSLEQ RawBranchTarget
    return 0;
}

stock RawJsleqFallthrough()
{
    #emit CONST.pri 1
    #emit CONST.alt -1
    #emit JSLEQ RawBranchTarget
    return 1;
}

stock RawJsgrtrTaken()
{
    #emit CONST.pri 1
    #emit CONST.alt -1
    #emit JSGRTR RawBranchTarget
    return 0;
}

stock RawJsgrtrFallthrough()
{
    #emit CONST.pri -1
    #emit CONST.alt 1
    #emit JSGRTR RawBranchTarget
    return 1;
}

stock RawJsgeqTaken()
{
    #emit CONST.pri -1
    #emit CONST.alt -1
    #emit JSGEQ RawBranchTarget
    return 0;
}

stock RawJsgeqFallthrough()
{
    #emit CONST.pri -1
    #emit CONST.alt 1
    #emit JSGEQ RawBranchTarget
    return 1;
}

stock RawConditionalJumpOpcodes()
{
    return RawJumpTaken() &&
        RawJeqTaken() && RawJeqFallthrough() &&
        RawJneqTaken() && RawJneqFallthrough() &&
        RawJlessTaken() && RawJlessFallthrough() &&
        RawJleqTaken() && RawJleqFallthrough() &&
        RawJgrtrTaken() && RawJgrtrFallthrough() &&
        RawJgeqTaken() && RawJgeqFallthrough() &&
        RawJslessTaken() && RawJslessFallthrough() &&
        RawJsleqTaken() && RawJsleqFallthrough() &&
        RawJsgrtrTaken() && RawJsgrtrFallthrough() &&
        RawJsgeqTaken() && RawJsgeqFallthrough();
}
#endif

stock RawRetTarget()
{
    #emit CONST.pri 24680
    #emit RET
}

stock RawCallAndRetOpcodes()
{
    new result;

    #emit PUSH.C 0
    #emit CONST.pri RawRetTarget
    #emit CALL.pri
    #emit STACK 4
    #emit STOR.S.pri result

    return result == 24680;
}

stock CallPriOpcode()
{
    new result;

    #emit PUSH.C 0
    #emit CONST.pri CallPriTarget
    #emit CALL.pri
    #emit STOR.S.pri result

    return result == 2468;
}

stock ConstantFlowCallPriOpcode()
{
    new result;

    #emit PUSH.C 0
    #emit CONST.pri CallPriTarget
    #emit ADD.C 0
    #emit CALL.pri
    #emit STOR.S.pri result

    return result == 2468;
}

stock CallPriTarget()
{
    return 2468;
}

stock JrelOpcode()
{
    new result = 0;

    #emit JREL 8
    #emit CONST.pri 999
    #emit CONST.pri 1357
    #emit STOR.S.pri result

    return result == 1357;
}

stock LivenessTrackingOpcodes()
{
    new source = 11;
    new priResult;
    new altResult;
    new popResult;

    // The LOAD is observable only through the following arithmetic chain.
    // The entire chain becomes dead once CONST.pri overwrites PRI.
    #emit LOAD.S.pri source
    #emit ADD.C 9
    #emit SMUL.C 7
    #emit INVERT
    #emit CONST.pri 77
    #emit STOR.S.pri priResult

    // Same idea for ALT.
    #emit CONST.alt 5
    #emit SHL.C.alt 3
    #emit INC.alt
    #emit CONST.alt 123
    #emit STOR.S.alt altResult

    // Stack-mutating opcodes are liveness barriers because their error paths
    // expose VM state. Keep this here to verify the optimizer does not eat POP.
    #emit PUSH.C 999
    #emit POP.pri
    #emit CONST.pri 321
    #emit STOR.S.pri popResult

    return priResult == 77 && altResult == 123 && popResult == 321;
}


stock CfgLivenessOpcodes()
{
    new result;

    // V19 stopped liveness at JREL. V20 follows the direct CFG edge and can
    // prove the incoming PRI chain dead because the target overwrites PRI.
    #emit CONST.pri 5
    #emit ADD.C 7
    #emit SMUL.C 3
    #emit INVERT
    #emit JREL 8
    #emit CONST.pri 999
    #emit CONST.pri 321
    #emit STOR.S.pri result

    return result == 321;
}


stock CfgConstantTraceOpcodes()
{
    new result;

    // V21 follows known constants across multiple direct CFG edges. The two
    // skipped CONST.pri instructions remain valid cold entries, but the normal
    // path can fold ADD.C and SMUL.C without materializing PRI between jumps.
    #emit CONST.pri 3
    #emit JREL 8
    #emit CONST.pri 999
    #emit ADD.C 4
    #emit JREL 8
    #emit CONST.pri 999
    #emit SMUL.C 5
    #emit STOR.S.pri result

    return result == 35;
}

stock JumpThreadingOpcodes()
{
    new result;

    // V31 codegen threads direct JREL/JUMP chains. The normal path crosses two
    // JRELs; both skipped CONST instructions remain valid canonical cold entries.
    #emit CONST.pri 77
    #emit JREL 8
    #emit CONST.pri 111
    #emit JREL 8
    #emit CONST.pri 222
    #emit STOR.S.pri result

    return result == 77;
}


stock ValueNumberingOpcodes()
{
    new source = 0x13579BDF;
    new result;

    // V22 starts with unknown PRI. MOVE.alt gives PRI/ALT the same symbolic
    // value, so XOR collapses to zero without knowing the runtime value. The
    // symbolic fact survives a direct CFG edge and turns the rest into 41.
    #emit LOAD.S.pri source
    #emit MOVE.alt
    #emit XOR
    #emit JREL 8
    #emit CONST.pri 999
    #emit NOT
    #emit ADD.C 40
    #emit STOR.S.pri result

    return result == 41;
}

stock StoreForwardingOpcodes()
{
    new priSlot;
    new altSlot;
    new crossSlot;
    new zeroSlot = 99;
    new priResult;
    new altResult;
    new crossResult;
    new zeroResult = -1;
    new globalResult = -1;
    new oldGlobal = ReadDirectGlobal();

    // V23 keeps the store side effect but forwards the later load from the
    // stored symbolic value. The normal path crosses a direct CFG edge.
    #emit CONST.pri 1234
    #emit STOR.S.pri priSlot
    #emit ADD.C 99
    #emit JREL 8
    #emit CONST.pri 999
    #emit LOAD.S.pri priSlot
    #emit STOR.S.pri priResult

    #emit CONST.alt 5678
    #emit STOR.S.alt altSlot
    #emit DEC.alt
    #emit JREL 8
    #emit CONST.alt 999
    #emit LOAD.S.alt altSlot
    #emit STOR.S.alt altResult

    // Forward a PRI store into ALT. This requires one register
    // materialization but still avoids the local-memory reload.
    #emit CONST.pri 2468
    #emit STOR.S.pri crossSlot
    #emit CONST.alt 7
    #emit ADD.C 11
    #emit INVERT
    #emit LOAD.S.alt crossSlot
    #emit STOR.S.alt crossResult

    // ZERO/ZERO.S are tracked as constant stores as well.
    #emit ZERO.S zeroSlot
    #emit CONST.pri 123
    #emit INVERT
    #emit LOAD.S.pri zeroSlot
    #emit STOR.S.pri zeroResult

    #emit ZERO gDirectValue
    #emit CONST.pri 55
    #emit INVERT
    #emit LOAD.pri gDirectValue
    #emit STOR.S.pri globalResult

    gDirectValue = oldGlobal;

    return priSlot == 1234 && priResult == 1234 &&
        altSlot == 5678 && altResult == 5678 &&
        crossSlot == 2468 && crossResult == 2468 &&
        zeroSlot == 0 && zeroResult == 0 &&
        globalResult == 0;
}

stock MemoryValueTrackingOpcodes()
{
    new firstSlot = 99;
    new secondSlot = 99;
    new result = -1;

    // V25 tracks several frame slots in one symbolic memory trace. The first
    // secondSlot store becomes dead inside the trace, both later loads are
    // forwarded, and the known values survive one direct CFG edge.
    #emit ZERO.S firstSlot
    #emit CONST.pri 11
    #emit STOR.S.pri secondSlot
    #emit CONST.pri 22
    #emit STOR.S.pri secondSlot
    #emit JREL 8
    #emit CONST.pri 999
    #emit LOAD.S.pri firstSlot
    #emit LOAD.S.alt secondSlot
    #emit ADD
    #emit STOR.S.pri result

    // Stop the memory trace at an error-capable stack operation. The canonical
    // path then continues normally and keeps the optimizer proof local.
    #emit PUSH.C 0
    #emit POP.pri

    return firstSlot == 0 && secondSlot == 22 && result == 22;
}

stock MemoryMergeFacts(condition)
{
    new firstSlot = 99;
    new secondSlot = 99;
    new result = -1;

    // V26 proves that both sides of this runtime branch arrive at the same CFG
    // merge with identical memory facts. The diamond can therefore use one
    // fast path, and both loads after the merge are forwarded from those facts.
    if (condition)
    {
        #emit ZERO.S firstSlot
        #emit ZERO.S secondSlot
    }
    else
    {
        #emit ZERO.S firstSlot
        #emit ZERO.S secondSlot
    }

    #emit LOAD.S.pri firstSlot
    #emit LOAD.S.alt secondSlot
    #emit ADD
    #emit ADD.C 77
    #emit STOR.S.pri result

    return firstSlot == 0 && secondSlot == 0 && result == 77;
}


stock MemoryMergeIntersection(condition)
{
    new commonSlot = 99;
    new takenOnly = 11;
    new fallthroughOnly = 22;
    new result = -1;

    // V27 preserves the runtime branch because each arm has a different side
    // effect, but intersects commonSlot=0 at the merge and forwards its load.
    if (condition)
    {
        #emit ZERO.S commonSlot
        #emit CONST.pri 111
        #emit STOR.S.pri takenOnly
        #emit ZERO.pri
    }
    else
    {
        #emit ZERO.S commonSlot
        #emit CONST.pri 222
        #emit STOR.S.pri fallthroughOnly
        #emit ZERO.pri
    }

    #emit LOAD.S.pri commonSlot
    #emit ADD.C 7
    #emit STOR.S.pri result

    if (condition)
        return commonSlot == 0 && takenOnly == 111 && fallthroughOnly == 22 && result == 7;
    return commonSlot == 0 && takenOnly == 11 && fallthroughOnly == 222 && result == 7;
}


stock BranchStoreDse(condition)
{
    new commonSlot = 99;
    new deadSlot = -1;
    new result = -1;

    // V28 keeps the runtime diamond, but both branch-specific writes to
    // deadSlot are dead: the merge path overwrites deadSlot before anything
    // can observe either 111 or 222. commonSlot remains the intersection fact
    // that lets the memory-merge fast path form.
    if (condition)
    {
        #emit ZERO.S commonSlot
        #emit CONST.pri 111
        #emit STOR.S.pri deadSlot
        #emit ZERO.pri
    }
    else
    {
        #emit ZERO.S commonSlot
        #emit CONST.pri 222
        #emit STOR.S.pri deadSlot
        #emit ZERO.pri
    }

    #emit CONST.pri 333
    #emit STOR.S.pri deadSlot
    #emit LOAD.S.pri commonSlot
    #emit ADD.C 7
    #emit STOR.S.pri result

    return commonSlot == 0 && deadSlot == 333 && result == 7;
}


stock NestedMemoryMerge(outerCondition, innerCondition)
{
    new commonSlot = 99;
    new nestedSlot = 88;
    new result = -1;

    // V29 collapses the exact inner diamond while tracing the taken arm of
    // the outer diamond. The outer merge can then see the same final memory
    // facts on both paths and forward both loads after the second merge.
    if (outerCondition)
    {
        if (innerCondition)
        {
            #emit ZERO.S commonSlot
            #emit CONST.pri 5
            #emit STOR.S.pri nestedSlot
            #emit ZERO.pri
        }
        else
        {
            #emit ZERO.S commonSlot
            #emit CONST.pri 5
            #emit STOR.S.pri nestedSlot
            #emit ZERO.pri
        }
    }
    else
    {
        #emit ZERO.S commonSlot
        #emit CONST.pri 5
        #emit STOR.S.pri nestedSlot
        #emit ZERO.pri
    }

    #emit LOAD.S.pri commonSlot
    #emit LOAD.S.alt nestedSlot
    #emit ADD
    #emit ADD.C 72
    #emit STOR.S.pri result

    return commonSlot == 0 && nestedSlot == 5 && result == 77;
}


stock NestedMemoryIntersection(outerCondition, innerCondition)
{
    new commonSlot = 99;
    new deadSlot = 88;
    new result = -1;

    // V30 allows the inner arms to disagree on deadSlot as long as that
    // branch-specific value is proven dead after the inner merge. commonSlot
    // is intersected, deadSlot's 111/222 stores disappear, and the later 333
    // store becomes the value that reaches the outer merge on both paths.
    if (outerCondition)
    {
        if (innerCondition)
        {
            #emit ZERO.S commonSlot
            #emit CONST.pri 111
            #emit STOR.S.pri deadSlot
            #emit ZERO.pri
        }
        else
        {
            #emit ZERO.S commonSlot
            #emit CONST.pri 222
            #emit STOR.S.pri deadSlot
            #emit ZERO.pri
        }

        #emit CONST.pri 333
        #emit STOR.S.pri deadSlot
        #emit ZERO.pri
    }
    else
    {
        #emit ZERO.S commonSlot
        #emit CONST.pri 333
        #emit STOR.S.pri deadSlot
        #emit ZERO.pri
    }

    #emit LOAD.S.pri commonSlot
    #emit LOAD.S.alt deadSlot
    #emit ADD
    #emit STOR.S.pri result

    return commonSlot == 0 && deadSlot == 333 && result == 333;
}


stock RedundantStoreElimination(condition)
{
    new slot;
    new localResult;
    new globalResult;
    new oldGlobal = ReadDirectGlobal();

    // V24 proves the first local store dead only if every direct CFG path
    // overwrites the same slot before anything can observe it. Both sides of
    // this normal Pawn if/else kill the initial 111.
    #emit CONST.pri 111
    #emit STOR.S.pri slot

    if (condition)
        slot = 222;
    else
        slot = 333;

    localResult = slot;

    // Straight-line global overwrite is the minimal DSE case. The 444 store
    // must disappear while the later 555 store remains observable.
    #emit CONST.pri 444
    #emit STOR.pri gDirectValue
    #emit CONST.pri 555
    #emit STOR.pri gDirectValue
    #emit LOAD.pri gDirectValue
    #emit STOR.S.pri globalResult

    gDirectValue = oldGlobal;

    return localResult == (condition ? 222 : 333) && globalResult == 555;
}

stock ConstantPropagationOpcodes()
{
    new arithmetic;
    new shift;
    new compare;

    #emit CONST.pri 7
    #emit ADD.C 5
    #emit SMUL.C 3
    #emit INVERT
    #emit INVERT
    #emit INC.pri
    #emit DEC.pri
    #emit STOR.S.pri arithmetic

    #emit CONST.pri -1
    #emit CONST.alt 1
    #emit SHR
    #emit ADD.C 1
    #emit NEG
    #emit STOR.S.pri shift

    #emit CONST.pri 10
    #emit CONST.alt 20
    #emit SLESS
    #emit NOT
    #emit NOT
    #emit STOR.S.pri compare

    return arithmetic == 36 &&
        shift == 0x80000000 &&
        compare == 1;
}

stock ConstantPropagationColdEntry()
{
    new result;

    // Jump directly into the middle of a sequence that has a constant
    // superblock at its normal entry. The cold bytecode CIP must still see
    // the live ALT=10 instead of the skipped CONST.alt 99 value.
    #emit CONST.alt 10
    #emit LCTRL 6
    #emit ADD.C 20
    #emit JUMP.pri

    #emit CONST.alt 99
    #emit MOVE.pri
    #emit ADD.C 5
    #emit STOR.S.pri result

    return result == 15;
}

stock LoopOptimizerRmw()
{
    new value = 1;
    new checksum;

    for (new i; i < 64; ++i)
    {
        #emit LOAD.S.pri value
        #emit ADD.C 3
        #emit STOR.S.pri value

        #emit LOAD.S.pri value
        #emit SMUL.C 2
        #emit STOR.S.pri value

        checksum ^= value;
    }

    return value == -6 && checksum == -1431655770;
}

stock LoopOptimizerCounter()
{
    new counter = 100;
    new iterations;

    do
    {
        ++iterations;
        --counter;
    }
    while (counter != 0);

    return counter == 0 && iterations == 100;
}

stock LoopOptimizerCompare(limit)
{
    new i;
    new result;

    while (i < limit)
    {
        result += i + 1;
        ++i;
    }

    return result == (limit * (limit + 1)) / 2;
}


stock LoopInductionFusion()
{
    new i;
    new limit = 64;
    new sum;

    // Use normal Pawn control flow here. pawncc 3.10.x does not resolve
    // source code labels passed as operands to #emit JZER/JUMP.
    while (++i < limit)
        sum += i;

    return i == 64 && sum != 0;
}

stock LoopMemoryCompareFusion()
{
    new i = 1;
    new limit = 20;
    new hits;

    while (i < limit)
    {
        ++hits;
        ++i;
    }

    return hits == 19;
}

stock LoopBoundsIndexFusion()
{
    new values[4] = {10, 20, 30, 40};
    new i;
    new total;
    while (i < 4)
    {
        // Normal Pawn array access is retained as a runtime parity check. The
        // compiler's generated bytecode gives V34 the LOAD/BOUNDS/CONST/LIDX
        // shape when bounds metadata is present.
        total += values[i];
        ++i;
    }
    return total == 100;
}

stock LoopRangeBoundsElimination()
{
    new values[4] = {10, 20, 30, 40};
    new i;
    new current;
    new total;

    while (i < 4)
    {
        #emit LOAD.S.pri i
        #emit BOUNDS 3
        #emit ADDR.alt values
        #emit LIDX
        #emit INC.S i
        #emit STOR.S.pri current
        total += current;
    }

    return i == 4 && total == 100;
}

stock LoopRangeAddressFusion()
{
    new values[4] = {7, 11, 13, 17};
    new i;
    new current;
    new total;

    while (i < 4)
    {
        #emit LOAD.S.pri i
        #emit BOUNDS 3
        #emit ADDR.alt values
        #emit IDXADDR
        #emit INC.S i
        #emit LOAD.I
        #emit STOR.S.pri current
        total += current;
    }

    return i == 4 && total == 48;
}

stock LoopRangeMemoryBound()
{
    new values[4] = {2, 4, 8, 16};
    new i;
    new limit = 4;
    new current;
    new total;

    while (i < limit)
    {
        #emit LOAD.S.pri i
        #emit BOUNDS 3
        #emit ADDR.alt values
        #emit LIDX
        #emit INC.S i
        #emit STOR.S.pri current
        total += current;
    }

    return i == 4 && limit == 4 && total == 30;
}


stock LoopEffectPureHelper(value)
{
    return (value * 3) ^ 0x55;
}

stock LoopEffectGlobalHelper(value)
{
    gLoopEffectSide = value + 17;
    return value ^ 0x33;
}

stock LoopRangeDirectCallEffects()
{
    new values[4] = {3, 6, 9, 12};
    new i;
    new current;
    new total;

    while (i < 4)
    {
        #emit LOAD.S.pri i
        #emit BOUNDS 3
        #emit ADDR.alt values
        #emit LIDX
        #emit STOR.S.pri current
        total += LoopEffectPureHelper(current);
        ++i;
    }

    return i == 4 && total == ((3 * 3 ^ 0x55) + (6 * 3 ^ 0x55) + (9 * 3 ^ 0x55) + (12 * 3 ^ 0x55));
}

stock LoopRangeDirectCallGlobalWrite()
{
    new values[4] = {5, 7, 11, 13};
    new i;
    new current;
    new total;
    gLoopEffectSide = 0;

    while (i < 4)
    {
        #emit LOAD.S.pri i
        #emit BOUNDS 3
        #emit ADDR.alt values
        #emit LIDX
        #emit STOR.S.pri current
        total += LoopEffectGlobalHelper(current);
        ++i;
    }

    return i == 4 && gLoopEffectSide == 30 &&
        total == ((5 ^ 0x33) + (7 ^ 0x33) + (11 ^ 0x33) + (13 ^ 0x33));
}

stock LoopGuardBoundsAcrossLateNative()
{
    new values[4] = {1, 2, 4, 8};
    new i;
    new current;
    new total;

    while (i < 4)
    {
        #emit LOAD.S.pri i
        #emit BOUNDS 3
        #emit ADDR.alt values
        #emit LIDX
        #emit STOR.S.pri current
        total += current;
        ++i;

        // The native is deliberately after the BOUNDS use. V34 rejected the
        // entire SCC as clobbered; V35's guard-local fact remains valid for the
        // already checked part of this iteration.
        new tick = GetTickCount();
        #pragma unused tick
    }

    return i == 4 && total == 15;
}

stock CallBenchLeaf(value)
{
    return (value * 33 + 17) ^ (value >>> 5);
}

stock CallBenchMid(value, salt)
{
    return CallBenchLeaf(CallBenchLeaf(value ^ salt));
}

// LCTRL is intentionally present so PawnJIT's conservative inliner leaves
// this as a real CALL/PROC/RETN path. The value itself is discarded; the
// benchmark isolates direct call/return overhead without relying on a plugin
// test native.
stock CallBenchRawLeaf(value)
{
    #emit LCTRL 4
    return value + 1;
}

forward PawnJIT_CallBenchmarkTarget();
forward PawnJIT_CfgCallBenchmarkTarget();
forward PawnJIT_ParityStressTarget();
forward PawnJIT_DifferentialFuzzTarget();
forward PawnJIT_LoopBenchmarkTarget();
forward PawnJIT_BenchmarkTarget();
forward PawnJIT_EmptyPublic();

public PawnJIT_CallBenchmarkTarget()
{
    new value = 0x2468ACE;

    for (new i; i < 5000; ++i)
        value = CallBenchMid(value, i);

    return value;
}

stock CallBenchCfgLeaf(value, salt)
{
    new mixed = value ^ salt;

    if (mixed & 1)
        mixed = mixed * 3 + 1;
    else
        mixed += 7;

    return mixed ^ (mixed >>> 5);
}

public PawnJIT_CfgCallBenchmarkTarget()
{
    new value = 0x10203040;

    for (new i; i < 5000; ++i)
        value = CallBenchCfgLeaf(value, i);

    return value;
}

public PawnJIT_EmptyPublic()
{
    return 1;
}

public PawnJIT_ParityStressTarget()
{
    new rngState = 0x13579BDF;
    new accumulator = 0x2468ACE0;
    new values[16];

    for (new i; i < sizeof values; ++i)
        values[i] = (i + 1) * 0x10203;

    for (new i; i < 2048; ++i)
    {
        rngState = rngState * 1664525 + 1013904223;
        new index = (rngState >>> 28) & 15;
        new value = values[index];
        new shift = (rngState >>> 23) & 31;
        new rotated = (value << shift) | (value >>> ((32 - shift) & 31));
        new divisor = ((rngState >>> 16) & 0x7FFF) + 1;

        if (rngState & 1)
            accumulator += rotated;
        else
            accumulator -= rotated;

        accumulator ^= rngState / divisor;
        accumulator += rngState % divisor;

        switch (rngState & 7)
        {
            case 0: accumulator ^= 0x55AA55AA;
            case 1: accumulator += value;
            case 2: accumulator -= index;
            case 3: accumulator = ~accumulator;
            case 4: accumulator ^= accumulator >>> 11;
            case 5: accumulator += i * 3;
            case 6: accumulator -= rngState >>> 9;
            default: accumulator ^= rngState << 3;
        }

        values[index] = (value ^ accumulator) + i;
    }

    for (new i; i < sizeof values; ++i)
        accumulator = (accumulator * 33) ^ values[i];

    return accumulator ^ rngState;
}


forward PawnJIT_SysreqSelfPatchTarget();
public PawnJIT_SysreqSelfPatchTarget()
{
    new accumulator;
    for (new i; i < 2; ++i)
    {
        #emit PUSH.C 0
        #emit SYSREQ.C GetTickCount
        #emit STACK 4
        #emit ADD.C 1
        #emit STOR.S.pri accumulator
    }
    return accumulator != 0;
}


stock PawnJIT_InlineAuditLeaf(value)
{
    new local = value + 3;
    return local * 5;
}

stock PawnJIT_InlineAuditMid(value)
{
    return PawnJIT_InlineAuditLeaf(value) + 7;
}

forward PawnJIT_InlineParityTarget();
public PawnJIT_InlineParityTarget()
{
    return PawnJIT_InlineAuditMid(11) + PawnJIT_InlineAuditMid(-9);
}

forward PawnJIT_BulkParityTarget();
public PawnJIT_BulkParityTarget()
{
    return MovsOpcode() &&
        MovsOverlapUnalignedOpcode() &&
        MovsOverlapShortOpcode() &&
        MovsOverlap64Opcode() &&
        MovsOverlap65Opcode() &&
        MovsOverlapLargeOpcode() &&
        CmpsOpcode() &&
        CmpsDifferentOpcode() &&
        CmpsExactOpcode() &&
        CmpsLargeExactOpcode() &&
        FillOpcode() &&
        FillOddAndZeroOpcodes() &&
        ZeroLengthBulkOpcodes();
}


public PawnJIT_DifferentialFuzzTarget()
{
    new rngState = gDifferentialSeed;
    new accumulator = rngState ^ 0x6D2B79F5;
    new values[32];

    for (new i; i < sizeof values; ++i)
        values[i] = (rngState + i * 0x10203) ^ (i << (i & 15));

    for (new i; i < 512; ++i)
    {
        rngState = rngState * 1664525 + 1013904223;
        new index = (rngState >>> 27) & 31;
        new other = (index + 13) & 31;
        new shift = (rngState >>> 21) & 31;
        new divisor = ((rngState >>> 16) & 0x7FFF) + 1;
        new value = values[index];

        if (rngState & 1)
            accumulator = (accumulator + value) ^ (value << shift);
        else
            accumulator = (accumulator - value) ^ (value >>> shift);

        accumulator += rngState / divisor;
        accumulator ^= rngState % divisor;

        switch ((rngState >>> 3) & 15)
        {
            case 0: accumulator = ~accumulator;
            case 1: accumulator += values[other];
            case 2: accumulator -= values[other];
            case 3: accumulator ^= values[other];
            case 4: accumulator |= value;
            case 5: accumulator &= value;
            case 6: accumulator = -accumulator;
            case 7: accumulator += i * 33;
            case 8: accumulator ^= rngState >>> 11;
            case 9: accumulator += rngState << 5;
            case 10: accumulator -= rngState >>> 7;
            case 11: accumulator ^= value * 17;
            case 12: if (index <= other) accumulator += 1;
            case 13: if (value > accumulator) accumulator -= 1;
            case 14: if (value == rngState) accumulator ^= 1;
            default: if (value != rngState) accumulator += 1;
        }

        values[index] = accumulator + values[other] + i;
    }

    for (new i; i < sizeof values; ++i)
        accumulator = (accumulator * 33) ^ values[i];

    gDifferentialSeed = rngState;
    return accumulator;
}


public PawnJIT_LoopBenchmarkTarget()
{
    new value = 0x13579BDF;
    new accumulator;

    for (new i; i < 10000; ++i)
    {
        // Deterministic RMW shapes for V34 loop fusion/strength reduction.
        #emit LOAD.S.pri value
        #emit ADD.C 3
        #emit STOR.S.pri value

        #emit LOAD.S.pri value
        #emit SMUL.C 2
        #emit STOR.S.pri value

        accumulator ^= value + i;
    }

    // Separate memory-bound induction loop. This intentionally keeps `limit`
    // in a local cell so V34 can benchmark the new memory compare path.
    new counter;
    new limit = 2000;
    while (++counter < limit)
        accumulator += counter & 31;

    new sample[8] = {3, 5, 7, 11, 13, 17, 19, 23};
    for (new j; j < sizeof(sample); ++j)
        accumulator ^= sample[j] * (j + 1);

    // V34 canonical range loop: the interpreter executes BOUNDS every trip,
    // while the JIT can prove rangeIndex is 0..7, remove BOUNDS, use the local
    // ADDR.alt base directly and fold the post-access INC.S into the index path.
    new rangeIndex;
    new rangeValue;
    while (rangeIndex < 8)
    {
        #emit LOAD.S.pri rangeIndex
        #emit BOUNDS 7
        #emit ADDR.alt sample
        #emit LIDX
        #emit INC.S rangeIndex
        #emit STOR.S.pri rangeValue
        accumulator ^= rangeValue + rangeIndex;
    }

    return value ^ accumulator ^ counter ^ rangeIndex;
}

public PawnJIT_BenchmarkTarget()
{
    new value = 0x13579BDF;

    for (new i; i < 10000; ++i)
    {
        value = value * 33 + i;
        value ^= value >>> 7;
        value += i & 31;
    }

    return value;
}

stock RunImmediate()
{
    printf("");
    printf("================ PawnJIT TORTURE V54.3 ================");

    Check("arithmetic", Arithmetic() != 0);
    Check("unsigned shift", UnsignedOps() != 0);
    Check("bitwise", Bitwise() != 0);

    Check("signed <", CmpLT(-10, 20) != 0);
    Check("signed <=", CmpLE(-10, 20) != 0);
    Check("signed >", CmpGT(20, -10) != 0);
    Check("signed >=", CmpGE(20, -10) != 0);
    Check("equal", CmpEQ(-10, -10) != 0);
    Check("not equal", CmpNE(-10, 20) != 0);
    Check("logical not zero", LogicalNot(0) != 0);
    Check("logical not nonzero", LogicalNot(5) == 0);

    Check("locals/calls", Nested(10, 20, 12) == 42);
    Check("call stress", CallStress() != 0);
    Check("return specialization", ReturnSpecializationOpcodes() != 0);
    Check("recursion", Factorial(7) == 5040);
    Check("for loop", ForLoop(10) == -5);
    Check("while loop", WhileLoop(10) == 55);
    Check("switch negative", SwitchTest(-5) == 50);
    Check("switch hit", SwitchTest(7) == 700);
    Check("switch default", SwitchTest(123) == -1);
    Check("switch range target",
        SwitchRangeTargetTest(1) == 77 && SwitchRangeTargetTest(4) == 77 &&
        SwitchRangeTargetTest(5) == -1 && SwitchRangeTargetTest(10) == 100);
    Check("switch tree hit", SwitchLargeTest(500) == 9);
    Check("switch tree range",
        SwitchLargeTest(501) == 9 && SwitchLargeTest(504) == 9 &&
        SwitchLargeTest(505) == -1);
    Check("switch tree default", SwitchLargeTest(123) == -1);

    new values[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    Check("array argument", Sum(values, sizeof(values)) == 36);
    Check("local array", LocalArray() == 183);
    Check("globals", Globals() != 0);
    Check("references", References() != 0);
    Check("strings", Strings() != 0);
    Check("floats", Floats() != 0);
    Check("native calls", NativeCalls() != 0);
    Check("numargs 0", VarArgs() == 0);
    Check("numargs 5", VarArgs(1, 2, 3, 4, 5) == 5);
    Check("sysreq preserves ALT", SysreqPreserveAlt() != 0);
    Check("swap PRI", SwapPri() != 0);
    Check("swap ALT", SwapAlt() != 0);
    Check("heap", HeapSpace() != 0);
    Check("indirect jump", IndirectJump() != 0);
    Check("movs", MovsOpcode() != 0);
    Check("movs unaligned overlap", MovsOverlapUnalignedOpcode() != 0);
    Check("movs short overlap", MovsOverlapShortOpcode() != 0);
    Check("movs inline boundary overlap", MovsOverlap64Opcode() != 0);
    Check("movs helper boundary overlap", MovsOverlap65Opcode() != 0);
    Check("movs large overlap", MovsOverlapLargeOpcode() != 0);
    Check("cmps", CmpsOpcode() != 0);
    Check("cmps different", CmpsDifferentOpcode() != 0);
    Check("cmps sign", CmpsExactOpcode() != 0);
    Check("cmps helper fallback", CmpsLargeOpcode() != 0);
    Check("cmps helper sign", CmpsLargeExactOpcode() != 0);
    Check("cmps medium sign", CmpsMediumExactOpcode() != 0);
    Check("fill", FillOpcode() != 0);
    Check("fill odd/zero lengths", FillOddAndZeroOpcodes() != 0);
    Check("zero-length movs/cmps", ZeroLengthBulkOpcodes() != 0);
    Check("rare arithmetic opcodes", RareArithmeticOpcodes() != 0);
    Check("rare register/stack opcodes", RareRegisterAndStackOpcodes() != 0);
    Check("rare direct references", RareReferenceOpcodes() != 0);
    Check("rare comparison opcodes", RareComparisonOpcodes() != 0);
    Check("remaining data opcodes", RemainingDataOpcodes() != 0);
    Check("masked shift counts", MaskedShiftCountOpcodes() != 0);

    new movsSource[1] = {0x44332211};
    new movsDest[1] = {0x7F000000};
    Check("inline movs", InlineV373Movs(movsSource, movsDest) != 0 && movsDest[0] == 0x7F332211);

    new cmpsLow[1] = {0x10};
    new cmpsHigh[1] = {0xF0};
    Check("inline cmps positive", InlineV373Cmps(cmpsLow, cmpsHigh) > 0);
    Check("inline cmps negative", InlineV373Cmps(cmpsHigh, cmpsLow) < 0);
    Check("retn signed/unaligned cleanup", RetnUnalignedSignedCleanup() != 0);

    new inlineFill[1] = {0};
    Check("inline fill", InlineV373Fill(inlineFill) != 0 && inlineFill[0] == 0x12345678);

    new inlineReference = 10;
    Check("inline lref/sref", InlineV373Reference(inlineReference) == 5678 && inlineReference == 5678);

    new inlineIndirect = 40;
    Check("inline inc.i/dec.i", InlineV373IncDecI(inlineIndirect) != 0 && inlineIndirect == 41);
    Check("inline sdiv", InlineV373Divide() == -3);
    Check("inline heap", InlineV373Heap() == 4);
    Check("inline stack ALT", InlineV373StackAlt() == 0);
    Check("inline cfg if true", InlineV38IfElse(7) == 107);
    Check("inline cfg if false", InlineV38IfElse(4) == 204);
    Check("inline cfg signed", InlineV38SignedBranch(-8) == 11);
    Check("inline cfg nested", InlineV38Nested(0) == 15 && InlineV38Nested(3) == 9);
    Check("inline cfg early return", InlineV38EarlyReturn(1) == 11 && InlineV38EarlyReturn(2) == 22 && InlineV38EarlyReturn(3) == 33);

    Check("inc/dec", IncDec() != 0);
    Check("bounds safe", BoundsSafe() != 0);
    Check("lref.s", LrefOpcode() != 0);
    Check("sref.s", SrefOpcode() != 0);
    Check("load.i", LoadIOpcode() != 0);
    Check("stor.i", StorIOpcode() != 0);
    Check("lodb.i", LodbOpcode() != 0);
    Check("strb.i", StrbOpcode() != 0);
    Check("invalid byte width", InvalidByteWidthOpcodes() != 0);
    Check("invalid control register", InvalidControlRegisters() != 0);
    Check("unaligned control registers", UnalignedControlRegisters() != 0);
    Check("invalid align profile", InvalidAlignWidths() != 0);
    RawProfileParityProbe();
    Check("lidx/idxaddr", IndexOpcodes() != 0);
    Check("lidx.b/idxaddr.b wide", WideIndexShiftOpcodes() != 0);
    Check("lidx.b/idxaddr.b masked", MaskedIndexShiftOpcodes() != 0);
    Check("push.r", PushROpcode() != 0);
    Check("push.r large count", LargePushROpcode() != 0);
    Check("scalar heap boundary", ScalarHeapBoundaryAccess() != 0);
    Check("stack opcode", StackOpcode() != 0);
    Check("stack ALT", StackAltOpcode() != 0);
    Check("pop top sentinel", PopTopSentinelOpcode() != 0);
    Check("swap top sentinel", SwapTopSentinelOpcodes() != 0);
    Check("raw top sentinel", RawTopSentinelOpcodes() != 0);
    Check("control registers", ControlRegisters() != 0);
    Check("shift immediate", ShiftImmediateOpcodes() != 0);
    Check("peephole opcodes", PeepholeOpcodes() != 0);
    Check("constant propagation", ConstantPropagationOpcodes() != 0);
    Check("const-prop cold entry", ConstantPropagationColdEntry() != 0);
    Check("liveness tracking", LivenessTrackingOpcodes() != 0);
    Check("cfg liveness", CfgLivenessOpcodes() != 0);
    Check("cfg const trace", CfgConstantTraceOpcodes() != 0);
    Check("jump threading", JumpThreadingOpcodes() != 0);
    Check("loop optimizer rmw", LoopOptimizerRmw() != 0);
    Check("loop optimizer compare", LoopOptimizerCompare(100) != 0);
    Check("loop optimizer counter", LoopOptimizerCounter() != 0);
    Check("loop induction fusion", LoopInductionFusion() != 0);
    Check("loop memory compare", LoopMemoryCompareFusion() != 0);
    Check("loop bounds/index", LoopBoundsIndexFusion() != 0);
    Check("loop range bounds", LoopRangeBoundsElimination() != 0);
    Check("loop range address", LoopRangeAddressFusion() != 0);
    Check("loop range memory bound", LoopRangeMemoryBound() != 0);
    Check("loop range direct pure call", LoopRangeDirectCallEffects() != 0);
    Check("loop range direct global call", LoopRangeDirectCallGlobalWrite() != 0);
    Check("loop guard bounds late native", LoopGuardBoundsAcrossLateNative() != 0);
    Check("value numbering", ValueNumberingOpcodes() != 0);
    Check("store forwarding", StoreForwardingOpcodes() != 0);
    Check("memory value tracking", MemoryValueTrackingOpcodes() != 0);
    Check("memory merge false", MemoryMergeFacts(0) != 0);
    Check("memory merge true", MemoryMergeFacts(1) != 0);
    Check("memory intersection false", MemoryMergeIntersection(0) != 0);
    Check("memory intersection true", MemoryMergeIntersection(1) != 0);
    Check("branch store dse false", BranchStoreDse(0) != 0);
    Check("branch store dse true", BranchStoreDse(1) != 0);
    Check("nested memory merge 000", NestedMemoryMerge(0, 0) != 0);
    Check("nested memory merge 100", NestedMemoryMerge(1, 0) != 0);
    Check("nested memory merge 110", NestedMemoryMerge(1, 1) != 0);
    Check("nested memory intersection 000", NestedMemoryIntersection(0, 0) != 0);
    Check("nested memory intersection 100", NestedMemoryIntersection(1, 0) != 0);
    Check("nested memory intersection 110", NestedMemoryIntersection(1, 1) != 0);
    Check("redundant stores false", RedundantStoreElimination(0) != 0);
    Check("redundant stores true", RedundantStoreElimination(1) != 0);
    Check("div opcode matrix", DivideOpcodes() != 0);
    Check("div opcode safe edges", DivideSafeEdgeOpcodes() != 0);
    Check("div profile quirks", DivideProfileQuirkOpcodes() != 0);
    Check("sign/eq.c", SignAndEqOpcodes() != 0);
    Check("direct inc", DirectIncOpcode() != 0);
    Check("direct dec", DirectDecOpcode() != 0);
    Check("direct zero", DirectZeroOpcode() != 0);
    Check("push.adr", PushAdrOpcode() != 0);
    Check("raw call.pri/ret", RawCallAndRetOpcodes() != 0);
    Check("call.pri", CallPriOpcode() != 0);
    Check("constant-flow call.pri", ConstantFlowCallPriOpcode() != 0);
    Check("jrel", JrelOpcode() != 0);

    gNestedMarker = 0;
    new nestedResult = CallLocalFunction("PawnJIT_NestedPublic", "ii", 19, 23);
    Check("nested amx_Exec", nestedResult == 42 && gNestedMarker == 42);

    Check("same-exec SYSREQ.C path", PawnJIT_SysreqSelfPatchTarget() != 0);
    Check("direct inline helper", PawnJIT_InlineParityTarget() == 54);
    Check("bulk opcode parity", PawnJIT_BulkParityTarget() != 0);
    Check("deterministic stress", PawnJIT_ParityStressTarget() == 667366768);

    gDifferentialSeed = 0x12345678;
    Check("deterministic fuzz",
        PawnJIT_DifferentialFuzzTarget() == 637701902 &&
        gDifferentialSeed == 1915052152);

    printf("------------------------------------------------");
    printf("[PawnJIT] immediate: %d passed, %d failed", gPassed, gFailed);
    printf("================================================");
    printf("");

    return 1;
}

stock RunBenchmarks()
{
    new start;
    new elapsed;
    new sink;

    printf("");
    printf("================ PawnJIT BENCHMARK ================");

    start = GetTickCount();
    for (new i; i < 256; ++i)
        sink = sink * 33 + PawnJIT_BenchmarkTarget() + i;
    elapsed = GetTickCount() - start;
    printf("[PawnJIT] arithmetic x256: %d ms (sink=%d)", elapsed, sink);

    start = GetTickCount();
    for (new i; i < 128; ++i)
        sink = sink * 33 + PawnJIT_LoopBenchmarkTarget() + i;
    elapsed = GetTickCount() - start;
    printf("[PawnJIT] loops x128:      %d ms (sink=%d)", elapsed, sink);

    start = GetTickCount();
    for (new i; i < 64; ++i)
        sink = sink * 33 + PawnJIT_CallBenchmarkTarget() + i;
    elapsed = GetTickCount() - start;
    printf("[PawnJIT] calls x64:       %d ms (sink=%d)", elapsed, sink);

    start = GetTickCount();
    for (new i; i < 64; ++i)
        sink = sink * 33 + PawnJIT_CfgCallBenchmarkTarget() + i;
    elapsed = GetTickCount() - start;
    printf("[PawnJIT] cfg calls x64:   %d ms (sink=%d)", elapsed, sink);

    // Deliberately non-inlineable direct CALL/PROC/RETN hot path.
    start = GetTickCount();
    for (new i; i < 200000; ++i)
        sink = sink * 33 + CallBenchRawLeaf(i) + i;
    elapsed = GetTickCount() - start;
    printf("[PawnJIT] raw calls x200000: %d ms (sink=%d)", elapsed, sink);

    // Measures the cost of repeatedly leaving Pawn through a native and
    // re-entering the same AMX through amx_Exec for an almost empty public.
    start = GetTickCount();
    for (new i; i < 20000; ++i)
        sink = sink * 33 + CallLocalFunction("PawnJIT_EmptyPublic", "") + i;
    elapsed = GetTickCount() - start;
    printf("[PawnJIT] public reentry x20000: %d ms (sink=%d)", elapsed, sink);

    // Native-heavy path with very little Pawn work around each SYSREQ.C.
    start = GetTickCount();
    for (new i; i < 200000; ++i)
        sink = sink * 33 + GetMaxPlayers() + i;
    elapsed = GetTickCount() - start;
    printf("[PawnJIT] native bridge x200000: %d ms (sink=%d)", elapsed, sink);

    printf("================================================");
    printf("");
    return sink;
}

main()
{
    printf("[PawnJIT] sleep/CONT before");
    sleep 100;
    printf("[PawnJIT] sleep/CONT after");
    Check("sleep/CONT", true);
}

public OnGameModeInit()
{
    if (gStarted)
        return 1;

    gStarted = true;

    RunImmediate();
    RunBenchmarks();

    SetTimer("PawnJIT_Timer", 150, false);
    SetTimer("PawnJIT_Final", 350, false);
    return 1;
}

forward PawnJIT_NestedPublic(a, b);
public PawnJIT_NestedPublic(a, b)
{
    gNestedMarker = a + b;
    return gNestedMarker;
}

forward PawnJIT_Timer();
public PawnJIT_Timer()
{
    Check("timer callback", Nested(14, 14, 14) == 42);
    return 1;
}

forward PawnJIT_Final();
public PawnJIT_Final()
{
    printf("");
    printf("================ PawnJIT FINAL =====================");
    printf("[PawnJIT] %d passed, %d failed", gPassed, gFailed);
    printf("====================================================");
    printf("");
    return 1;
}
