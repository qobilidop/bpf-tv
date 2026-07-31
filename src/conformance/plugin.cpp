// bpf_conformance plugin backed by the bpf-tv lifter.
//
// Protocol (see third_party/bpf_conformance): stdin carries the BPF
// program as hex bytes; argv[1] (if present and not a flag) carries the
// input memory as hex bytes; stdout must be the final value of %r0 in
// hex. The conformance runner treats a nonzero exit code as an error
// and matches stderr against expected-error strings.
//
// This turns the lifter into a BPF "runtime": bytecode is disassembled
// (LLVM BPF MCDisassembler), lifted to LLVM IR by bpf2llvm, JIT
// compiled for the host, and executed with r1 = memory pointer and
// r2 = memory size, per the conformance calling convention. Its pass
// rate over the 313-test corpus measures the lifter's ISA fidelity
// differentially against the executable spec.

#include "lifter/bpf2llvm.h"
#include "lifter/lifter.h"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace llvm;
using namespace std;

// storage backing the lifter's unspecified-value loads when the JIT'd
// code actually executes them
static uint8_t unspecified_mem[65536];

static vector<uint8_t> parse_hex(const string &s) {
  vector<uint8_t> out;
  string clean;
  for (char c : s)
    if (isxdigit(static_cast<unsigned char>(c)))
      clean.push_back(c);
  for (size_t i = 0; i + 1 < clean.size() || (clean.size() % 2 == 1 && i < clean.size()); i += 2) {
    string byte = clean.substr(i, 2);
    out.push_back(static_cast<uint8_t>(stoul(byte, nullptr, 16)));
  }
  return out;
}

int main(int argc, char **argv) {
  string mem_hex;
  for (int i = 1; i < argc; ++i) {
    string arg = argv[i];
    if (arg == "--help") {
      cout << "usage: " << argv[0] << " [memory-hex] < program-hex\n";
      return 1;
    }
    if (arg.rfind("--", 0) == 0) {
      // unsupported option (e.g. --elf); conformance runner treats
      // nonzero exit as error
      cerr << "unsupported option: " << arg << "\n";
      return 1;
    }
    mem_hex = arg;
  }

  string prog_hex, line;
  while (getline(cin, line))
    prog_hex += line;
  auto prog = parse_hex(prog_hex);
  auto mem = parse_hex(mem_hex);
  if (prog.empty() || prog.size() % 8 != 0) {
    cerr << "invalid program size " << prog.size() << "\n";
    return 1;
  }

  LLVMInitializeBPFTargetInfo();
  LLVMInitializeBPFTarget();
  LLVMInitializeBPFTargetMC();
  LLVMInitializeBPFDisassembler();
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();

  LLVMContext Ctx;
  Triple TT("bpfel-unknown-unknown");
  auto srcModule = make_unique<Module>("conformance", Ctx);
  srcModule->setTargetTriple(TT);
  srcModule->setDataLayout(TT.computeDataLayout());

  // dummy source: signature carries the conformance ABI (r1 = memory,
  // r2 = size); the body is irrelevant -- we lift and execute, no
  // refinement check
  auto *fnTy = FunctionType::get(
      Type::getInt64Ty(Ctx),
      {PointerType::get(Ctx, 0), Type::getInt64Ty(Ctx)}, false);
  auto *srcFn = Function::Create(fnTy, GlobalValue::ExternalLinkage,
                                 "conformance_test", srcModule.get());
  {
    IRBuilder<> B(BasicBlock::Create(Ctx, "entry", srcFn));
    B.CreateRet(B.getInt64(0));
  }

  string Error;
  const auto *Targ = TargetRegistry::lookupTarget(TT, Error);
  if (!Targ) {
    cerr << "no BPF target: " << Error << "\n";
    return 1;
  }

  // lift
  unordered_map<unsigned, Instruction *> lineMap;
  static ostream *nullout = &cerr;
  auto lifter = make_unique<lifter::bpf2llvm>(
      srcFn, nullptr, lineMap, nullout, Targ, TT, "v4", "");
  auto [F1, tgtFn] = lifter->runBytes(prog);
  (void)F1;
  lifter->fixupOptimizedTgt(tgtFn);

  // JIT the lifted module for the host
  auto tgtModule = tgtFn->getParent();
  string fnName = tgtFn->getName().str();
  orc::LLJITBuilder builder;
  auto jitOrErr = builder.create();
  if (!jitOrErr) {
    cerr << "LLJIT: " << toString(jitOrErr.takeError()) << "\n";
    return 1;
  }
  auto &jit = *jitOrErr;
  tgtModule->setTargetTriple(Triple(sys::getProcessTriple()));
  tgtModule->setDataLayout(jit->getDataLayout());

  // resolve the lifter's unspecified-value scratch global
  auto &dylib = jit->getMainJITDylib();
  orc::SymbolMap syms;
  syms[jit->mangleAndIntern("__bpf_tv_unspecified")] = orc::ExecutorSymbolDef(
      orc::ExecutorAddr::fromPtr(&unspecified_mem[0]), JITSymbolFlags());
  if (auto err = dylib.define(orc::absoluteSymbols(syms))) {
    cerr << "define: " << toString(std::move(err)) << "\n";
    return 1;
  }

  // the lifted module shares srcModule's LLVMContext, but the JIT
  // wants to own its module together with its context; round-trip
  // through bitcode into a fresh context to untangle ownership
  SmallString<0> bc;
  {
    raw_svector_ostream os(bc);
    WriteBitcodeToFile(*tgtModule, os);
  }
  auto tsCtx = std::make_unique<LLVMContext>();
  auto jitModOrErr = parseBitcodeFile(
      MemoryBufferRef(StringRef(bc.data(), bc.size()), "lifted"), *tsCtx);
  if (!jitModOrErr) {
    cerr << "bitcode: " << toString(jitModOrErr.takeError()) << "\n";
    return 1;
  }
  auto tsm =
      orc::ThreadSafeModule(std::move(*jitModOrErr), std::move(tsCtx));

  auto sym = [&]() -> Expected<orc::ExecutorAddr> {
    if (auto err = jit->addIRModule(std::move(tsm)))
      return std::move(err);
    return jit->lookup(fnName);
  }();
  if (!sym) {
    cerr << "jit error: " << toString(sym.takeError()) << "\n";
    return 1;
  }

  using ConformanceFn = uint64_t (*)(void *, uint64_t);
  auto *fn = sym->toPtr<ConformanceFn>();
  uint64_t r0 = fn(mem.empty() ? nullptr : mem.data(), mem.size());

  printf("%llx\n", static_cast<unsigned long long>(r0));
  return 0;
}
