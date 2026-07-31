// BPF → LLVM IR lifter for bpf-tv.
// Modeled on riscv2llvm from the Alive2 arm-tv branch (regehr/alive2),
// Copyright (c) 2018-present the Alive2 authors. MIT license.
// Reference implementation: third_party/alive2-arm-tv/backend_tv/

#include "lifter/bpf2llvm.h"

#include "llvm/ADT/APInt.h"
#include "llvm/BinaryFormat/ELF.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#define GET_INSTRINFO_ENUM
#include "Target/BPF/BPFGenInstrInfo.inc"

#define GET_REGINFO_ENUM
#include "Target/BPF/BPFGenRegisterInfo.inc"

using namespace std;
using namespace lifter;
using namespace llvm;

bpf2llvm::bpf2llvm(Function *srcFn, unique_ptr<MemoryBuffer> MB,
                   std::unordered_map<unsigned, llvm::Instruction *> &lineMap,
                   std::ostream *out, const llvm::Target *Targ,
                   llvm::Triple DefaultTT, const char *DefaultCPU,
                   const char *DefaultFeatures)
    : mc2llvm(srcFn, std::move(MB), lineMap, out, Targ, DefaultTT, DefaultCPU,
              DefaultFeatures) {}

unsigned bpf2llvm::branchInst() {
  return BPF::JMP;
}

unsigned bpf2llvm::sentinelNOP() {
  return BPF::NOP;
}

bool bpf2llvm::isWReg(unsigned Reg) {
  return Reg >= BPF::W0 && Reg <= BPF::W11;
}

unsigned bpf2llvm::mapRegToBackingReg(unsigned Reg) {
  if (isWReg(Reg))
    return Reg - BPF::W0 + BPF::R0;
  assert(Reg >= BPF::R0 && Reg <= BPF::R11 && "unknown BPF register");
  return Reg;
}

Value *bpf2llvm::lookupReg(unsigned Reg) {
  return RegFile[mapRegToBackingReg(Reg)];
}

Value *bpf2llvm::readReg64(unsigned Reg) {
  return createLoad(getIntTy(64), lookupReg(Reg));
}

Value *bpf2llvm::readReg32(unsigned Reg) {
  return createTrunc(readReg64(Reg), getIntTy(32));
}

void bpf2llvm::updateReg64(Value *V, unsigned Reg) {
  assert(getBitWidth(V) == 64);
  createStore(V, lookupReg(Reg));
}

// BPF ALU32 semantics: a 32-bit write zeroes the upper 32 bits of the
// destination register. This is the semantic dimension behind the
// backend's zext-elimination bug cluster; model it exactly.
void bpf2llvm::updateReg32(Value *V, unsigned Reg) {
  assert(getBitWidth(V) == 32);
  updateReg64(createZExt(V, getIntTy(64)), Reg);
}

Value *bpf2llvm::readRegOperand64(int idx) {
  auto op = CurInst->getOperand(idx);
  assert(op.isReg());
  return readReg64(op.getReg());
}

Value *bpf2llvm::readRegOperand32(int idx) {
  auto op = CurInst->getOperand(idx);
  assert(op.isReg());
  return readReg32(op.getReg());
}

Value *bpf2llvm::readPtrRegOperand(int idx) {
  auto op = CurInst->getOperand(idx);
  assert(op.isReg());
  return createLoad(PointerType::get(Ctx, 0), lookupReg(op.getReg()));
}

Value *bpf2llvm::readImmOperand(int idx, unsigned bits) {
  auto op = CurInst->getOperand(idx);
  assert(op.isImm());
  return getSignedIntConst(op.getImm(), bits);
}

Value *bpf2llvm::readALUSrcOperand(int idx, unsigned bits) {
  auto op = CurInst->getOperand(idx);
  if (op.isImm())
    return readImmOperand(idx, bits);
  assert(op.isReg());
  return bits == 64 ? readReg64(op.getReg()) : readReg32(op.getReg());
}

