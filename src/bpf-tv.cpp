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

#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"

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
    // let's not even bother if Alive2 can't process our function

    auto Pred = [](unsigned MDKind, llvm::MDNode *Node) {
      return MDKind == llvm::LLVMContext::MD_alias_scope ||
             MDKind == llvm::LLVMContext::MD_noalias ||
             MDKind == llvm::LLVMContext::MD_tbaa;
    };
    for (auto &bb : *srcFn)
      for (auto &i : bb)
        i.eraseMetadataIf(Pred);

    auto fn = llvm2alive(*srcFn, TLI.getTLI(*srcFn), /*isSrc=*/true);
    if (!fn) {
      *out << "Fatal error, exiting\n";
      exit(-1);
    }
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
  if (opt_asm_input == "") {
    lifter::addDebugInfo(srcFn, lineMap);
    AsmBuffer = lifter::generateAsm(*srcModule, Targ, DefaultTT,
                                    DefaultCPU.c_str(), DefaultFeatures);
    if (!AsmBuffer) {
      *out << "\nERROR: BPF backend reported an error lowering this "
              "function (see stderr); no code to validate\n\n";
      exit(-1);
    }
  } else {
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
                                   DefaultCPU.c_str(), DefaultFeatures);

  if (save_lifted_ir) {
    std::filesystem::path p{(string)opt_file};
    p.replace_extension(".lifted.ll");
    ofstream of(p);
    of << lifter::moduleToString(F2->getParent());
    of.close();
  }

  if (run_replace_ptrtoint)
    tryReplacePtrtoInt(F2);

  if (!opt_skip_verification)
    verifier.compareFunctions(*F1, *F2);

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
