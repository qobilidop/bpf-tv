// Derived from the Alive2 arm-tv branch (regehr/alive2, backend_tv/lifter.h).
// Copyright (c) 2018-present the Alive2 authors. MIT license.
// Reference implementation: third_party/alive2-arm-tv/backend_tv/

#pragma once

#include <ostream>
#include <string>
#include <unordered_map>

#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"

namespace llvm {
class Constant;
class Function;
} // namespace llvm

namespace lifter {

/*
 * add debug info to an IR file that will help the lifter figure out
 * which LLVM instruction each asm instruction came from
 */
void addDebugInfo(llvm::Function *srcFn,
                  std::unordered_map<unsigned, llvm::Instruction *> &lineMap);

/*
 * lower LLVM IR to textual assembly
 */
std::unique_ptr<llvm::MemoryBuffer>
generateAsm(llvm::Module &, const llvm::Target *Targ, llvm::Triple DefaultTT,
            const char *DefaultCPU, const char *DefaultFeatures);

/*
 * lift textual BPF assembly to LLVM IR. the target arguments should
 * be the same as those used to generate assembly, and lineMap should
 * come from addDebugInfo(). return value is a source/target pair that
 * is ready for an Alive2 refinement check; the source function is
 * returned because it has been rewritten, for example to implement
 * ABI checks.
 */
std::pair<llvm::Function *, llvm::Function *>
liftFunc(llvm::Function *, std::unique_ptr<llvm::MemoryBuffer>,
         std::unordered_map<unsigned, llvm::Instruction *> &lineMap,
         std::string optimize_tgt, std::ostream *out, const llvm::Target *Targ,
         llvm::Triple DefaultTT, const char *DefaultCPU,
         const char *DefaultFeatures);

/*
 * random utility function
 */
inline std::string moduleToString(llvm::Module *M) {
  std::string sss;
  llvm::raw_string_ostream ss(sss);
  M->print(ss, nullptr);
  return sss;
}

} // end namespace lifter