Value *bpf2llvm::getMemPointerOperand(int idx) {
  auto base = readPtrRegOperand(idx);
  auto offset = readImmOperand(idx + 1, 64);
  return createGEP(getIntTy(8), base, {offset}, nextName());
}

// TODO -- this is identical to the riscv2llvm version; move it up to
// mc2llvm
tuple<BasicBlock *, BasicBlock *> bpf2llvm::getBranchTargetsOperand(int op) {
  auto &jmp_tgt_op = CurInst->getOperand(op);
  assert(jmp_tgt_op.isExpr() && "expected expression");
  assert((jmp_tgt_op.getExpr()->getKind() == MCExpr::ExprKind::SymbolRef) &&
         "expected symbol ref as branch operand");
  const MCSymbolRefExpr &SRE = cast<MCSymbolRefExpr>(*jmp_tgt_op.getExpr());
  const MCSymbol &Sym = SRE.getSymbol();
  auto dst_true = getBBByName(Sym.getName());
  assert(MCBB->getSuccs().size() == 1 || MCBB->getSuccs().size() == 2);
  const string *dst_false_name = nullptr;
  for (auto &succ : MCBB->getSuccs()) {
    if (succ->getName() != Sym.getName()) {
      dst_false_name = &succ->getName();
      break;
    }
  }
  auto dst_false =
      getBBByName(dst_false_name ? *dst_false_name : Sym.getName());
  return make_tuple(dst_true, dst_false);
}

Value *bpf2llvm::enforceSExtZExt(Value *V, bool isSExt, bool isZExt) {
  auto argTy = V->getType();
  unsigned targetWidth = 64;

  // no work needed
  if (argTy->isPointerTy() || argTy->isVoidTy())
    return V;

  assert(argTy->isIntegerTy() && "only integer/pointer values on BPF");

  if (isZExt && getBitWidth(V) < targetWidth)
    V = createZExt(V, getIntTy(targetWidth));

  if (isSExt && getBitWidth(V) < targetWidth)
    V = createSExt(V, getIntTy(targetWidth));

  // pad out any remaining bits with junk (frozen poison)
  auto junkBits = targetWidth - getBitWidth(V);
  if (junkBits > 0) {
    auto junk = createFreeze(PoisonValue::get(getIntTy(junkBits)));
    auto ext1 = createZExt(junk, getIntTy(targetWidth));
    auto shifted =
        createRawShl(ext1, getUnsignedIntConst(getBitWidth(V), targetWidth));
    auto ext2 = createZExt(V, getIntTy(targetWidth));
    V = createOr(shifted, ext2);
  }

  return V;
}

void bpf2llvm::platformInit() {
  auto i8ty = getIntTy(8);

  // allocate storage for the register file; R11 exists in LLVM's
  // enum as an internal pseudo but should never appear in emitted
  // assembly -- having storage for it is harmless
  for (unsigned Reg = BPF::R0; Reg <= BPF::R11; ++Reg) {
    stringstream Name;
    Name << "R" << Reg - BPF::R0;
    createRegStorage(Reg, 64, Name.str());
  }

  // r10 is the read-only frame pointer; it points at the *top* of the
  // stack frame and code accesses the frame at negative offsets
  auto frameTop =
      createGEP(i8ty, stackMem, {getUnsignedIntConst(stackBytes, 64)}, "");
  createStore(frameTop, RegFile[BPF::R10]);

  *out << "created registers\n";

  // callee side of the ABI: up to five integer/pointer arguments
  // arrive in r1-r5
  unsigned argNum = 0;
  for (Function::arg_iterator arg = liftedFn->arg_begin(),
                              E = liftedFn->arg_end(),
                              srcArg = srcFn->arg_begin();
       arg != E; ++arg, ++srcArg) {
    auto *argTy = arg->getType();
    if (!(argTy->isIntegerTy() || argTy->isPointerTy())) {
      *out << "\nERROR: only integer/pointer arguments supported\n\n";
      exit(-1);
    }
    if (argNum >= 5) {
      *out << "\nERROR: more than 5 arguments not supported by the BPF "
              "calling convention\n\n";
      exit(-1);
    }
    auto *val =
        enforceSExtZExt(arg, srcArg->hasSExtAttr(), srcArg->hasZExtAttr());
    createStore(val, RegFile[BPF::R1 + argNum]);
    ++argNum;
  }

  *out << "done with callee-side ABI stuff\n";
}

