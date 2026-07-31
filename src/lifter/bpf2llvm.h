// BPF → LLVM IR lifter for bpf-tv.
// Modeled on riscv2llvm from the Alive2 arm-tv branch (regehr/alive2),
// Copyright (c) 2018-present the Alive2 authors. MIT license.
// Reference implementation: third_party/alive2-arm-tv/backend_tv/

#pragma once

/*
 * primary references for this lifter:
 *
 * RFC 9669 (the standardized BPF ISA, including runtime edge-case
 * semantics: div/mod by zero, shift masking):
 * https://www.rfc-editor.org/rfc/rfc9669.html
 *
 * the kernel's instruction-set doc:
 * https://docs.kernel.org/bpf/standardization/instruction-set.html
 *
 * LLVM's BPF target: llvm/lib/Target/BPF/ (BPFInstrInfo.td is the
 * ground truth for MCInst operand layouts)
 */

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCInst.h"

#include "lifter/lifter.h"
#include "lifter/mc2llvm.h"

#include <string>

namespace lifter {

class bpf2llvm final : public mc2llvm {
  void lift(llvm::MCInst &I) override;

  void doCall(llvm::FunctionCallee FC, llvm::CallInst *llvmCI,
              const std::string &calleeName) override;
  void doReturn() override;

  void platformInit() override;
  void checkArgSupport(llvm::Argument &arg) override;
  void checkFuncSupport(llvm::Function &func) override;
  void checkTypeSupport(llvm::Type *ty) override;
  void checkCallingConv(llvm::Function *fn) override;

  unsigned branchInst() override;
  unsigned sentinelNOP() override;

  bool isGOT(uint16_t spec) override {
    return false;
  }

  // map any register (R or W alias) to its 64-bit backing register
  unsigned mapRegToBackingReg(unsigned Reg);
  bool isWReg(unsigned Reg);

  llvm::Value *lookupReg(unsigned Reg);

  // read the full 64-bit register
  llvm::Value *readReg64(unsigned Reg);
  // read the low 32 bits of a register (through a W alias)
  llvm::Value *readReg32(unsigned Reg);

  // write a 64-bit value to a register
  void updateReg64(llvm::Value *V, unsigned Reg);
  // write a 32-bit value to a register, zeroing the upper 32 bits --
  // the BPF ALU32 semantics
  void updateReg32(llvm::Value *V, unsigned Reg);

  // operand helpers
  llvm::Value *readRegOperand64(int idx);
  llvm::Value *readRegOperand32(int idx);
  llvm::Value *readPtrRegOperand(int idx);
  llvm::Value *readImmOperand(int idx, unsigned bits);

  // reg-or-imm second source operand of an ALU instruction
  llvm::Value *readALUSrcOperand(int idx, unsigned bits);

  // pointer for a MEMri operand (base register + signed 16-bit offset)
  // occupying operand slots idx (reg) and idx+1 (imm)
  llvm::Value *getMemPointerOperand(int idx);

  // conditional-branch targets: (taken, fallthrough)
  std::tuple<llvm::BasicBlock *, llvm::BasicBlock *>
  getBranchTargetsOperand(int op);

  llvm::Value *enforceSExtZExt(llvm::Value *V, bool isSExt, bool isZExt);
  std::vector<llvm::Value *> marshallArgs(llvm::FunctionType *fTy);

  // unspecified machine state is modeled as loads from an external
  // global (contents unknown to the optimizer, havocked across calls)
  // instead of the reference's freeze(poison), which -O3 may refine
  // away -- see DECISIONS.md
  llvm::Value *unspecifiedValue(unsigned Width) override;
  llvm::GlobalVariable *unspecifiedMem{nullptr};
  uint64_t unspecifiedOff{0};
  static constexpr uint64_t unspecifiedMemBytes = 65536;

public:
  bpf2llvm(llvm::Function *srcFn, std::unique_ptr<llvm::MemoryBuffer> MB,
           std::unordered_map<unsigned, llvm::Instruction *> &lineMap,
           std::ostream *out, const llvm::Target *Targ, llvm::Triple DefaultTT,
           const char *DefaultCPU, const char *DefaultFeatures);

  /*
   * bytes-mode entry: disassemble raw BPF bytecode (e.g. from the
   * bpf_conformance harness), reconstruct the CFG, and lift -- no
   * textual assembly involved. Returns the same (src, lifted) pair as
   * run().
   */
  std::pair<llvm::Function *, llvm::Function *>
  runBytes(llvm::ArrayRef<uint8_t> bytes);
};

} // end namespace lifter
