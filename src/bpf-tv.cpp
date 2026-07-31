// bpf-tv driver: translation validation for the LLVM BPF backend.
// Modeled on tools/backend-tv.cpp from the Alive2 arm-tv branch
// (regehr/alive2), Copyright (c) 2018-present the Alive2 authors.
// MIT license. Reference: third_party/alive2-arm-tv/tools/backend-tv.cpp

#include "lifter/lifter.h"
#include "cache/cache.h"
#include "llvm_util/compare.h"
#include "llvm_util/llvm2alive.h"
#include "llvm_util/utils.h"
#include "smt/smt.h"
#include "tools/transform.h"
#include "util/version.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <bit>
#include <functional>
#include <fstream>
#include <iostream>
#include <utility>

using namespace tools;
using namespace util;
using namespace std;
using namespace llvm_util;

#define LLVM_ARGS_PREFIX ""
#define ARGS_SRC_TGT
#define ARGS_REFINEMENT
#include "llvm_util/cmd_args_list.h"

namespace {

llvm::cl::opt<string> opt_file(llvm::cl::Positional,
                               llvm::cl::desc("bitcode_file"),
                               llvm::cl::Required,
                               llvm::cl::value_desc("filename"),
                               llvm::cl::cat(alive_cmdargs));

llvm::cl::opt<std::string>
    opt_fn(LLVM_ARGS_PREFIX "fn",
           llvm::cl::desc("Name of function to verify, without @ (default "
                          "= first function in the module)"),
           llvm::cl::cat(alive_cmdargs));

// named "cpu" rather than "mcpu" because llvm::codegen::RegisterCodeGenFlags
// (constructed inside alive2's optimize_module) registers "mcpu" itself
llvm::cl::opt<string> opt_cpu(
    LLVM_ARGS_PREFIX "cpu",
    llvm::cl::desc("BPF cpu to target: v1-v4 (default=v3)"),
    llvm::cl::cat(alive_cmdargs), llvm::cl::init("v3"));

llvm::cl::opt<string> opt_optimize_tgt(
    LLVM_ARGS_PREFIX "optimize-tgt",
    llvm::cl::desc("Optimize lifted code before performing translation "
                   "validation (default=O3)"),
    llvm::cl::cat(alive_cmdargs), llvm::cl::init("O3"));

llvm::cl::opt<bool> opt_skip_verification(
    LLVM_ARGS_PREFIX "skip-verification",
    llvm::cl::desc(
        "Perform lifting but skip the refinement check (default=false)"),
    llvm::cl::cat(alive_cmdargs), llvm::cl::init(false));

llvm::cl::opt<bool> opt_asm_only(
    "asm-only",
    llvm::cl::desc("Only generate assembly and exit (default=false)"),
    llvm::cl::init(false), llvm::cl::cat(alive_cmdargs));

llvm::cl::opt<string> opt_asm_output("asm-output",
                                     llvm::cl::desc("Save assembly to file "
                                                    "(default=no asm output)"),
                                     llvm::cl::cat(alive_cmdargs));

llvm::cl::opt<bool> save_lifted_ir(
    "save-lifted-ir",
    llvm::cl::desc("Save lifted LLVM IR to file (default=false)"),
    llvm::cl::init(false), llvm::cl::cat(alive_cmdargs));

llvm::cl::opt<string> opt_asm_input(
    "asm-input",
    llvm::cl::desc("Use the provided file as lifted assembly, instead of "
                   "lifting the LLVM IR. This is only for testing. "
                   "(default=no asm input)"),
    llvm::cl::cat(alive_cmdargs));

llvm::cl::opt<bool> run_replace_ptrtoint(
    "run-replace-ptrtoint",
    llvm::cl::desc(
        "Replace ptr-int round trips with single GEP (default=true)"),
    llvm::cl::init(true), llvm::cl::cat(alive_cmdargs));

llvm::ExitOnError ExitOnErr;
llvm::Triple DefaultTT;
std::string DefaultDL;
std::string DefaultCPU;
const char *DefaultFeatures = "";

// Given an intToPtr instruction, checks for a round trip from a
// ptrToInt instruction, then replaces with a single GEP instruction.
// (from the reference backend-tv.cpp; without this, lifted stack
// pointer arithmetic that flows into calls looks like a provenance
// escape and produces spurious refinement failures)
bool tryReplaceRoundTrip(llvm::IntToPtrInst *intToPtr) {
  assert(intToPtr);

  // we only understand add instructions in this context
  auto *op_inst = dyn_cast<llvm::Instruction>(intToPtr->getOperand(0));
  if (!op_inst || op_inst->getOpcode() != llvm::Instruction::Add ||
      op_inst->getNumUses() > 1)
    return false;

  // keep track of (ptr + int) vs (int + ptr)
  bool ptrOnLeft = true;
  auto *ptrToInt = dyn_cast<llvm::PtrToIntInst>(op_inst->getOperand(0));

  if (!ptrToInt) {
    ptrOnLeft = false;
    ptrToInt = dyn_cast<llvm::PtrToIntInst>(op_inst->getOperand(1));
  }

  if (!ptrToInt || ptrToInt->getNumUses() > 1)
    return false;

  llvm::IRBuilder<> B(intToPtr);

  llvm::Value *gep = B.CreateGEP(B.getInt8Ty(), ptrToInt->getOperand(0),
                                 {op_inst->getOperand(ptrOnLeft ? 1 : 0)}, "");

  intToPtr->replaceAllUsesWith(gep);
  intToPtr->eraseFromParent();
  op_inst->eraseFromParent();
  ptrToInt->eraseFromParent();

  return true;
}

/*
 * remove empty-template void inline asm ("compiler barriers",
 * asm volatile("" ::: "memory"), the pervasive barrier()/barrier_var
 * macro family) from the SEMANTIC copy of the source. an empty
 * template emits no instructions and a void call produces no values,
 * so at runtime these are no-ops; they only constrain the optimizer.
 * codegen runs on an unstripped clone, so the backend still sees them.
 * see DECISIONS.md.
 */
void stripEmptyInlineAsm(
    llvm::Function *fn,
    std::unordered_map<unsigned, llvm::Instruction *> &lineMap) {
  llvm::SmallVector<llvm::CallInst *, 8> dead;
  for (auto &bb : *fn)
    for (auto &i : bb)
      if (auto *ci = dyn_cast<llvm::CallInst>(&i))
        if (auto *ia =
                dyn_cast<llvm::InlineAsm>(ci->getCalledOperand()))
          if (ia->getAsmString().empty() && ci->getType()->isVoidTy())
            dead.push_back(ci);
  for (auto *ci : dead) {
    std::erase_if(lineMap, [&](auto &kv) { return kv.second == ci; });
    ci->eraseFromParent();
  }
}

/*
 * barrier_var passthrough: asm volatile("" : "+r"(x)) -- an empty
 * template whose register outputs are each tied to an input -- emits
 * no instructions, so at runtime every output equals its tied input:
 * an identity function that only constrains the optimizer. replace
 * the results with the tied inputs in the SEMANTIC copy; codegen runs
 * on the unstripped clone and still sees the register constraints.
 * void empty-template asm is handled by stripEmptyInlineAsm above.
 */
void replaceIdentityInlineAsm(
    llvm::Function *fn,
    std::unordered_map<unsigned, llvm::Instruction *> &lineMap) {
  llvm::SmallVector<llvm::CallInst *, 8> dead;
  for (auto &bb : *fn) {
    for (auto &i : bb) {
      auto *ci = dyn_cast<llvm::CallInst>(&i);
      if (!ci)
        continue;
      auto *ia = dyn_cast<llvm::InlineAsm>(ci->getCalledOperand());
      if (!ia || !ia->getAsmString().empty() || ci->getType()->isVoidTy())
        continue;

      // map constraint entries to call arguments (inputs consume an
      // argument slot each, in order) and require every output to be
      // a direct register tied to an input; clobbers are irrelevant
      // for a no-instruction template
      auto cinfos = ia->ParseConstraints();
      std::vector<int> argOf(cinfos.size(), -1);
      llvm::SmallVector<llvm::Value *, 4> outVal;
      unsigned arg = 0;
      bool ok = true;
      for (auto &c : cinfos) {
        if (c.Type == llvm::InlineAsm::isOutput && c.isIndirect)
          ok = false;
        if (c.Type == llvm::InlineAsm::isInput)
          argOf[&c - &cinfos[0]] = arg++;
      }
      if (!ok)
        continue;
      for (auto &c : cinfos) {
        if (c.Type != llvm::InlineAsm::isOutput)
          continue;
        int m = c.MatchingInput;
        if (m < 0 || argOf[m] < 0) {
          ok = false;
          break;
        }
        outVal.push_back(ci->getArgOperand(argOf[m]));
      }
      if (!ok || outVal.empty())
        continue;

      if (!ci->getType()->isStructTy()) {
        if (outVal.size() != 1 || outVal[0]->getType() != ci->getType())
          continue;
        ci->replaceAllUsesWith(outVal[0]);
        dead.push_back(ci);
        continue;
      }

      // aggregate return: users must all be single-index extractvalue
      llvm::SmallVector<llvm::ExtractValueInst *, 4> evs;
      for (auto *u : ci->users()) {
        auto *ev = dyn_cast<llvm::ExtractValueInst>(u);
        if (!ev || ev->getNumIndices() != 1 ||
            ev->getIndices()[0] >= outVal.size() ||
            ev->getType() != outVal[ev->getIndices()[0]]->getType()) {
          ok = false;
          break;
        }
        evs.push_back(ev);
      }
      if (!ok)
        continue;
      for (auto *ev : evs) {
        ev->replaceAllUsesWith(outVal[ev->getIndices()[0]]);
        std::erase_if(lineMap, [&](auto &kv) { return kv.second == ev; });
        ev->eraseFromParent();
      }
      dead.push_back(ci);
    }
  }
  for (auto *ci : dead) {
    std::erase_if(lineMap, [&](auto &kv) { return kv.second == ci; });
    ci->eraseFromParent();
  }
}

/*
 * helper calls by number: selftest-style BPF code calls kernel
 * helpers through an integer-constant function pointer --
 * `call i64 inttoptr (i64 N to ptr)(...)` -- and the backend emits
 * `call N`. rewrite the SEMANTIC copy of the source to call a
 * synthesized external declaration @__bpf_helper_N (typed like the
 * call site) so that source and lifted target agree on one
 * uninterpreted function per helper ID. codegen runs on the
 * unmodified clone and still emits `call N`; the lifter resolves the
 * immediate back to @__bpf_helper_N (see bpf2llvm::doHelperCall).
 */
void rewriteHelperCallsByNumber(llvm::Function *fn) {
  auto *M = fn->getParent();
  for (auto &bb : *fn) {
    for (auto &i : bb) {
      auto *ci = dyn_cast<llvm::CallInst>(&i);
      if (!ci)
        continue;
      auto *ce = dyn_cast<llvm::ConstantExpr>(ci->getCalledOperand());
      if (!ce || ce->getOpcode() != llvm::Instruction::IntToPtr)
        continue;
      auto *id = dyn_cast<llvm::ConstantInt>(ce->getOperand(0));
      if (!id || id->isNegative())
        continue;
      auto name = "__bpf_helper_" + std::to_string(id->getZExtValue());
      ci->setCalledFunction(M->getOrInsertFunction(name, ci->getFunctionType()));
    }
  }
}

// find and collapse sequences of the form ptrToInt, add, intToPtr
// into a single GEP instruction
void tryReplacePtrtoInt(llvm::Function *fn) {
  for (auto it = llvm::instructions(*fn).begin(),
            end = llvm::instructions(*fn).end();
       it != end;) {
    llvm::Instruction &Inst = *it++;
    if (auto *intToPtr = dyn_cast<llvm::IntToPtrInst>(&Inst)) {
      tryReplaceRoundTrip(intToPtr);
    }
  }
}

/*
 * per-object stack blocks (see DECISIONS.md): the lifter models the
 * whole BPF frame as one block, which Alive2's per-block call-input
 * matching cannot reconcile with a source function whose stack
 * objects (allocas) escape to callees -- the source has one local
 * block per alloca. Using the backend's own frame layout (StackSlot,
 * from MachineFrameInfo), carve the alloca-backed slots out of the
 * lifted stack block into per-object allocas so the block structures
 * match.
 *
 * runs on the lifted function AFTER -O3 and the ptrtoint-roundtrip
 * collapse, where frame addresses appear in two shapes:
 *   gep i8, %stack, C                      (loads/stores; C constant)
 *   inttoptr(add(ptrtoint(gep %stack, 1024), -K))   (escaping call
 *     args; the multi-use ptrtoint defeats the roundtrip collapse)
 * derived pointers (nested geps, ptrtoint of a rewritten gep) follow
 * automatically since only DIRECT users of the stack block move.
 *
 * ALL-OR-NOTHING: a partial split is the one unsound shape (an
 * unattributed access could touch a split object's bytes in the
 * machine but miss its block in the model, hiding a real clobber), so
 * any unclassifiable use aborts the whole rewrite and the faithful
 * single-block model stays in effect. attributed-but-dynamically-OOB
 * accesses only make the model MORE undefined than the machine, which
 * biases toward refinement failure, never silent acceptance.
 */
bool rewriteStackObjects(llvm::Function *F2,
                         const std::vector<lifter::StackSlot> &slots,
                         std::ostream *log) {
  if (slots.empty())
    return true;

  // the lifted stack block: the lifter's myalloc call, usually
  // promoted to a plain alloca named "stack..." by -O3
  llvm::Value *stackMem = nullptr;
  for (auto &i : F2->getEntryBlock()) {
    if (auto *ai = dyn_cast<llvm::AllocaInst>(&i)) {
      if (ai->getName().starts_with("stack")) {
        stackMem = ai;
        break;
      }
    }
    if (auto *ci = dyn_cast<llvm::CallInst>(&i)) {
      auto *callee = ci->getCalledFunction();
      if (callee && callee->getName() == "myalloc" &&
          ci->getName().starts_with("stack")) {
        stackMem = ci;
        break;
      }
    }
  }
  if (!stackMem)
    return true; // no stack block at all (fully optimized away)

  const int64_t base = lifter::StackFrameTopOffset; // r10's byte offset
  auto &DL = F2->getParent()->getDataLayout();
  auto &Ctx = F2->getContext();
  auto *i8 = llvm::Type::getInt8Ty(Ctx);
  auto *i64 = llvm::Type::getInt64Ty(Ctx);

  // slot lookup by stack-block byte offset
  auto slotAt = [&](int64_t c) -> const lifter::StackSlot * {
    for (auto &s : slots)
      if (c >= base + s.offset && c < base + s.offset + (int64_t)s.size)
        return &s;
    return nullptr;
  };

  // classification plans; applied only if every use classifies
  struct GepPlan {
    llvm::GetElementPtrInst *gep;
    const lifter::StackSlot *slot; // null = residual, keep
    int64_t rel;                   // offset within the object
    llvm::Value *varIdx;           // extra variable index (may be null)
  };
  struct IntPlan {
    llvm::Instruction *inst; // the add (or ptrtoint) producing the address
    const lifter::StackSlot *slot;
    int64_t rel;
  };
  std::vector<GepPlan> gepPlans;
  std::vector<IntPlan> intPlans;

  auto bail = [&](const char *why, const llvm::Value *v) {
    std::string sss;
    llvm::raw_string_ostream ss(sss);
    if (v)
      v->print(ss);
    *log << "per-object stack rewrite: cannot classify (" << why << "): "
         << sss << "\n";
    return false;
  };

  // forward declarations for the mutually recursive classifiers
  std::function<bool(llvm::Value *, int64_t)> classifyPtr;
  std::function<bool(llvm::Instruction *, int64_t)> classifyIntUses;

  /*
   * classify every use of a pointer known to be stack-block byte
   * offset c, where c is NOT inside any slot (slot-addressed geps get
   * planned for replacement instead and their users simply follow the
   * new pointer). loads/stores/call-args through a residual pointer
   * are precise; derived geps re-enter classification (constant
   * offsets may land in a slot -> plan; variable offsets must be
   * attributable to a slot by their constant base); ptrtoint hands
   * off to the integer classifier. anything else bails.
   */
  classifyPtr = [&](llvm::Value *p, int64_t c) -> bool {
    for (auto *u : p->users()) {
      if (auto *gep = dyn_cast<llvm::GetElementPtrInst>(u)) {
        llvm::APInt off(64, 0);
        if (gep->accumulateConstantOffset(DL, off)) {
          int64_t total = c + off.getSExtValue();
          if (auto *s = slotAt(total)) {
            gepPlans.push_back({gep, s, total - (base + s->offset), nullptr});
            continue;
          }
          if (!classifyPtr(gep, total))
            return false;
          continue;
        }
        // variable index: only the canonical single-index i8 shape,
        // attributed to a slot by its constant base
        if (gep->getNumIndices() != 1 || gep->getSourceElementType() != i8)
          return bail("non-canonical variable-index gep", gep);
        auto *idx = gep->getOperand(1);
        llvm::Value *var = nullptr;
        llvm::ConstantInt *k = nullptr;
        if (auto *bo = dyn_cast<llvm::BinaryOperator>(idx)) {
          if (bo->getOpcode() == llvm::Instruction::Add) {
            k = dyn_cast<llvm::ConstantInt>(bo->getOperand(1));
            var = bo->getOperand(0);
            if (!k) {
              k = dyn_cast<llvm::ConstantInt>(bo->getOperand(0));
              var = bo->getOperand(1);
            }
          }
        }
        if (!k)
          return bail("variable-index gep without constant base", gep);
        int64_t total = c + k->getSExtValue();
        auto *s = slotAt(total);
        if (!s)
          return bail("variable-index gep base outside slots", gep);
        gepPlans.push_back({gep, s, total - (base + s->offset), var});
        continue;
      }
      if (auto *pti = dyn_cast<llvm::PtrToIntInst>(u)) {
        if (!classifyIntUses(pti, c))
          return false;
        continue;
      }
      if (isa<llvm::LoadInst>(u)) {
        if (slotAt(c))
          return bail("direct access at a slot-covered base", u);
        continue;
      }
      if (auto *si = dyn_cast<llvm::StoreInst>(u)) {
        if (si->getPointerOperand() == p && !slotAt(c))
          continue;
        return bail("residual frame pointer stored as data", si);
      }
      if (isa<llvm::CallBase>(u))
        continue; // residual escape: block matching handles (or fails safely)
      if (isa<llvm::ICmpInst>(u))
        continue;
      return bail("residual pointer user", u);
    }
    return true;
  };

  /*
   * classify the integer uses of a frame address known to be
   * stack-block offset c (from ptrtoint): adds with a constant give a
   * concrete address (slot -> plan; residual -> its inttoptr results
   * re-enter pointer classification); icmp is address comparison
   * (harmless); anything else is untrackable integer flow.
   */
  classifyIntUses = [&](llvm::Instruction *pti, int64_t c) -> bool {
    auto classifyAddr = [&](llvm::Instruction *inst, int64_t total) -> bool {
      if (auto *s = slotAt(total)) {
        intPlans.push_back({inst, s, total - (base + s->offset)});
        return true;
      }
      // residual constant address: integer users must be inttoptr
      // (whose pointer re-enters classification) or icmp;
      // storing/re-deriving it could reach a slot untracked
      if (auto *itp = dyn_cast<llvm::IntToPtrInst>(inst))
        return classifyPtr(itp, total);
      for (auto *u : inst->users()) {
        if (auto *itp = dyn_cast<llvm::IntToPtrInst>(u)) {
          if (!classifyPtr(itp, total))
            return false;
          continue;
        }
        if (isa<llvm::ICmpInst>(u))
          continue;
        return bail("residual integer address user", u);
      }
      return true;
    };
    for (auto *u : pti->users()) {
      if (auto *bo = dyn_cast<llvm::BinaryOperator>(u)) {
        if (bo->getOpcode() != llvm::Instruction::Add)
          return bail("non-add arithmetic on frame address", bo);
        auto *k = dyn_cast<llvm::ConstantInt>(bo->getOperand(1));
        if (!k)
          k = dyn_cast<llvm::ConstantInt>(bo->getOperand(0));
        if (!k)
          return bail("add of frame address with non-constant", bo);
        if (!classifyAddr(bo, c + k->getSExtValue()))
          return false;
        continue;
      }
      if (auto *itp = dyn_cast<llvm::IntToPtrInst>(u)) {
        if (!classifyAddr(itp, c))
          return false;
        continue;
      }
      if (isa<llvm::ICmpInst>(u))
        continue;
      return bail("frame-address integer user", u);
    }
    return true;
  };

  if (!classifyPtr(stackMem, 0))
    return false;

  // apply: one alloca per referenced slot. alignment is what the
  // machine guarantees for that offset within the 16-aligned frame
  // (O3 has already stamped that alignment on accesses), never less
  // than the slot's own.
  llvm::DenseMap<const lifter::StackSlot *, llvm::AllocaInst *> objs;
  auto objFor = [&](const lifter::StackSlot *s) -> llvm::AllocaInst * {
    auto &obj = objs[s];
    if (!obj) {
      uint64_t frameAlign =
          1ull << std::min(std::countr_zero<uint64_t>(base + s->offset), 4);
      obj = new llvm::AllocaInst(
          i8, 0, llvm::ConstantInt::get(i64, s->size),
          llvm::Align(std::max<uint64_t>(s->align, frameAlign)),
          "bpftv_stkobj", F2->getEntryBlock().getFirstInsertionPt());
    }
    return obj;
  };

  for (auto &p : gepPlans) {
    if (!p.slot)
      continue;
    llvm::IRBuilder<> B(p.gep);
    llvm::Value *idx = B.getInt64(p.rel);
    if (p.varIdx)
      idx = B.CreateAdd(p.varIdx, idx);
    auto *np = B.CreateGEP(i8, objFor(p.slot), {idx});
    p.gep->replaceAllUsesWith(np);
    p.gep->eraseFromParent();
  }
  for (auto &p : intPlans) {
    llvm::IRBuilder<> B(p.inst);
    auto *np = B.CreateGEP(i8, objFor(p.slot), {B.getInt64(p.rel)});
    if (isa<llvm::IntToPtrInst>(p.inst)) {
      p.inst->replaceAllUsesWith(np);
    } else {
      // integer frame address (escapes into stores/other flow): keep
      // it an integer but give it the object's provenance; fold any
      // direct inttoptr users back to the pointer
      auto *ni = B.CreatePtrToInt(np, i64);
      for (auto *iu : llvm::make_early_inc_range(p.inst->users())) {
        if (auto *itp = dyn_cast<llvm::IntToPtrInst>(iu)) {
          itp->replaceAllUsesWith(np);
          itp->eraseFromParent();
        }
      }
      p.inst->replaceAllUsesWith(ni);
    }
    p.inst->eraseFromParent();
  }

  *log << "per-object stack rewrite: split " << objs.size() << " of "
       << slots.size() << " frame slots\n";
  return true;
}

void doit(llvm::Module *srcModule, llvm::Function *srcFn, Verifier &verifier,
          llvm::TargetLibraryInfoWrapperPass &TLI) {

  // do this check early to stop an assertion in LLVM from firing
  if (srcFn->isVarArg()) {
    *out << "\nERROR: varargs not supported\n\n";
    exit(-1);
  }

  for (auto &global : srcModule->globals()) {
    if (global.isThreadLocal()) {
      *out << "\nERROR: thread_local not supported\n\n";
      exit(-1);
    }
  }

  {
    // strip metadata Alive2 cannot process and that carries no
    // refinement-relevant semantics: aliasing hints (like the
    // reference driver) plus srcloc/errno.tbaa/inline_history/
    // tbaa.struct, which real-world BPF objects carry
    auto &Ctx = srcFn->getContext();
    llvm::SmallSet<unsigned, 8> drop;
    drop.insert(llvm::LLVMContext::MD_alias_scope);
    drop.insert(llvm::LLVMContext::MD_noalias);
    drop.insert(llvm::LLVMContext::MD_tbaa);
    drop.insert(llvm::LLVMContext::MD_tbaa_struct);
    for (const char *name :
         {"srcloc", "errno.tbaa", "inline_history", "prof", "unpredictable"})
      drop.insert(Ctx.getMDKindID(name));
    auto Pred = [&drop](unsigned MDKind, llvm::MDNode *Node) {
      return drop.contains(MDKind);
    };
    for (auto &bb : *srcFn)
      for (auto &i : bb)
        i.eraseMetadataIf(Pred);
  }

  // nuke the rest of the functions in the module -- no need to
  // generate and then parse assembly that we don't care about
  for (auto &F : *srcModule) {
    if (&F != srcFn && !F.isDeclaration())
      F.deleteBody();
  }

  string Error;
  const auto *Targ = llvm::TargetRegistry::lookupTarget(DefaultTT, Error);
  if (!Targ) {
    *out << "Can't lookup target\n";
    *out << Error;
    exit(-1);
  }

  unique_ptr<llvm::MemoryBuffer> AsmBuffer;
  std::unordered_map<unsigned, llvm::Instruction *> lineMap;
  std::vector<lifter::StackSlot> stackSlots;
  if (opt_asm_input == "") {
    lifter::addDebugInfo(srcFn, lineMap);
    // codegen must see the module unmodified (compiler barriers affect
    // scheduling and layout); clone it before stripping the semantic
    // copy that the refinement check consumes
    auto CodegenM = llvm::CloneModule(*srcModule);
    stripEmptyInlineAsm(srcFn, lineMap);
    replaceIdentityInlineAsm(srcFn, lineMap);
    rewriteHelperCallsByNumber(srcFn);

    // pre-flight the stripped source through Alive2 BEFORE running the
    // backend: inputs Alive2 can't process anyway (atomics, remaining
    // inline asm, ...) can drive codegen into report_fatal_error
    // aborts, and there is no point compiling what we cannot verify
    {
      auto fn = llvm2alive(*srcFn, TLI.getTLI(*srcFn), /*isSrc=*/true);
      if (!fn) {
        *out << "Fatal error, exiting\n";
        exit(-1);
      }
    }

    AsmBuffer = lifter::generateAsm(*CodegenM, Targ, DefaultTT,
                                    DefaultCPU.c_str(), DefaultFeatures,
                                    &stackSlots);
    if (!AsmBuffer) {
      *out << "\nERROR: BPF backend reported an error lowering this "
              "function (see stderr); no code to validate\n\n";
      exit(-1);
    }
  } else {
    // asm-input mode: instrument the source anyway so .loc directives
    // in asm that was generated from this same source (then possibly
    // hand-mutated) still map calls back to their source instructions
    lifter::addDebugInfo(srcFn, lineMap);
    stripEmptyInlineAsm(srcFn, lineMap);
    replaceIdentityInlineAsm(srcFn, lineMap);
    rewriteHelperCallsByNumber(srcFn);
    AsmBuffer = ExitOnErr(
        llvm::errorOrToExpected(llvm::MemoryBuffer::getFile(opt_asm_input)));
  }


  if (opt_asm_output != "") {
    std::ofstream asm_file(opt_asm_output);
    if (!asm_file)
      llvm::report_fatal_error("Couldn't open output file, exiting");
    asm_file << AsmBuffer->getBuffer().str();
  }

  *out << "\n\n------------ Assembly: ------------\n\n";
  *out << AsmBuffer->getBuffer().str();
  *out << "-------------" << std::endl;

  if (opt_asm_only)
    exit(0);

  auto [F1, F2] = lifter::liftFunc(srcFn, std::move(AsmBuffer), lineMap,
                                   opt_optimize_tgt, out, Targ, DefaultTT,
                                   DefaultCPU.c_str(), DefaultFeatures,
                                   &stackSlots);

  if (save_lifted_ir) {
    std::filesystem::path p{(string)opt_file};
    p.replace_extension(".lifted.ll");
    ofstream of(p);
    of << lifter::moduleToString(F2->getParent());
    of.close();
  }

  if (run_replace_ptrtoint)
    tryReplacePtrtoInt(F2);

  if (!rewriteStackObjects(F2, stackSlots, out)) {
    // the rewrite had to bail; multi-escape functions were admitted by
    // checkFuncSupport only on the promise of the split, so re-impose
    // the single-block-model rejection here rather than emit a false
    // INCORRECT
    if (lifter::escapingStackObjects(*F1).size() > 1 &&
        !lifter::stackEscapeAllowed()) {
      *out << "\nERROR: multiple stack objects escape to callees and the "
              "per-object stack rewrite could not attribute every frame "
              "access; the lifted single-block stack model cannot match "
              "them (known limitation)\n\n";
      exit(-1);
    }
    *out << "per-object stack rewrite bailed; keeping the single-block "
            "stack model\n";
  }

  {
    std::string sss;
    llvm::raw_string_ostream ss(sss);
    if (llvm::verifyModule(*F2->getParent(), &ss)) {
      *out << sss;
      *out << "\nERROR: Lifted module is broken, this should not happen\n";
      exit(-1);
    }
  }

  if (!opt_skip_verification) {
    auto unsoundBefore = verifier.num_unsound;
    verifier.compareFunctions(*F1, *F2);
    /*
     * a multi-escape function only got past checkFuncSupport's
     * rejection on the promise of the per-object split; when
     * refinement STILL fails as unsound, the remaining model gaps in
     * this class (opaque callees writing through escaped stack
     * pointers, pointer bytes through integer registers, ...) make
     * the verdict untrustworthy as a miscompilation claim -- report
     * the known limitation instead. real miscompiles in this class
     * remain masked exactly as they were when every multi-escape
     * function was rejected up front; the split's gain is the
     * functions that now VERIFY.
     */
    if (verifier.num_unsound > unsoundBefore &&
        lifter::escapingStackObjects(*F1).size() > 1 &&
        !lifter::stackEscapeAllowed()) {
      *out << "\nERROR: refinement failed under the per-object stack "
              "model for a function with multiple stack objects that "
              "escape to callees (known limitation)\n\n";
      exit(-1);
    }
  }

  *out << "done comparing functions\n";
  out->flush();
}

} // anonymous namespace