vector<Value *> bpf2llvm::marshallArgs(FunctionType *fTy) {
  *out << "entering marshallArgs()\n";
  assert(fTy);
  if (fTy->getReturnType()->isStructTy() ||
      fTy->getReturnType()->isArrayTy()) {
    *out << "\nERROR: aggregate return values not supported\n\n";
    exit(-1);
  }
  unsigned argNum = 0;
  vector<Value *> args;
  for (auto arg = fTy->param_begin(); arg != fTy->param_end(); ++arg) {
    Type *argTy = *arg;
    assert(argTy);
    if (!(argTy->isIntegerTy() || argTy->isPointerTy())) {
      *out << "\nERROR: only integer/pointer call arguments supported\n\n";
      exit(-1);
    }
    if (argNum >= 5) {
      *out << "\nERROR: calls with more than 5 arguments not supported\n\n";
      exit(-1);
    }
    Value *param = readReg64(BPF::R1 + argNum);
    ++argNum;
    if (argTy->isPointerTy()) {
      param = new IntToPtrInst(param, PointerType::get(Ctx, 0), "", LLVMBB);
    } else {
      assert(argTy->getIntegerBitWidth() <= 64);
      if (argTy->getIntegerBitWidth() < 64)
        param = createTrunc(param, getIntTy(argTy->getIntegerBitWidth()));
    }
    args.push_back(param);
  }
  *out << "marshalled up " << args.size() << " arguments\n";
  return args;
}

void bpf2llvm::doCall(FunctionCallee FC, CallInst *llvmCI,
                      const string &calleeName) {
  *out << "entering doCall()\n";

  auto args = marshallArgs(FC.getFunctionType());

  // these functions have an LLVM "immediate" as their last argument;
  // it is not present in the assembly at all, we have to provide it
  // by hand
  if (calleeName == "llvm.memset.p0.i64" ||
      calleeName == "llvm.memset.p0.i32" ||
      calleeName == "llvm.memcpy.p0.p0.i64" ||
      calleeName == "llvm.memmove.p0.p0.i64") {
    *out << "adding constant Boolean as args[3]\n";
    args[3] = getBoolConst(false);
  }

  auto CI = CallInst::Create(FC, args, "", LLVMBB);

  bool sext{false}, zext{false};

  assert(llvmCI);
  if (llvmCI->hasFnAttr(Attribute::NoReturn)) {
    auto a = CI->getAttributes();
    auto a2 = a.addFnAttribute(Ctx, Attribute::NoReturn);
    CI->setAttributes(a2);
  }
  // NB we have to check for both function attributes and call site
  // attributes
  if (llvmCI->hasRetAttr(Attribute::SExt))
    sext = true;
  if (llvmCI->hasRetAttr(Attribute::ZExt))
    zext = true;
  auto calledFn = llvmCI->getCalledFunction();
  if (calledFn) {
    if (calledFn->hasRetAttribute(Attribute::SExt))
      sext = true;
    if (calledFn->hasRetAttribute(Attribute::ZExt))
      zext = true;
  }

  auto RV = enforceSExtZExt(CI, sext, zext);

  // r1-r5 are clobbered by calls; r6-r9 are callee-saved
  for (unsigned reg = BPF::R1; reg <= BPF::R5; ++reg)
    invalidateReg(reg, 64);

  auto retTy = FC.getFunctionType()->getReturnType();
  if (retTy->isIntegerTy() || retTy->isPointerTy()) {
    if (retTy->isPointerTy())
      RV = new PtrToIntInst(RV, getIntTy(64), "", LLVMBB);
    updateReg64(RV, BPF::R0);
  } else {
    assert(retTy->isVoidTy());
    invalidateReg(BPF::R0, 64);
  }
}

