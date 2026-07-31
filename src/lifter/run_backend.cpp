// Derived from the Alive2 arm-tv branch (regehr/alive2, backend_tv/run_backend.cpp).
// Copyright (c) 2018-present the Alive2 authors. MIT license.
// Reference implementation: third_party/alive2-arm-tv/backend_tv/

#include "lifter/lifter.h"

#include "llvm/Analysis/RuntimeLibcallInfo.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/CodeGen/CodeGenTargetMachineImpl.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <cassert>
#include <regex>
#include <iostream>
#include <memory>

using namespace std;
using namespace llvm;
using namespace lifter;

SmallString<1024> Asm;

void appendTargetFeatures(std::unique_ptr<llvm::Module> &MClone,
                          const char *DefaultFeatures) {
  for (llvm::Function &F : *MClone) {
    if (F.hasFnAttribute("target-features")) {
      std::string current_features =
          F.getFnAttribute("target-features").getValueAsString().str();
      F.addFnAttr("target-features", current_features + "," + DefaultFeatures);
    }
  }
}

unique_ptr<MemoryBuffer> lifter::generateAsm(Module &M, const Target *Targ,
                                             Triple DefaultTT,
                                             const char *DefaultCPU,
                                             const char *DefaultFeatures,
                                             std::vector<StackSlot> *slotsOut) {
  assert(DefaultFeatures && "[generateAsm] DefaultFeatures must be set");
  TargetOptions Opt;
  Opt.FloatABIType = llvm::FloatABI::Hard;
  auto RM = optional<Reloc::Model>();
  unique_ptr<TargetMachine> TM(Targ->createTargetMachine(
      DefaultTT, DefaultCPU, DefaultFeatures, Opt, RM));

  // we should never allow machine outlining
  TM->setMachineOutliner(false);
  TM->setSupportsDefaultOutlining(false);

  Asm = "";
  raw_svector_ostream os(Asm);

  legacy::PassManager pass;
  /*
   * replicate CodeGenTargetMachineImpl::addPassesToEmitFile minus its
   * trailing FreeMachineFunctionPass: the finalized frame layout
   * (MachineFrameInfo) must still be readable after the run so the
   * per-object stack rewrite can use it (slotsOut). MMIWP is owned by
   * the pass manager and keeps the MachineFunctions alive until the
   * end of this function.
   */
  auto &CGTM = static_cast<CodeGenTargetMachineImpl &>(*TM);
  auto *MMIWP = new MachineModuleInfoWrapperPass(TM.get());
  TargetPassConfig *PassConfig = CGTM.createPassConfig(pass);
  PassConfig->setDisableVerify(false);
  pass.add(PassConfig);
  pass.add(MMIWP);
  {
    const TargetOptions &Options = TM->Options;
    TargetLibraryInfoImpl TLII(TM->getTargetTriple(), Options.VecLib);
    pass.add(new TargetLibraryInfoWrapperPass(TLII));
    pass.add(new RuntimeLibraryInfoWrapper(
        TM->getTargetTriple(), Options.ExceptionModel, Options.FloatABIType,
        Options.EABIVersion, Options.MCOptions.ABIName, Options.VecLib));
  }
  if (PassConfig->addISelPasses()) {
    cerr << "\nERROR: Failed to add pass to generate assembly\n\n";
    exit(-1);
  }
  PassConfig->addMachinePasses();
  PassConfig->setInitialized();
  if (CGTM.addAsmPrinter(pass, os, nullptr, CodeGenFileType::AssemblyFile,
                         MMIWP->getMMI().getContext())) {
    cerr << "\nERROR: Failed to add asm printer\n\n";
    exit(-1);
  }
  /*
   * sigh... running these passes changes the module, and some of
   * these changes are non-trivial refinements
   */
  auto MClone = CloneModule(M);
  MClone->setDataLayout(TM->createDataLayout());

  if (DefaultFeatures[0] != '\0')
    appendTargetFeatures(MClone, DefaultFeatures);

  /*
   * capture backend diagnostics: e.g. the BPF backend reports "signed
   * division unsupported for this cpu" as an error diagnostic and then
   * emits garbage code (a bare exit). validating that garbage would
   * misreport a miscompilation; the right outcome is "backend error".
   */
  struct ErrorCatcher final : DiagnosticHandler {
    bool hadError = false;
    bool handleDiagnostics(const DiagnosticInfo &DI) override {
      if (DI.getSeverity() == DS_Error) {
        hadError = true;
        DiagnosticPrinterRawOStream DP(errs());
        errs() << "backend diagnostic: ";
        DI.print(DP);
        errs() << "\n";
      }
      return true; // handled; don't crash on unhandled errors
    }
  };
  auto &Ctx = MClone->getContext();
  auto oldHandler = Ctx.getDiagnosticHandler();
  auto catcherOwned = std::make_unique<ErrorCatcher>();
  auto *catcher = catcherOwned.get();
  Ctx.setDiagnosticHandler(std::move(catcherOwned));

  pass.run(*MClone.get());

  bool hadError = catcher->hadError;
  Ctx.setDiagnosticHandler(std::move(oldHandler));
  if (hadError)
    return nullptr;

  /*
   * read back the finalized frame layout: every non-dead slot that
   * originated in an IR alloca, with the r10-relative offset the
   * backend baked into the instruction stream (BPF eliminateFrameIndex
   * emits getObjectOffset directly against r10). slots whose alloca
   * lost its debug line, overlap another slot (StackColoring can merge
   * lifetime-disjoint allocas), or would fall outside the lifted
   * frame are dropped -- a dropped slot just isn't split, which the
   * driver's all-or-nothing rewrite handles by bailing.
   */
  if (slotsOut) {
    slotsOut->clear();
    for (auto &F : *MClone) {
      if (F.isDeclaration())
        continue;
      auto *MF = MMIWP->getMMI().getMachineFunction(F);
      if (!MF)
        continue;
      auto &MFI = MF->getFrameInfo();
      for (int i = 0, e = MFI.getObjectIndexEnd(); i < e; ++i) {
        if (MFI.isDeadObjectIndex(i))
          continue;
        auto *AI = MFI.getObjectAllocation(i);
        if (!AI)
          continue; // spill/scratch slots stay in the residual block
        auto DL = AI->getDebugLoc();
        int64_t size = MFI.getObjectSize(i);
        int64_t off = MFI.getObjectOffset(i);
        if (!DL || size <= 0 || off >= 0 ||
            StackFrameTopOffset + off < 0)
          continue;
        slotsOut->push_back({off, (uint64_t)size,
                             MFI.getObjectAlign(i).value(), DL->getLine()});
      }
    }
    std::sort(slotsOut->begin(), slotsOut->end(),
              [](const StackSlot &a, const StackSlot &b) {
                return a.offset < b.offset;
              });
    for (size_t i = 0; i + 1 < slotsOut->size();) {
      if (slotsOut->at(i).offset + (int64_t)slotsOut->at(i).size >
          slotsOut->at(i + 1).offset) {
        slotsOut->erase(slotsOut->begin() + i, slotsOut->begin() + i + 2);
      } else {
        ++i;
      }
    }
  }

  /*
   * LLVM's BPF asm printer emits MOV_32_64 (zext of a w register into
   * an r register) as "rN = wM", which LLVM's own BPF asm parser
   * rejects -- a printer/parser round-trip hole (as of the pinned
   * LLVM, 2026-05). At the encoding level MOV_32_64 is the same
   * machine instruction as the 32-bit register move (0xbc, BPF_ALU |
   * BPF_MOV | BPF_X), whose ISA semantics zero the upper 32 bits, so
   * rewriting "rN = wM" to "wN = wM" is a byte-level identity on the
   * emitted code. See DECISIONS.md.
   */
  std::string text = Asm.str().str();
  std::regex mov3264(R"((^|\n)(\s*)r([0-9]|10) = (w[0-9]+)\b)");
  text = std::regex_replace(text, mov3264, "$1$2w$3 = $4");
  return MemoryBuffer::getMemBufferCopy(text);
}
