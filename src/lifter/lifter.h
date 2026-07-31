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

#include "llvm/ADT/SmallPtrSet.h"

#include <cstdint>
#include <vector>

namespace llvm {
class Constant;
class Function;
class Value;
} // namespace llvm

namespace lifter {

// r10 (the BPF frame pointer) corresponds to this byte offset inside
// the lifted stack block; frame accesses are at negative r10 offsets
inline constexpr int64_t StackFrameTopOffset = 1024;

/*
 * one backend stack-frame slot that originated in a source-level
 * alloca: its r10-relative offset (negative), extent, and the
 * addDebugInfo line of the originating alloca. Extracted from
 * MachineFrameInfo after codegen (generateAsm) and used to carve
 * per-object blocks out of the lifted single-block stack
 * (rewriteStackObjects in bpf-tv.cpp; see DECISIONS.md).
 */
struct StackSlot {
  int64_t offset;   // r10-relative, negative
  uint64_t size;
  uint64_t align;
  unsigned srcLine; // addDebugInfo line of the originating alloca
};

/*
 * add debug info to an IR file that will help the lifter figure out
 * which LLVM instruction each asm instruction came from
 */
void addDebugInfo(llvm::Function *srcFn,
                  std::unordered_map<unsigned, llvm::Instruction *> &lineMap);

/*
 * lower LLVM IR to textual assembly. if slotsOut is non-null it
 * receives the alloca-backed stack-frame slots of the (single)
 * defined function, from MachineFrameInfo after frame finalization.
 */
std::unique_ptr<llvm::MemoryBuffer>
generateAsm(llvm::Module &, const llvm::Target *Targ, llvm::Triple DefaultTT,
            const char *DefaultCPU, const char *DefaultFeatures,
            std::vector<StackSlot> *slotsOut = nullptr);

/*
 * lift textual BPF assembly to LLVM IR. the target arguments should
 * be the same as those used to generate assembly, and lineMap should
 * come from addDebugInfo(). return value is a source/target pair that
 * is ready for an Alive2 refinement check; the source function is
 * returned because it has been rewritten, for example to implement
 * ABI checks. slots (may be null) are the frame slots from
 * generateAsm; when they cover every escaping alloca, the lifter
 * admits multi-escape functions on the promise that the driver's
 * per-object rewrite will split them.
 */
std::pair<llvm::Function *, llvm::Function *>
liftFunc(llvm::Function *, std::unique_ptr<llvm::MemoryBuffer>,
         std::unordered_map<unsigned, llvm::Instruction *> &lineMap,
         std::string optimize_tgt, std::ostream *out, const llvm::Target *Targ,
         llvm::Triple DefaultTT, const char *DefaultCPU,
         const char *DefaultFeatures,
         const std::vector<StackSlot> *slots = nullptr);

/*
 * the set of allocas whose address reaches a (non-intrinsic) call
 * argument -- the objects the single-block stack model cannot match
 * when there is more than one of them
 */
llvm::SmallPtrSet<const llvm::Value *, 8>
escapingStackObjects(llvm::Function &fn);

// the hidden --allow-stack-escape escape hatch (bpf2llvm.cpp)
bool stackEscapeAllowed();

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
