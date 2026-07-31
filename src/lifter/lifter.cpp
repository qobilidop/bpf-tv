// Derived from the Alive2 arm-tv branch (regehr/alive2, backend_tv/lifter.cpp).
// Copyright (c) 2018-present the Alive2 authors. MIT license.
// Reference implementation: third_party/alive2-arm-tv/backend_tv/

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

#include "lifter/bpf2llvm.h"
#include "lifter/lifter.h"
#include "lifter/mc2llvm.h"
#include "llvm_util/llvm_optimizer.h"

using namespace std;
using namespace llvm;
using namespace lifter;

namespace lifter {

void addDebugInfo(Function *srcFn,
                  unordered_map<unsigned, Instruction *> &lineMap) {
  auto &M = *srcFn->getParent();

  // start with a clean slate
  StripDebugInfo(M);

  M.setModuleFlag(Module::Warning, "Dwarf Version", dwarf::DWARF_VERSION);
  M.setModuleFlag(Module::Warning, "Debug Info Version",
                  DEBUG_METADATA_VERSION);

  auto &Ctx = srcFn->getContext();

  DIBuilder DBuilder(M);
  auto DIF = DBuilder.createFile("foo.ll", ".");
  auto CU = DBuilder.createCompileUnit(dwarf::DW_LANG_C, DIF, "bpf-tv", false,
                                       "", 0);
  auto Ty = DBuilder.createSubroutineType(DBuilder.getOrCreateTypeArray({}));
  auto SP = DBuilder.createFunction(CU, srcFn->getName(), StringRef(), DIF, 0,
                                    Ty, 0, DINode::FlagPrototyped,
                                    DISubprogram::SPFlagDefinition);
  srcFn->setSubprogram(SP);
  unsigned line = 0;
  for (auto &bb : *srcFn) {
    for (auto &i : bb) {
      lineMap[line] = &i;
      i.setDebugLoc(DILocation::get(Ctx, line, 0, SP));
      ++line;
    }
  }

  DBuilder.finalize();
  verifyModule(M);
}

SmallPtrSet<const Value *, 8> escapingStackObjects(Function &fn) {
  SmallPtrSet<const Value *, 8> escaped;
  for (auto &bb : fn) {
    for (auto &i : bb) {
      auto *cb = dyn_cast<CallBase>(&i);
      if (!cb)
        continue;
      auto *callee = cb->getCalledFunction();
      if (callee && callee->isIntrinsic())
        continue; // memcpy & friends are modeled precisely
      for (auto &arg : cb->args()) {
        if (!arg->getType()->isPointerTy())
          continue;
        const Value *obj = getUnderlyingObject(arg);
        if (isa<AllocaInst>(obj))
          escaped.insert(obj);
      }
    }
  }
  return escaped;
}

pair<Function *, Function *>
liftFunc(Function *srcFn, unique_ptr<MemoryBuffer> MB,
         std::unordered_map<unsigned, llvm::Instruction *> &lineMap,
         std::string optimize_tgt, std::ostream *out, const Target *Targ,
         llvm::Triple DefaultTT, const char *DefaultCPU,
         const char *DefaultFeatures, const std::vector<StackSlot> *slots) {
  auto lifter = make_unique<bpf2llvm>(srcFn, std::move(MB), lineMap, out, Targ,
                                      DefaultTT, DefaultCPU, DefaultFeatures,
                                      slots);

  auto [adjustedSrc, tgtFn] = lifter->run();

  auto tgtModule = tgtFn->getParent();

  *out << "\n\nabout to optimize lifted code:\n\n";
  *out << moduleToString(tgtModule) << std::endl;

  auto err = llvm_util::optimize_module(*tgtModule, optimize_tgt);
  if (!err.empty()) {
    *out << "\n\nERROR running LLVM optimizations:\n";
    *out << err << "\n";
    exit(-1);
  }

  lifter->fixupOptimizedTgt(tgtFn);

  *out << "\n\noptimized lifted code:\n\n";
  *out << moduleToString(tgtModule) << std::endl;

  return make_pair(adjustedSrc, tgtFn);
}

} // namespace lifter