unique_ptr<Cache> cache;

int main(int argc, char **argv) {
  llvm::InitLLVM X(argc, argv);
  llvm::EnableDebugBuffering = true;
  llvm::LLVMContext Context;

  std::string Usage =
      R"EOF(bpf-tv -- Alive2-based translation validation for the LLVM BPF backend
built on Alive2 version )EOF";
  Usage += alive_version;

  llvm::cl::HideUnrelatedOptions(alive_cmdargs);
  llvm::cl::ParseCommandLineOptions(argc, argv, Usage);

  auto srcModule = openInputFile(Context, opt_file);
  if (!srcModule.get()) {
    cerr << "Could not read bitcode from '" << opt_file << "'\n";
    return -1;
  }

#define ARGS_MODULE_VAR srcModule
#include "llvm_util/cmd_args_def.h"

  // if src is always UB we end up with weird effects such as targets
  // that never reach a return instruction. let's just weed these out
  // here.
  config::fail_if_src_is_ub = true;

  // turn on Alive2's asm-level memory model for the target; this
  // helps Alive2 deal more gracefully with the fact that integers and
  // pointers are freely mixed at the asm level, unlike in LLVM IR in
  // general
  config::tgt_is_asm = true;

  // undef inputs put doubly-quantified terms in the refinement query
  // that make Z3 diverge even on trivial functions (measured: a
  // two-register add whose operands get commuted by -O3 goes from
  // unprovable-in-120s to 16ms). undef is on its way out of LLVM
  // anyway, so bpf-tv always runs with undef inputs disabled.
  config::disable_undef_input = true;

  DefaultTT = llvm::Triple("bpfel-unknown-unknown");
  DefaultDL = DefaultTT.computeDataLayout();
  DefaultCPU = opt_cpu;
  LLVMInitializeBPFTargetInfo();
  LLVMInitializeBPFTarget();
  LLVMInitializeBPFTargetMC();
  LLVMInitializeBPFAsmParser();
  LLVMInitializeBPFAsmPrinter();

  srcModule.get()->setTargetTriple(DefaultTT);
  srcModule.get()->setDataLayout(DefaultDL);

  auto &DL = srcModule.get()->getDataLayout();
  llvm::Triple targetTriple(srcModule.get()->getTargetTriple());
  llvm::TargetLibraryInfoWrapperPass TLI(targetTriple);

  llvm_util::initializer llvm_util_init(*out, DL);
  smt::smt_initializer smt_init;
  Verifier verifier(TLI, smt_init, *out);
  verifier.always_verify = opt_always_verify;
  verifier.print_dot = opt_print_dot;
  verifier.bidirectional = opt_bidirectional;

  if (opt_fn != "") {
    auto *srcFn = findFunction(*srcModule, opt_fn);
    if (srcFn == nullptr) {
      *out << "ERROR: Couldn't find function to verify\n";
      exit(-1);
    }
    doit(srcModule.get(), srcFn, verifier, TLI);
  } else {
    for (auto &srcFn : *srcModule.get()) {
      if (srcFn.isDeclaration())
        continue;
      doit(srcModule.get(), &srcFn, verifier, TLI);
      break;
    }
  }

  *out << "Summary:\n"
          "  "
       << verifier.num_correct
       << " correct transformations\n"
          "  "
       << verifier.num_unsound
       << " incorrect transformations\n"
          "  "
       << verifier.num_failed
       << " failed-to-prove transformations\n"
          "  "
       << verifier.num_errors << " Alive2 errors\n";

  if (opt_smt_stats)
    smt::solver_print_stats(*out);

  smt_init.reset();

  if (opt_alias_stats)
    IR::Memory::printAliasStats(*out);

  return verifier.num_errors > 0;
}
