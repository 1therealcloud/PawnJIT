/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The original code is copyright (c) 2026, 1therealcloud.
 */

#pragma once

#include "decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

constexpr cell kTopReturn = 0;

enum class JitExitKind : int
{
    None = 0,
    Halt,
    Abort,
    DirectCheck,
    ExternalError,
    CpuFault,
};

struct JitContext
{
    AMX* amx = nullptr;
    unsigned char* data = nullptr;
    void** dispatch = nullptr;
    uint32_t dispatchCount = 0;
    cell pri = 0;
    cell alt = 0;
    cell cip = 0;
    cell frm = 0;
    cell stk = 0;
    cell hea = 0;
    cell stp = 0;
    cell resetStk = 0;
    cell resetHea = 0;
    int error = AMX_ERR_NONE;
    int abortRequested = 0;
    int exitKind = static_cast<int>(JitExitKind::None);
    int unloadRequested = 0;
    int hasRetval = 0;
    int syntheticFrame = 0;
    cell entryParameterBytes = 0;
    /* scratch cell used by the generated portable signed-division sequence */
    cell callbackResult = 0;
    uint32_t hostEsp = 0;
    uint32_t hostCallEsp4 = 0;
    uint32_t hostCallEsp8 = 0;
    uint32_t hostCallEsp16 = 0;
    JitContext* parentActive = nullptr;
};

/*
    private control-flow sentinel used only between callback helpers and
    PawnJITRuntime::Execute. it must never escape through the public AMX
    API.
*/
constexpr int kInternalUnload = -0x504A54;

using JitEntry = int (*)(JitContext* context);

struct CompiledProgram
{
    unsigned char* base = nullptr;
    unsigned char* data = nullptr;
    AMX_DEBUG debugHook = nullptr;
    JitEntry entry = nullptr;
    unsigned char* code = nullptr;
    cell codeSize = 0;
    unsigned char* nativeCode = nullptr;
    std::size_t nativeCodeSize = 0;
    cell hlw = 0;
    cell stp = 0;
    cell topReturn = kTopReturn;
    std::vector<void*> dispatch;
    std::vector<cell> publicCips;
};

bool CompileProgram(AMX* amx, CompiledProgram& program);
void ReleaseProgram(CompiledProgram& program) noexcept;

int JitCallNative(JitContext* context, cell index, cell nextCip);
int JitSignedDivide(JitContext* context, int reverse);
int JitUnsignedDivide(JitContext* context, int reverse);
int JitMoveMemory(JitContext* context, cell bytes);
int JitCompareMemory(JitContext* context, cell bytes);
int JitFillMemory(JitContext* context, cell bytes);
int JitDebugBreak(JitContext* context, cell cip);

/*
    process-level fault containment is installed by the component lifecycle.
    Windows uses SEH directly at the generated entry; Linux uses a chained
    sigaction handler for synchronous generated-code faults plus the narrow
    internal MOVS/CMPS/FILL memory-helper scope. external/native/user-
    generated signals remain outside PawnJIT containment.
*/
bool InitializePawnJITFaultContainment();
bool PreparePawnJITThreadFaultContainment();
void ShutdownPawnJITFaultContainment();

class PawnJITRuntime
{
public:
    void OnLoad(AMX* amx);
    void OnUnload(AMX* amx);
    void Shutdown();
    bool Execute(AMX* amx, cell* retval, int index, int& error);

private:
    struct State
    {
        bool compileAttempted = false;
        bool headerCaptured = false;
        unsigned char* loadedBase = nullptr;
        unsigned char* loadedData = nullptr;
        AMX_HEADER loadedHeader{};
        uint32_t activeExecutions = 0;
        bool unloadPending = false;
        bool jitQuarantined = false;
        uint32_t lastCpuFault = 0;
        JitContext* activeContext = nullptr;
        std::unique_ptr<CompiledProgram> program;
    };

    State* GetState(AMX* amx) noexcept;
    bool CanExecuteJit(AMX* amx, const State& state) const;
    bool ProgramMatchesAmx(AMX* amx, const CompiledProgram& program) const;
    bool EnsureCompiled(AMX* amx, State& state);
    bool GetEntryPoint(AMX* amx, int index, cell& cip) const;
    bool PrepareContext(AMX* amx, CompiledProgram& program, int index, cell entryCip, JitContext& context) const;
    bool PrepareNestedContext(AMX* amx, CompiledProgram& program, int index, cell entryCip, JitContext& parent, JitContext& context) const;
    bool RunPreparedContext(AMX* amx, State& state, JitContext& context, cell* retval, int& error);
    void SyncAfterRun(JitContext& context, cell* retval, int error) const;

    std::unordered_map<AMX*, State> states_;
};

extern PawnJITRuntime g_pawnJITRuntime;