void bpf2llvm::doReturn() {
  auto i32ty = getIntTy(32);
  auto i64ty = getIntTy(64);

  auto *retTyp = srcFn->getReturnType();
  if (retTyp->isVoidTy()) {
    createReturn(nullptr);
    return;
  }

  Value *retVal = readReg64(BPF::R0);
  if (retTyp->isPointerTy()) {
    retVal = new IntToPtrInst(retVal, PointerType::get(Ctx, 0), "", LLVMBB);
  } else {
    auto retWidth = DL.getTypeSizeInBits(retTyp);
    auto retValWidth = DL.getTypeSizeInBits(retVal->getType());

    if (retWidth < retValWidth)
      retVal = createTrunc(retVal, getIntTy(retWidth));

    // mask off any don't-care bits
    if (has_ret_attr && (origRetWidth < 32)) {
      assert(retWidth >= origRetWidth);
      assert(retWidth == 64);
      auto trunc = createTrunc(retVal, i32ty);
      retVal = createZExt(trunc, i64ty);
    }
  }
  createReturn(retVal);
}

void bpf2llvm::checkArgSupport(Argument &arg) {
  auto *ty = arg.getType();
  if (!(ty->isIntegerTy() || ty->isPointerTy())) {
    *out << "\nERROR: only integer/pointer arguments supported on BPF\n\n";
    exit(-1);
  }
}

void bpf2llvm::checkFuncSupport(Function &func) {}

void bpf2llvm::checkTypeSupport(Type *ty) {
  if (ty->isVectorTy()) {
    *out << "\nERROR: vectors not supported on BPF\n\n";
    exit(-1);
  }
  if (ty->isFloatingPointTy()) {
    *out << "\nERROR: floating point not supported on BPF\n\n";
    exit(-1);
  }
  if (ty->isIntegerTy() && ty->getIntegerBitWidth() > 64) {
    *out << "\nERROR: integers wider than 64 bits not supported yet\n\n";
    exit(-1);
  }
}

void bpf2llvm::checkCallingConv(Function *fn) {
  if (fn->getCallingConv() != CallingConv::C) {
    *out << "\nERROR: Only the C calling convention is supported\n\n";
    exit(-1);
  }
}

