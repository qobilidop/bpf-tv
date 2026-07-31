// Derived from the Alive2 arm-tv branch (regehr/alive2, backend_tv/run_backend.cpp).
// Copyright (c) 2018-present the Alive2 authors. MIT license.
// Reference implementation: third_party/alive2-arm-tv/backend_tv/

#include "lifter/lifter.h"

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

#include <cassert>
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
                                             const char *DefaultFeatures) {
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
  if (TM->addPassesToEmitFile(pass, os, nullptr, CodeGenFileType::AssemblyFile,
                              false)) {
    cerr << "\nERROR: Failed to add pass to generate assembly\n\n";
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
  return MemoryBuffer::getMemBuffer(Asm.c_str());
}
