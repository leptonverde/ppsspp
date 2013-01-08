// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "../../Core.h"
#include "../../CoreTiming.h"
#include "../MIPS.h"
#include "../MIPSCodeUtils.h"
#include "../MIPSInt.h"
#include "../MIPSTables.h"

#include "ArmRegCache.h"
#include "ArmJit.h"

#include "../../ext/disarm.h"

void DisassembleArm(const u8 *data, int size) {
	char temp[256];
	for (int i = 0; i < size; i += 4) {
		const u32 *codePtr = (const u32 *)(data + i);
		u32 inst = codePtr[0];
		u32 next = (i < size - 4) ? codePtr[1] : 0;
		// MAGIC SPECIAL CASE for MOVW/MOVT readability!
		if ((inst & 0x0FF00000) == 0x03000000 && (next & 0x0FF00000) == 0x03400000) {
			u32 low = ((inst & 0x000F0000) >> 4) | (inst & 0x0FFF);
			u32 hi = ((next & 0x000F0000) >> 4) | (next	 & 0x0FFF);
			int reg0 = (inst & 0x0000F000) >> 12;
			int reg1 = (next & 0x0000F000) >> 12;
			if (reg0 == reg1) {
				sprintf(temp, "%08x MOV32? %s, %04x%04x", (u32)inst, ArmRegName(reg0), hi, low);
				INFO_LOG(DYNA_REC, "A:   %s", temp);
				i += 4;
				continue;
			}
		}
		ArmDis((u32)codePtr, inst, temp);
		INFO_LOG(DYNA_REC, "A:   %s", temp);
	}
}