void bpf2llvm::lift(MCInst &I) {
  auto opcode = I.getOpcode();

  auto i64ty = getIntTy(64);

  // 64-bit ALU: operands (dst, src2, src-or-imm), dst tied to src2
  auto alu64 = [&](Instruction::BinaryOps op) {
    auto a = readRegOperand64(1);
    auto b = readALUSrcOperand(2, 64);
    updateReg64(createBinop(a, b, op), CurInst->getOperand(0).getReg());
  };
  // 32-bit ALU: same layout on W registers; result zero-extends
  auto alu32 = [&](Instruction::BinaryOps op) {
    auto a = readRegOperand32(1);
    auto b = readALUSrcOperand(2, 32);
    updateReg32(createBinop(a, b, op), CurInst->getOperand(0).getReg());
  };
  auto shift64 = [&](Value *(mc2llvm::*shifter)(Value *, Value *)) {
    auto a = readRegOperand64(1);
    auto b = readALUSrcOperand(2, 64);
    updateReg64((this->*shifter)(a, b), CurInst->getOperand(0).getReg());
  };
  auto shift32 = [&](Value *(mc2llvm::*shifter)(Value *, Value *)) {
    auto a = readRegOperand32(1);
    auto b = readALUSrcOperand(2, 32);
    updateReg32((this->*shifter)(a, b), CurInst->getOperand(0).getReg());
  };
  // conditional jump: operands (lhs, rhs-or-imm, target)
  auto condBr = [&](ICmpInst::Predicate pred, unsigned bits) {
    auto a = bits == 64 ? readRegOperand64(0) : readRegOperand32(0);
    auto b = readALUSrcOperand(1, bits);
    auto cond = createICmp(pred, a, b);
    auto [t, f] = getBranchTargetsOperand(2);
    createBranch(cond, t, f);
  };
  // byte swap family: operands (dst, src), dst tied to src.
  // swap = byte-swap the low `bits`, else just mask; result
  // zero-extends to 64 bits
  auto endian = [&](unsigned bits, bool swap) {
    Value *v = readRegOperand64(1);
    if (bits < 64)
      v = createTrunc(v, getIntTy(bits));
    if (swap)
      v = createBSwap(v);
    if (bits < 64)
      v = createZExt(v, i64ty);
    updateReg64(v, CurInst->getOperand(0).getReg());
  };
  // memory loads: operands (dst, base, offset)
  auto load = [&](unsigned bits, bool sext, bool wDest) {
    auto ptr = getMemPointerOperand(1);
    Value *v = createLoad(getIntTy(bits), ptr);
    if (wDest) {
      if (bits < 32)
        v = createZExt(v, getIntTy(32));
      updateReg32(v, CurInst->getOperand(0).getReg());
    } else {
      if (bits < 64)
        v = sext ? createSExt(v, i64ty) : createZExt(v, i64ty);
      updateReg64(v, CurInst->getOperand(0).getReg());
    }
  };
  // memory stores: operands (src, base, offset)
  auto store = [&](unsigned bits, bool wSrc) {
    auto ptr = getMemPointerOperand(1);
    Value *v = wSrc ? readRegOperand32(0) : readRegOperand64(0);
    if (bits < getBitWidth(v))
      v = createTrunc(v, getIntTy(bits));
    createStore(v, ptr);
  };
  // store-immediate: operands (imm, base, offset)
  auto storeImm = [&](unsigned bits) {
    auto ptr = getMemPointerOperand(1);
    auto v = readImmOperand(0, bits);
    createStore(v, ptr);
  };

  switch (opcode) {
  case BPF::NOP:
    break;

  // ---- 64-bit ALU ----
  case BPF::ADD_rr:
  case BPF::ADD_ri:
    alu64(Instruction::Add);
    break;
  case BPF::SUB_rr:
  case BPF::SUB_ri:
    alu64(Instruction::Sub);
    break;
  case BPF::MUL_rr:
  case BPF::MUL_ri:
    alu64(Instruction::Mul);
    break;
  case BPF::AND_rr:
  case BPF::AND_ri:
    alu64(Instruction::And);
    break;
  case BPF::OR_rr:
  case BPF::OR_ri:
    alu64(Instruction::Or);
    break;
  case BPF::XOR_rr:
  case BPF::XOR_ri:
    alu64(Instruction::Xor);
    break;
  case BPF::SLL_rr:
  case BPF::SLL_ri:
    shift64(&mc2llvm::createMaskedShl);
    break;
  case BPF::SRL_rr:
  case BPF::SRL_ri:
    shift64(&mc2llvm::createMaskedLShr);
    break;
  case BPF::SRA_rr:
  case BPF::SRA_ri:
    shift64(&mc2llvm::createMaskedAShr);
    break;

  // per RFC 9669: division by zero produces 0; modulo by zero leaves
  // the destination unchanged; INT_MIN/-1 wraps to INT_MIN (and the
  // corresponding remainder is 0)
  case BPF::DIV_rr:
  case BPF::DIV_ri: {
    auto a = readRegOperand64(1);
    auto b = readALUSrcOperand(2, 64);
    updateReg64(createCheckedUDiv(a, b, getUnsignedIntConst(0, 64)),
                CurInst->getOperand(0).getReg());
    break;
  }
  case BPF::MOD_rr:
  case BPF::MOD_ri: {
    auto a = readRegOperand64(1);
    auto b = readALUSrcOperand(2, 64);
    updateReg64(createCheckedURem(a, b, a), CurInst->getOperand(0).getReg());
    break;
  }
  case BPF::SDIV_rr:
  case BPF::SDIV_ri: {
    auto a = readRegOperand64(1);
    auto b = readALUSrcOperand(2, 64);
    updateReg64(createCheckedSDiv(a, b, getUnsignedIntConst(0, 64), a),
                CurInst->getOperand(0).getReg());
    break;
  }
  case BPF::SMOD_rr:
  case BPF::SMOD_ri: {
    auto a = readRegOperand64(1);
    auto b = readALUSrcOperand(2, 64);
    updateReg64(createCheckedSRem(a, b, a, getUnsignedIntConst(0, 64)),
                CurInst->getOperand(0).getReg());
    break;
  }

  case BPF::NEG_64: {
    auto v = readRegOperand64(1);
    updateReg64(createSub(getUnsignedIntConst(0, 64), v),
                CurInst->getOperand(0).getReg());
    break;
  }
  case BPF::NEG_32: {
    auto v = readRegOperand32(1);
    updateReg32(createSub(getUnsignedIntConst(0, 32), v),
                CurInst->getOperand(0).getReg());
    break;
  }

  // ---- 32-bit ALU ----
  case BPF::ADD_rr_32:
  case BPF::ADD_ri_32:
    alu32(Instruction::Add);
    break;
  case BPF::SUB_rr_32:
  case BPF::SUB_ri_32:
    alu32(Instruction::Sub);
    break;
  case BPF::MUL_rr_32:
  case BPF::MUL_ri_32:
    alu32(Instruction::Mul);
    break;
  case BPF::AND_rr_32:
  case BPF::AND_ri_32:
    alu32(Instruction::And);
    break;
  case BPF::OR_rr_32:
  case BPF::OR_ri_32:
    alu32(Instruction::Or);
    break;
  case BPF::XOR_rr_32:
  case BPF::XOR_ri_32:
    alu32(Instruction::Xor);
    break;
  case BPF::SLL_rr_32:
  case BPF::SLL_ri_32:
    shift32(&mc2llvm::createMaskedShl);
    break;
  case BPF::SRL_rr_32:
  case BPF::SRL_ri_32:
    shift32(&mc2llvm::createMaskedLShr);
    break;
  case BPF::SRA_rr_32:
  case BPF::SRA_ri_32:
    shift32(&mc2llvm::createMaskedAShr);
    break;
  case BPF::DIV_rr_32:
  case BPF::DIV_ri_32: {
    auto a = readRegOperand32(1);
    auto b = readALUSrcOperand(2, 32);
    updateReg32(createCheckedUDiv(a, b, getUnsignedIntConst(0, 32)),
                CurInst->getOperand(0).getReg());
    break;
  }
  case BPF::MOD_rr_32:
  case BPF::MOD_ri_32: {
    auto a = readRegOperand32(1);
    auto b = readALUSrcOperand(2, 32);
    updateReg32(createCheckedURem(a, b, a), CurInst->getOperand(0).getReg());
    break;
  }
  case BPF::SDIV_rr_32:
  case BPF::SDIV_ri_32: {
    auto a = readRegOperand32(1);
    auto b = readALUSrcOperand(2, 32);
    updateReg32(createCheckedSDiv(a, b, getUnsignedIntConst(0, 32), a),
                CurInst->getOperand(0).getReg());
    break;
  }
  case BPF::SMOD_rr_32:
  case BPF::SMOD_ri_32: {
    auto a = readRegOperand32(1);
    auto b = readALUSrcOperand(2, 32);
    updateReg32(createCheckedSRem(a, b, a, getUnsignedIntConst(0, 32)),
                CurInst->getOperand(0).getReg());
    break;
  }

  // ---- moves ----
  case BPF::MOV_rr:
    updateReg64(readRegOperand64(1), CurInst->getOperand(0).getReg());
    break;
  case BPF::MOV_ri:
    updateReg64(readImmOperand(1, 64), CurInst->getOperand(0).getReg());
    break;
  case BPF::MOV_rr_32:
    updateReg32(readRegOperand32(1), CurInst->getOperand(0).getReg());
    break;
  case BPF::MOV_ri_32:
    updateReg32(readImmOperand(1, 32), CurInst->getOperand(0).getReg());
    break;
  case BPF::MOV_32_64:
    // explicit zero-extension of a W register into an R register
    updateReg32(readRegOperand32(1), CurInst->getOperand(0).getReg());
    break;
  case BPF::MOVSX_rr_8: {
    auto v = createTrunc(readRegOperand64(1), getIntTy(8));
    updateReg64(createSExt(v, i64ty), CurInst->getOperand(0).getReg());
    break;
  }
  case BPF::MOVSX_rr_16: {
    auto v = createTrunc(readRegOperand64(1), getIntTy(16));
    updateReg64(createSExt(v, i64ty), CurInst->getOperand(0).getReg());
    break;
  }
  case BPF::MOVSX_rr_32: {
    auto v = readRegOperand32(1);
    updateReg64(createSExt(v, i64ty), CurInst->getOperand(0).getReg());
    break;
  }
  case BPF::MOVSX_rr_32_8: {
    auto v = createTrunc(readRegOperand32(1), getIntTy(8));
    updateReg32(createSExt(v, getIntTy(32)), CurInst->getOperand(0).getReg());
    break;
  }
  case BPF::MOVSX_rr_32_16: {
    auto v = createTrunc(readRegOperand32(1), getIntTy(16));
    updateReg32(createSExt(v, getIntTy(32)), CurInst->getOperand(0).getReg());
    break;
  }

  case BPF::LD_imm64: {
    auto &op = CurInst->getOperand(1);
    Value *v{nullptr};
    if (op.isImm()) {
      v = getUnsignedIntConst((uint64_t)op.getImm(), 64);
    } else {
      assert(op.isExpr());
      auto [g, spec] = getExprVar(op.getExpr());
      v = new PtrToIntInst(g, i64ty, "", LLVMBB);
    }
    updateReg64(v, CurInst->getOperand(0).getReg());
    break;
  }

  // ---- endian conversions (target is little-endian) ----
  case BPF::BE16:
    endian(16, /*swap=*/true);
    break;
  case BPF::BE32:
    endian(32, /*swap=*/true);
    break;
  case BPF::BE64:
    endian(64, /*swap=*/true);
    break;
  case BPF::LE16:
    endian(16, /*swap=*/false);
    break;
  case BPF::LE32:
    endian(32, /*swap=*/false);
    break;
  case BPF::LE64:
    endian(64, /*swap=*/false);
    break;

  // ---- memory ----
  case BPF::LDD:
    load(64, false, false);
    break;
  case BPF::LDW:
    load(32, false, false);
    break;
  case BPF::LDH:
    load(16, false, false);
    break;
  case BPF::LDB:
    load(8, false, false);
    break;
  case BPF::LDWSX:
    load(32, true, false);
    break;
  case BPF::LDHSX:
    load(16, true, false);
    break;
  case BPF::LDBSX:
    load(8, true, false);
    break;
  case BPF::LDW32:
    load(32, false, true);
    break;
  case BPF::LDH32:
    load(16, false, true);
    break;
  case BPF::LDB32:
    load(8, false, true);
    break;
  case BPF::STD:
    store(64, false);
    break;
  case BPF::STW:
    store(32, false);
    break;
  case BPF::STH:
    store(16, false);
    break;
  case BPF::STB:
    store(8, false);
    break;
  case BPF::STW32:
    store(32, true);
    break;
  case BPF::STH32:
    store(16, true);
    break;
  case BPF::STB32:
    store(8, true);
    break;
  case BPF::STD_imm:
    storeImm(64);
    break;
  case BPF::STW_imm:
    storeImm(32);
    break;
  case BPF::STH_imm:
    storeImm(16);
    break;
  case BPF::STB_imm:
    storeImm(8);
    break;

  // ---- control flow ----
  case BPF::JMP: {
    auto &op = CurInst->getOperand(0);
    if (op.isExpr()) {
      auto *bb = getBB(CurInst->getOperand(0));
      assert(bb);
      createBranch(bb);
    } else {
      // synthetic branch inserted by checkEntryBlock(); its target is
      // the block's unique successor
      assert(MCBB->getSuccs().size() == 1);
      createBranch(getBBByName(MCBB->getSuccs()[0]->getName()));
    }
    break;
  }
  case BPF::JEQ_rr:
  case BPF::JEQ_ri:
    condBr(ICmpInst::ICMP_EQ, 64);
    break;
  case BPF::JNE_rr:
  case BPF::JNE_ri:
    condBr(ICmpInst::ICMP_NE, 64);
    break;
  case BPF::JUGT_rr:
  case BPF::JUGT_ri:
    condBr(ICmpInst::ICMP_UGT, 64);
    break;
  case BPF::JUGE_rr:
  case BPF::JUGE_ri:
    condBr(ICmpInst::ICMP_UGE, 64);
    break;
  case BPF::JULT_rr:
  case BPF::JULT_ri:
    condBr(ICmpInst::ICMP_ULT, 64);
    break;
  case BPF::JULE_rr:
  case BPF::JULE_ri:
    condBr(ICmpInst::ICMP_ULE, 64);
    break;
  case BPF::JSGT_rr:
  case BPF::JSGT_ri:
    condBr(ICmpInst::ICMP_SGT, 64);
    break;
  case BPF::JSGE_rr:
  case BPF::JSGE_ri:
    condBr(ICmpInst::ICMP_SGE, 64);
    break;
  case BPF::JSLT_rr:
  case BPF::JSLT_ri:
    condBr(ICmpInst::ICMP_SLT, 64);
    break;
  case BPF::JSLE_rr:
  case BPF::JSLE_ri:
    condBr(ICmpInst::ICMP_SLE, 64);
    break;
  case BPF::JEQ_rr_32:
  case BPF::JEQ_ri_32:
    condBr(ICmpInst::ICMP_EQ, 32);
    break;
  case BPF::JNE_rr_32:
  case BPF::JNE_ri_32:
    condBr(ICmpInst::ICMP_NE, 32);
    break;
  case BPF::JUGT_rr_32:
  case BPF::JUGT_ri_32:
    condBr(ICmpInst::ICMP_UGT, 32);
    break;
  case BPF::JUGE_rr_32:
  case BPF::JUGE_ri_32:
    condBr(ICmpInst::ICMP_UGE, 32);
    break;
  case BPF::JULT_rr_32:
  case BPF::JULT_ri_32:
    condBr(ICmpInst::ICMP_ULT, 32);
    break;
  case BPF::JULE_rr_32:
  case BPF::JULE_ri_32:
    condBr(ICmpInst::ICMP_ULE, 32);
    break;
  case BPF::JSGT_rr_32:
  case BPF::JSGT_ri_32:
    condBr(ICmpInst::ICMP_SGT, 32);
    break;
  case BPF::JSGE_rr_32:
  case BPF::JSGE_ri_32:
    condBr(ICmpInst::ICMP_SGE, 32);
    break;
  case BPF::JSLT_rr_32:
  case BPF::JSLT_ri_32:
    condBr(ICmpInst::ICMP_SLT, 32);
    break;
  case BPF::JSLE_rr_32:
  case BPF::JSLE_ri_32:
    condBr(ICmpInst::ICMP_SLE, 32);
    break;
  case BPF::JSET_rr:
  case BPF::JSET_ri: {
    auto a = readRegOperand64(0);
    auto b = readALUSrcOperand(1, 64);
    auto cond = createICmp(ICmpInst::ICMP_NE, createAnd(a, b),
                           getUnsignedIntConst(0, 64));
    auto [t, f] = getBranchTargetsOperand(2);
    createBranch(cond, t, f);
    break;
  }

  case BPF::JAL:
    doDirectCall();
    break;

  case BPF::RET:
    doReturn();
    break;

  default:
    visitError();
  }
}