namespace MIPSComp
{

Jit::Jit(MIPSState *mips) : blocks(mips), gpr(mips), mips_(mips)
{ 
	blocks.Init();
	asm_.Init(mips, this);
	gpr.SetEmitter(this);
	//fpr.SetEmitter(this);
	AllocCodeSpace(1024 * 1024 * 16);
}


void Jit::FlushAll()
{
	gpr.FlushAll();
	//fpr.Flush(FLUSH_ALL);
}

void Jit::ClearCache()
{
	blocks.Clear();
	ClearCodeSpace();
}

void Jit::CompileAt(u32 addr)
{
	u32 op = Memory::Read_Instruction(addr);
	MIPSCompileOp(op);
}

void Jit::Compile(u32 em_address)
{
	ERROR_LOG(CPU, "Compile %08x", em_address);
	if (GetSpaceLeft() < 0x10000 || blocks.IsFull())
	{
		ClearCache();
	}

	int block_num = blocks.AllocateBlock(em_address);
	ArmJitBlock *b = blocks.GetBlock(block_num);
	blocks.FinalizeBlock(block_num, jo.enableBlocklink, DoJit(em_address, b));
}

void Jit::RunLoopUntil(u64 globalticks)
{
	// TODO: copy globalticks somewhere
	((void (*)())asm_.enterCode)();
	INFO_LOG(DYNA_REC, "Left asm code like a boss!");
	INFO_LOG(DYNA_REC, "or Two!");
}

const u8 *Jit::DoJit(u32 em_address, ArmJitBlock *b)
{
	js.cancel = false;
	js.blockStart = js.compilerPC = mips_->pc;
	js.downcountAmount = 0;
	js.curBlock = b;
	js.compiling = true;
	js.inDelaySlot = false;

	// We add a check before the block, used when entering from a linked block.
	b->checkedEntry = GetCodePtr();
	// Downcount flag check. The last block decremented downcounter, and the flag should still be available.
	SetCC(CC_LT);
	ARMABI_MOVI2R(R0, js.blockStart);
	MovToPC(R0);
	ARMABI_MOVI2R(R0, (u32)asm_.outerLoop);   // downcount hit zero - go advance.
	B(R0);
	SetCC(CC_AL);

	b->normalEntry = GetCodePtr();
	// TODO: this needs work
	MIPSAnalyst::AnalysisResults analysis; // = MIPSAnalyst::Analyze(em_address);

	gpr.Start(analysis);

	int numInstructions = 0;
	int cycles = 0;
// #define LOGASM
#ifdef LOGASM
	char temp[256];
#endif
	while (js.compiling)
	{
		u32 inst = Memory::Read_Instruction(js.compilerPC);
#ifdef LOGASM
		MIPSDisAsm(inst, js.compilerPC, temp, true);
		INFO_LOG(DYNA_REC, "M: %08x   %s", js.compilerPC, temp);
#endif
		js.downcountAmount += MIPSGetInstructionCycleEstimate(inst);

		MIPSCompileOp(inst);

		js.compilerPC += 4;
		numInstructions++;
	}
#ifdef LOGASM
	MIPSDisAsm(Memory::Read_Instruction(js.compilerPC), js.compilerPC, temp, true);
	INFO_LOG(DYNA_REC, "M: %08x   %s", js.compilerPC, temp);
#endif

	b->codeSize = GetCodePtr() - b->normalEntry;

#ifdef LOGASM
	DisassembleArm(b->checkedEntry, GetCodePtr() - b->checkedEntry);
#endif
	AlignCode16();

	// Don't forget to zap the instruction cache!
	FlushIcache();

	b->originalSize = numInstructions;
	return b->normalEntry;
}

void Jit::Comp_RunBlock(u32 op)
{
	// This shouldn't be necessary, the dispatcher should catch us before we get here.
	ERROR_LOG(DYNA_REC, "Comp_RunBlock");
}

void Jit::Comp_Generic(u32 op)
{
	FlushAll();
	MIPSInterpretFunc func = MIPSGetInterpretFunc(op);
	if (func)
	{
		ARMABI_MOVI2R(R0, js.compilerPC);
		MovToPC(R0);
		ARMABI_CallFunctionC((void *)func, op);
	}
}

void Jit::MovFromPC(ARMReg r) {
	LDR(r, R10, offsetof(MIPSState, pc));
}

void Jit::MovToPC(ARMReg r) {
	STR(R10, r, offsetof(MIPSState, pc));
}

void Jit::DoDownCount()
{
	ARMABI_MOVI2R(R0, (u32)&CoreTiming::downcount);
	LDR(R1, R0);
	if(js.downcountAmount < 255) // We can enlarge this if we used rotations
	{
		SUBS(R1, R1, js.downcountAmount);
		STR(R0, R1);
	} else {
		// Should be fine to use R2 here, flushed the regcache anyway.
		// If js.downcountAmount can be expressed as an Imm8, we don't need this anyway.
		ARMABI_MOVI2R(R2, js.downcountAmount);
		SUBS(R1, R1, R2);
		STR(R0, R1);
	}
}


void Jit::WriteExit(u32 destination, int exit_num)
{
	DoDownCount(); 
	//If nobody has taken care of this yet (this can be removed when all branches are done)
	ArmJitBlock *b = js.curBlock;
	b->exitAddress[exit_num] = destination;
	b->exitPtrs[exit_num] = GetWritableCodePtr();

	// Link opportunity!
	int block = blocks.GetBlockNumberFromStartAddress(destination);
	if (block >= 0 && jo.enableBlocklink) {
		// It exists! Joy of joy!
		B(blocks.GetBlock(block)->checkedEntry);
		b->linkStatus[exit_num] = true;
	} else {
		ARMABI_MOVI2R(R0, destination);
		MovToPC(R0);
		ARMABI_MOVI2R(R0, (u32)asm_.dispatcher);
		B(R0);	
	}
}

void Jit::WriteExitDestInR(ARMReg Reg) 
{
	MovToPC(Reg);
	DoDownCount();
	// TODO: shouldn't need an indirect branch here...
	ARMABI_MOVI2R(R0, (u32)asm_.dispatcher);
	B(R0);
}

void Jit::WriteSyscallExit()
{
	DoDownCount();
	ARMABI_MOVI2R(R0, (u32)asm_.dispatcherCheckCoreState);
	B(R0);
}


#define _RS ((op>>21) & 0x1F)
#define _RT ((op>>16) & 0x1F)
#define _RD ((op>>11) & 0x1F)
#define _FS ((op>>11) & 0x1F)
#define _FT ((op>>16) & 0x1F)
#define _FD ((op>>6) & 0x1F)
#define _POS	((op>>6) & 0x1F)
#define _SIZE ((op>>11) & 0x1F)

//memory regions:
//
// 08-0A
// 48-4A
// 04-05
// 44-45
// mov eax, addrreg
	// shr eax, 28
// mov eax, [table+eax]
// mov dreg, [eax+offreg]
	
}