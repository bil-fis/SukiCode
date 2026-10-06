#include "compiler/codegen/IRGenerator.h"

#include "compiler/codegen/TypeLayout.h"
#include "compiler/sema/Sema.h"
#include "compiler/sema/Type.h"
#include "compiler/lexer/Token.h" // punctToString

// LLVM headers live ONLY in this file (strict PIMPL).
#include <cerrno>
#include <limits>
#include <unordered_map>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/TypeSize.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>

// BLK-B: in-process object emission needs the legacy pass manager and the file
// system helpers. These headers live only in this TU (strict PIMPL).
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/FileSystem.h>
#include <system_error>

#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

namespace suki {

class IRGenerator::Impl {
public:
    explicit Impl(const TargetInfo& t) : target_(t) { setup(); }
    const TargetInfo& target() const { return target_; }

private:
    void setup() {
        static bool initialized = false;
        if (!initialized) {
            llvm::InitializeAllTargetInfos();
            llvm::InitializeAllTargets();
            llvm::InitializeAllTargetMCs();
            // BLK-B: in-process object emission needs the AsmPrinter backend that
            // turns MC instructions into an ELF object; without it
            // addPassesToEmitFile(CGFT_ObjectFile) fails.
            llvm::InitializeAllAsmPrinters();
            llvm::InitializeAllAsmParsers();
            initialized = true;
        }
        ctx_ = std::make_unique<llvm::LLVMContext>();
        module_ = std::make_unique<llvm::Module>("sukic", *ctx_);
        module_->setTargetTriple(target_.triple);
        b_ = std::make_unique<llvm::IRBuilder<>>(*ctx_);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // 协作组件：并发 lowering（async / Future / Channel<T> / select / TaskGroup /
    // Task / for await）。该组件持有对 Impl 的回指（owner_），共享 module_ / b_ /
    // ctx_ / layout_ 与函数/局部符号表；组件间（与表达式生成、ARC、异常）通过
    // owner_ 互相调用，保持调用点不变。这样把「最容易出竞争问题」的并发代码从
    // 巨型 Impl 中隔离出来，单独推理。
    // ─────────────────────────────────────────────────────────────────────────
    class ConcurrencyLowerer {
    public:
        explicit ConcurrencyLowerer(Impl* o) : owner_(o) {}
        Impl* owner_;

        // Synchronous runtime builtins with fixed C prototypes.
        llvm::Value* genSyncBuiltin(const std::string& name, CallExpr* e) {
            llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
            llvm::Type* i64p = llvm::PointerType::getUnqual(i64);
            llvm::Type* voidTy = llvm::Type::getVoidTy(*owner_->ctx_);
            std::string cname;
            std::vector<llvm::Type*> ptys;
            llvm::Type* rty = voidTy;
            if (name == "atomicAdd")         { cname = "suki_atomic_add_i64"; ptys = {i64p, i64}; rty = i64; }
            else if (name == "mutexLock")    { cname = "suki_spin_lock";      ptys = {i64p};      rty = voidTy; }
            else if (name == "mutexUnlock")  { cname = "suki_spin_unlock";    ptys = {i64p};      rty = voidTy; }
            else return nullptr;
            llvm::Function* fn = owner_->declareExternalSig(cname, ptys, rty);
            std::vector<llvm::Value*> args;
            for (auto& a : e->arguments) {
                llvm::Value* v = owner_->genExpr(a.get());
                if (!v) return nullptr;
                args.push_back(v);
            }
            for (size_t i = 0; i < args.size() && i < ptys.size(); ++i)
                args[i] = owner_->coerce(args[i], ptys[i]);
            return owner_->b_->CreateCall(fn, args);
        }

        // Runtime entry points used by async lowering.
        llvm::Function* asyncAllocFn() {
            return owner_->declareExternalSig("suki_alloc", { llvm::Type::getInt64Ty(*owner_->ctx_) },
                                              llvm::PointerType::get(*owner_->ctx_, 0));
        }
        llvm::Function* asyncFreeFn() {
            return owner_->declareExternalSig("suki_free", { llvm::PointerType::get(*owner_->ctx_, 0) },
                                              llvm::Type::getVoidTy(*owner_->ctx_));
        }
        llvm::Function* asyncFutureCreateFn() {
            return owner_->declareExternalSig("suki_future_create",
                                              { llvm::Type::getInt64Ty(*owner_->ctx_) },
                                              llvm::PointerType::get(*owner_->ctx_, 0));
        }

        // Emit `<sym>_spawn` for an async function.
        void genAsyncSpawn(FunctionDecl* fn, const std::string& sym) {
            const std::string spawnSym = sym + "_spawn";
            if (owner_->fns_.count(spawnSym)) return;
            llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
            llvm::Type* voidTy = llvm::Type::getVoidTy(*owner_->ctx_);
            llvm::PointerType* i8p = llvm::PointerType::get(*owner_->ctx_, 0);

            std::vector<llvm::Type*> ptys;
            for (auto& p : fn->params)
                ptys.push_back(p.semaType ? owner_->layout_->lower(p.semaType) : i64);

            const Type* rt = fn->returnType ? fn->returnType->semaType : nullptr;
            const bool isVoid = !rt || rt->kind == TypeKind::Void ||
                                rt->kind == TypeKind::Unknown;
            llvm::Type* rty = isVoid ? nullptr : owner_->layout_->lower(rt);
            const int64_t rsize = isVoid ? 0 : (int64_t)owner_->sizeOf(rty);

            std::vector<llvm::Type*> ctys = ptys;
            ctys.push_back(i8p);
            llvm::StructType* ctxTy = llvm::StructType::get(*owner_->ctx_, ctys, false);
            const unsigned futIdx = (unsigned)ptys.size();

            llvm::Function* bodyFn = owner_->fns_[fn->name];
            if (!bodyFn) bodyFn = owner_->declareAs(fn, fn->name);

            llvm::Function* storeFn = owner_->declareExternalSig("suki_future_store",
                                                                { i8p, i8p }, voidTy);
            llvm::Function* finishFn = owner_->declareExternalSig("suki_future_finish",
                                                                 { i8p }, voidTy);

            llvm::FunctionType* trampTy = llvm::FunctionType::get(i8p, { i8p }, false);
            llvm::Function* tramp = llvm::Function::Create(
                trampTy, llvm::GlobalValue::InternalLinkage, sym + "_tramp",
                owner_->module_.get());
            {
                llvm::BasicBlock* tb = llvm::BasicBlock::Create(*owner_->ctx_, "entry", tramp);
                owner_->b_->SetInsertPoint(tb);
                llvm::Value* rawCtx = tramp->getArg(0);
                llvm::Value* cp = owner_->b_->CreateBitCast(rawCtx,
                    llvm::PointerType::getUnqual(ctxTy), "ctx");
                std::vector<llvm::Value*> callArgs;
                for (size_t i = 0; i < ptys.size(); ++i) {
                    llvm::Value* fp = owner_->b_->CreateStructGEP(ctxTy, cp, (unsigned)i,
                                                                  "arg.addr");
                    callArgs.push_back(owner_->b_->CreateLoad(ptys[i], fp));
                }
                llvm::Value* futAddr = owner_->b_->CreateStructGEP(ctxTy, cp, futIdx, "fut.addr");
                llvm::Value* fut = owner_->b_->CreateLoad(i8p, futAddr, "fut");
                if (!isVoid) {
                    llvm::Value* v = owner_->b_->CreateCall(bodyFn, callArgs);
                    llvm::Value* slot = owner_->b_->CreateAlloca(rty, nullptr, "result");
                    owner_->b_->CreateStore(v, slot);
                    owner_->b_->CreateCall(storeFn, { fut, owner_->b_->CreateBitCast(slot, i8p) });
                } else {
                    owner_->b_->CreateCall(bodyFn, callArgs);
                }
                owner_->b_->CreateCall(finishFn, { fut });
                owner_->b_->CreateCall(asyncFreeFn(), { rawCtx });
                owner_->b_->CreateRet(llvm::ConstantPointerNull::get(i8p));
            }

            llvm::FunctionType* sft = llvm::FunctionType::get(i8p, ptys, false);
            llvm::Function* spawnFn = llvm::Function::Create(
                sft, llvm::GlobalValue::InternalLinkage, spawnSym, owner_->module_.get());
            {
                llvm::BasicBlock* sb = llvm::BasicBlock::Create(*owner_->ctx_, "entry", spawnFn);
                owner_->b_->SetInsertPoint(sb);
                llvm::Value* fut = owner_->b_->CreateCall(asyncFutureCreateFn(),
                                                  { llvm::ConstantInt::get(i64, rsize) });
                llvm::Value* rawCtx = owner_->b_->CreateCall(asyncAllocFn(),
                    { llvm::ConstantInt::get(i64, (int64_t)owner_->sizeOf(ctxTy)) });
                llvm::Value* cp = owner_->b_->CreateBitCast(rawCtx,
                    llvm::PointerType::getUnqual(ctxTy), "ctx");
                for (size_t i = 0; i < ptys.size(); ++i) {
                    llvm::Value* fp = owner_->b_->CreateStructGEP(ctxTy, cp, (unsigned)i,
                                                                  "arg.addr");
                    owner_->b_->CreateStore(spawnFn->getArg((unsigned)i), fp);
                }
                llvm::Value* futAddr = owner_->b_->CreateStructGEP(ctxTy, cp, futIdx, "fut.addr");
                owner_->b_->CreateStore(fut, futAddr);

                llvm::Function* startFn = owner_->declareExternalSig("suki_thread_start",
                    { llvm::PointerType::getUnqual(trampTy), i8p },
                    llvm::Type::getInt32Ty(*owner_->ctx_));
                llvm::Value* started = owner_->b_->CreateCall(startFn, { tramp, rawCtx },
                                                              "started");
                llvm::Value* failed = owner_->b_->CreateICmpNE(started,
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(*owner_->ctx_), 0));
                llvm::BasicBlock* inlineBB = llvm::BasicBlock::Create(*owner_->ctx_, "inline",
                                                                      spawnFn);
                llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*owner_->ctx_, "spawned",
                                                                    spawnFn);
                owner_->b_->CreateCondBr(failed, inlineBB, doneBB);
                owner_->b_->SetInsertPoint(inlineBB);
                owner_->b_->CreateCall(tramp, { rawCtx });
                owner_->b_->CreateBr(doneBB);
                owner_->b_->SetInsertPoint(doneBB);
                owner_->b_->CreateRet(fut);
            }
            owner_->fns_[spawnSym] = spawnFn;
        }

        // `Channel<T>` allocation.
        llvm::Value* genChannelInit(const Type* instTy, CallExpr* e) {
            llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
            llvm::PointerType* i8p = llvm::PointerType::get(*owner_->ctx_, 0);
            const Type* elemTy = (!instTy->elements.empty())
                                     ? instTy->elements[0] : nullptr;
            llvm::Type* elemLLVM = elemTy ? owner_->layout_->lower(elemTy) : i64;
            const int64_t esz = (int64_t)owner_->sizeOf(elemLLVM);

            llvm::Value* cap = llvm::ConstantInt::get(i64, 1);
            if (!e->arguments.empty()) {
                llvm::Value* cv = owner_->genExpr(e->arguments[0].get());
                if (cv) cap = owner_->coerce(cv, i64);
            }
            llvm::Function* createFn = owner_->declareExternalSig("suki_channel_create",
                                                              { i64, i64 }, i8p);
            llvm::Value* handle = owner_->b_->CreateCall(createFn,
                { cap, llvm::ConstantInt::get(i64, esz) });

            llvm::Type* sty = owner_->layout_->lower(instTy);
            if (!sty || !sty->isStructTy()) return nullptr;
            llvm::Value* slot = owner_->b_->CreateAlloca(sty, nullptr, "channel.tmp");
            owner_->b_->CreateStore(llvm::Constant::getNullValue(sty), slot);
            llvm::Value* hp = owner_->b_->CreateStructGEP(sty, slot, 0, "handle.addr");
            owner_->b_->CreateStore(handle, hp);
            return owner_->b_->CreateLoad(sty, slot);
        }

        // Channel methods are runtime-backed.
        void genChannelMethodBody(const std::string& method, const Type* instTy,
                                  llvm::Function* f) {
            if (!f || !f->empty()) return;
            llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
            llvm::PointerType* i8p = llvm::PointerType::get(*owner_->ctx_, 0);
            llvm::Type* voidTy = llvm::Type::getVoidTy(*owner_->ctx_);
            const Type* elemTy = (!instTy->elements.empty())
                                     ? instTy->elements[0] : nullptr;
            llvm::Type* elemLLVM = elemTy ? owner_->layout_->lower(elemTy) : i64;

            llvm::Type* selfTy = f->getFunctionType()->getParamType(0);
            owner_->b_->SetInsertPoint(llvm::BasicBlock::Create(*owner_->ctx_, "entry", f));
            llvm::Value* selfSlot = owner_->b_->CreateAlloca(selfTy, nullptr, "self");
            owner_->b_->CreateStore(f->getArg(0), selfSlot);
            llvm::Value* hp = owner_->b_->CreateStructGEP(selfTy, selfSlot, 0, "handle.addr");
            llvm::Value* handle = owner_->b_->CreateLoad(i8p, hp, "handle");

            if (method == "send") {
                llvm::Value* vSlot = owner_->b_->CreateAlloca(elemLLVM, nullptr, "value");
                owner_->b_->CreateStore(owner_->coerce(f->getArg(1), elemLLVM), vSlot);
                llvm::Function* sf = owner_->declareExternalSig("suki_channel_send",
                                                                { i8p, i8p }, voidTy);
                owner_->b_->CreateCall(sf, { handle, owner_->b_->CreateBitCast(vSlot, i8p) });
                owner_->b_->CreateRetVoid();
            } else if (method == "receive") {
                llvm::Value* out = owner_->b_->CreateAlloca(elemLLVM, nullptr, "out");
                llvm::Function* rf = owner_->declareExternalSig("suki_channel_receive",
                                                                { i8p, i8p },
                                                                llvm::Type::getInt32Ty(*owner_->ctx_));
                owner_->b_->CreateCall(rf, { handle, owner_->b_->CreateBitCast(out, i8p) });
                owner_->b_->CreateRet(owner_->b_->CreateLoad(elemLLVM, out));
            } else if (method == "close") {
                llvm::Function* cf = owner_->declareExternalSig("suki_channel_close",
                                                                { i8p }, voidTy);
                owner_->b_->CreateCall(cf, { handle });
                owner_->b_->CreateRetVoid();
            } else {
                owner_->b_->CreateRetVoid();
            }
        }

        // ── 并发剩余：select / for await / Future / Channel 方法 / Task / TaskGroup ──
        void genSelect(SwitchStmt* s) {
            if (!s) return;
            llvm::Function* cur = owner_->b_->GetInsertBlock()->getParent();
            llvm::BasicBlock* retry = llvm::BasicBlock::Create(*owner_->ctx_, "select.retry", cur);
            llvm::BasicBlock* done = llvm::BasicBlock::Create(*owner_->ctx_, "select.done", cur);
            owner_->b_->CreateBr(retry);
            owner_->b_->SetInsertPoint(retry);

            llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
            llvm::PointerType* i8p = llvm::PointerType::get(*owner_->ctx_, 0);
            llvm::Type* voidTy = llvm::Type::getVoidTy(*owner_->ctx_);

            for (auto& c : s->cases) {
                if (!c || c->kind != NodeKind::CaseClause) continue;
                auto* cc = static_cast<CaseClause*>(c.get());
                if (cc->isDefault || !cc->pattern) continue;
                if (cc->pattern->kind != NodeKind::BinaryExpr) continue;
                auto* be = static_cast<BinaryExpr*>(cc->pattern.get());
                if (be->op != PunctuatorID::LeftArrow) continue;

                const bool isReceive = cc->isBindingPattern || (!cc->bindings.empty());
                const Type* chTy = be->rhs ? be->rhs->semaType : nullptr;
                const Type* elemTy = (chTy && !chTy->elements.empty())
                                         ? chTy->elements[0] : nullptr;
                llvm::Type* elemLLVM = elemTy ? owner_->layout_->lower(elemTy) : i64;
                llvm::Value* chVal = owner_->genExpr(be->rhs.get());
                if (!chVal) continue;
                llvm::Type* chStruct = chVal->getType();
                llvm::Value* chSlot = owner_->b_->CreateAlloca(chStruct, nullptr, "sel.chan");
                owner_->b_->CreateStore(chVal, chSlot);
                llvm::Value* hp = owner_->b_->CreateStructGEP(chStruct, chSlot, 0, "sel.h");
                llvm::Value* handle = owner_->b_->CreateLoad(i8p, hp, "sel.handle");

                llvm::Value* ok = nullptr;
                llvm::Value* outSlot = nullptr;
                if (isReceive) {
                    outSlot = owner_->b_->CreateAlloca(elemLLVM, nullptr, "sel.recv");
                    llvm::Function* tf = owner_->declareExternalSig("suki_channel_try_receive",
                        { i8p, i8p }, llvm::Type::getInt32Ty(*owner_->ctx_));
                    ok = owner_->b_->CreateCall(tf, { handle,
                        owner_->b_->CreateBitCast(outSlot, i8p) });
                } else {
                    llvm::Value* v = owner_->genExpr(be->lhs.get());
                    if (!v) continue;
                    llvm::Value* vSlot = owner_->b_->CreateAlloca(elemLLVM, nullptr, "sel.send");
                    owner_->b_->CreateStore(owner_->coerce(v, elemLLVM), vSlot);
                    llvm::Function* tf = owner_->declareExternalSig("suki_channel_try_send",
                        { i8p, i8p }, llvm::Type::getInt32Ty(*owner_->ctx_));
                    ok = owner_->b_->CreateCall(tf, { handle,
                        owner_->b_->CreateBitCast(vSlot, i8p) });
                }
                llvm::Value* ready = owner_->b_->CreateICmpEQ(ok,
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(*owner_->ctx_), 1));
                llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*owner_->ctx_, "sel.case", cur);
                llvm::BasicBlock* nextBB = llvm::BasicBlock::Create(*owner_->ctx_, "sel.next", cur);
                owner_->b_->CreateCondBr(ready, bodyBB, nextBB);
                owner_->b_->SetInsertPoint(bodyBB);
                if (isReceive && outSlot && !cc->bindings.empty()) {
                    llvm::Value* val = owner_->b_->CreateLoad(elemLLVM, outSlot, "sel.value");
                    llvm::Value* slot = owner_->b_->CreateAlloca(elemLLVM, nullptr,
                                                                 cc->bindings[0]);
                    owner_->b_->CreateStore(val, slot);
                    owner_->locals_[cc->bindings[0]] = slot;
                }
                for (auto& st : cc->body) owner_->genStmt(st.get());
                owner_->b_->CreateBr(done);
                owner_->b_->SetInsertPoint(nextBB);
            }

            bool hasDefault = false;
            for (auto& c : s->cases) {
                if (!c || c->kind != NodeKind::CaseClause) continue;
                if (static_cast<CaseClause*>(c.get())->isDefault) { hasDefault = true; break; }
            }
            if (hasDefault) {
                for (auto& c : s->cases) {
                    if (!c || c->kind != NodeKind::CaseClause) continue;
                    auto* cc = static_cast<CaseClause*>(c.get());
                    if (!cc->isDefault) continue;
                    for (auto& st : cc->body) owner_->genStmt(st.get());
                    break;
                }
                owner_->b_->CreateBr(done);
            } else {
                llvm::Function* sleepFn = owner_->declareExternalSig("suki_sleep", { i64 },
                                                                     voidTy);
                owner_->b_->CreateCall(sleepFn, { llvm::ConstantInt::get(i64, 1) });
                owner_->b_->CreateBr(retry);
            }
            owner_->b_->SetInsertPoint(done);
        }

        void genForAwait(ForInStmt* fr) {
            llvm::Function* f = owner_->b_->GetInsertBlock()->getParent();
            llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
            llvm::PointerType* i8p = llvm::PointerType::get(*owner_->ctx_, 0);
            llvm::Type* i32 = llvm::Type::getInt32Ty(*owner_->ctx_);

            llvm::Value* seq = fr->sequence ? owner_->genExpr(fr->sequence.get()) : nullptr;
            if (!seq) return;
            llvm::Type* seqTy = seq->getType();

            const Type* seqType = fr->sequence ? fr->sequence->semaType : nullptr;
            const Type* elemTy = (seqType && seqType->kind == TypeKind::Named &&
                                  !seqType->elements.empty()) ? seqType->elements[0]
                                                             : nullptr;
            llvm::Type* elemLLVM = elemTy ? owner_->layout_->lower(elemTy) : i64;

            const std::string seqName = (seqType && seqType->kind == TypeKind::Named)
                                           ? seqType->name : std::string();
            const bool isTaskGroup = (seqName.rfind("TaskGroup", 0) == 0);

            llvm::Value* hslot = owner_->b_->CreateAlloca(seqTy, nullptr, "fa.seq");
            owner_->b_->CreateStore(seq, hslot);
            llvm::Value* hp = owner_->b_->CreateStructGEP(seqTy, hslot, 0, "fa.handle.addr");
            llvm::Value* handle = owner_->b_->CreateLoad(i8p, hp, "fa.handle");

            std::string vname;
            VarDecl* pat = fr->pattern && fr->pattern->kind == NodeKind::VarDecl
                               ? static_cast<VarDecl*>(fr->pattern.get()) : nullptr;
            if (pat && !pat->name.empty()) vname = pat->name;

            llvm::Value* out = owner_->b_->CreateAlloca(elemLLVM, nullptr, "fa.out");
            llvm::Value* idxSlot = nullptr;
            if (isTaskGroup)
                idxSlot = owner_->b_->CreateAlloca(i64, nullptr, "fa.i");
            llvm::BasicBlock* headBB = llvm::BasicBlock::Create(*owner_->ctx_, "fa.head", f);
            llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*owner_->ctx_, "fa.body", f);
            llvm::BasicBlock* stepBB = llvm::BasicBlock::Create(*owner_->ctx_, "fa.step", f);
            llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(*owner_->ctx_, "fa.exit", f);
            if (isTaskGroup)
                owner_->b_->CreateStore(llvm::ConstantInt::get(i64, 0), idxSlot);
            owner_->b_->CreateBr(headBB);
            owner_->b_->SetInsertPoint(headBB);

            if (isTaskGroup) {
                llvm::Value* cnt = owner_->b_->CreateCall(
                    owner_->declareExternalSig("suki_taskgroup_count", { i8p }, i64), { handle });
                llvm::Value* i = owner_->b_->CreateLoad(i64, idxSlot);
                owner_->b_->CreateCondBr(owner_->b_->CreateICmpSLT(i, cnt), bodyBB, exitBB);
                owner_->b_->SetInsertPoint(bodyBB);
                llvm::Value* ii = owner_->b_->CreateLoad(i64, idxSlot);
                owner_->b_->CreateCall(
                    owner_->declareExternalSig("suki_taskgroup_result_at", { i8p, i64, i8p }, i32),
                    { handle, ii, owner_->b_->CreateBitCast(out, i8p) });
            } else {
                llvm::Value* ok = owner_->b_->CreateCall(
                    owner_->declareExternalSig("suki_channel_receive", { i8p, i8p }, i32),
                    { handle, owner_->b_->CreateBitCast(out, i8p) });
                owner_->b_->CreateCondBr(owner_->b_->CreateICmpEQ(ok, llvm::ConstantInt::get(i32, 0)),
                                         bodyBB, exitBB);
                owner_->b_->SetInsertPoint(bodyBB);
            }

            if (!vname.empty()) {
                llvm::Value* vslot = owner_->b_->CreateAlloca(elemLLVM, nullptr, vname);
                owner_->b_->CreateStore(owner_->b_->CreateLoad(elemLLVM, out), vslot);
                owner_->locals_[vname] = vslot;
                owner_->trackScopedRef(vname, pat && pat->semaType ? pat->semaType : nullptr);
            }
            owner_->breakTargets_.push_back(exitBB);
            owner_->continueTargets_.push_back(stepBB);
            for (auto& st : fr->body) owner_->genStmt(st.get());
            owner_->breakTargets_.pop_back();
            owner_->continueTargets_.pop_back();
            if (!owner_->b_->GetInsertBlock()->getTerminator())
                owner_->b_->CreateBr(stepBB);
            owner_->b_->SetInsertPoint(stepBB);
            if (isTaskGroup && idxSlot) {
                llvm::Value* ni = owner_->b_->CreateAdd(owner_->b_->CreateLoad(i64, idxSlot),
                                                       llvm::ConstantInt::get(i64, 1));
                owner_->b_->CreateStore(ni, idxSlot);
            }
            owner_->b_->CreateBr(headBB);
            owner_->b_->SetInsertPoint(exitBB);
        }

        // 已完成的 Future，供运行时支撑的 async 成员使用。
        llvm::Value* asyncCompletedFuture(llvm::Type* valueTy, llvm::Value* valuePtr) {
            llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
            llvm::PointerType* i8p = llvm::PointerType::get(*owner_->ctx_, 0);
            llvm::Type* voidTy = llvm::Type::getVoidTy(*owner_->ctx_);
            const int64_t sz = valueTy ? (int64_t)owner_->sizeOf(valueTy) : 0;
            llvm::Value* fut = owner_->b_->CreateCall(asyncFutureCreateFn(),
                { llvm::ConstantInt::get(i64, sz) });
            if (valueTy && valuePtr)
                owner_->b_->CreateCall(owner_->declareExternalSig("suki_future_store",
                    { i8p, i8p }, voidTy), { fut, owner_->b_->CreateBitCast(valuePtr, i8p) });
            owner_->b_->CreateCall(owner_->declareExternalSig("suki_future_finish",
                { i8p }, voidTy), { fut });
            return fut;
        }

        void genChannelMethod(FunctionDecl* mf, const Type* instTy) {
            const std::string mkey = std::string(instTy->record->name) + "." + mf->name;
            if (owner_->methodFns_.count(mkey)) return;
            llvm::PointerType* i8p = llvm::PointerType::get(*owner_->ctx_, 0);
            llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
            llvm::Type* voidTy = llvm::Type::getVoidTy(*owner_->ctx_);
            const Type* elemTy = (!instTy->elements.empty()) ? instTy->elements[0] : nullptr;
            llvm::Type* elemLLVM = elemTy ? owner_->layout_->lower(elemTy) : i64;
            llvm::Type* selfTy = owner_->layout_->lower(instTy);
            if (!selfTy) return;

            std::vector<llvm::Type*> ptys{ selfTy };
            if (mf->name == "send") ptys.push_back(elemLLVM);
            llvm::FunctionType* fty = llvm::FunctionType::get(i8p, ptys, false);
            llvm::Function* f = llvm::Function::Create(fty,
                llvm::GlobalValue::InternalLinkage, mkey, owner_->module_.get());
            owner_->b_->SetInsertPoint(llvm::BasicBlock::Create(*owner_->ctx_, "entry", f));

            llvm::Value* selfSlot = owner_->b_->CreateAlloca(selfTy, nullptr, "self");
            owner_->b_->CreateStore(f->getArg(0), selfSlot);
            llvm::Value* hp = owner_->b_->CreateStructGEP(selfTy, selfSlot, 0, "handle.addr");
            llvm::Value* handle = owner_->b_->CreateLoad(i8p, hp, "handle");

            if (mf->name == "send") {
                llvm::Value* vSlot = owner_->b_->CreateAlloca(elemLLVM, nullptr, "value");
                owner_->b_->CreateStore(owner_->coerce(f->getArg(1), elemLLVM), vSlot);
                owner_->b_->CreateCall(owner_->declareExternalSig("suki_channel_send",
                    { i8p, i8p }, voidTy), { handle, owner_->b_->CreateBitCast(vSlot, i8p) });
                owner_->b_->CreateRet(asyncCompletedFuture(nullptr, nullptr));
            } else if (mf->name == "receive") {
                llvm::Value* out = owner_->b_->CreateAlloca(elemLLVM, nullptr, "out");
                owner_->b_->CreateCall(owner_->declareExternalSig("suki_channel_receive",
                    { i8p, i8p }, llvm::Type::getInt32Ty(*owner_->ctx_)),
                    { handle, owner_->b_->CreateBitCast(out, i8p) });
                owner_->b_->CreateRet(asyncCompletedFuture(elemLLVM, out));
            } else if (mf->name == "close") {
                owner_->b_->CreateCall(owner_->declareExternalSig("suki_channel_close",
                    { i8p }, voidTy), { handle });
                owner_->b_->CreateRet(asyncCompletedFuture(nullptr, nullptr));
            } else {
                owner_->b_->CreateRet(asyncCompletedFuture(nullptr, nullptr));
            }
            owner_->methodFns_[mkey] = f;
        }

        void genTaskGroupMethod(FunctionDecl* mf, const Type* instTy) {
            const std::string mkey = std::string(instTy->name) + "." + mf->name;
            if (owner_->methodFns_.count(mkey)) return;
            llvm::PointerType* i8p = llvm::PointerType::get(*owner_->ctx_, 0);
            llvm::Type* voidTy = llvm::Type::getVoidTy(*owner_->ctx_);
            const Type* elemTy = (!instTy->elements.empty()) ? instTy->elements[0] : nullptr;
            llvm::Type* elemLLVM = elemTy ? owner_->layout_->lower(elemTy) : nullptr;
            llvm::Type* selfTy = owner_->layout_->lower(instTy);
            if (!selfTy) return;

            if (mf->name == "waitForAll") {
                llvm::FunctionType* fty = llvm::FunctionType::get(voidTy, { selfTy }, false);
                llvm::Function* f = llvm::Function::Create(fty,
                    llvm::GlobalValue::InternalLinkage, mkey, owner_->module_.get());
                owner_->b_->SetInsertPoint(llvm::BasicBlock::Create(*owner_->ctx_, "entry", f));
                llvm::Value* g = loadHandle(f->getArg(0), selfTy);
                owner_->b_->CreateCall(owner_->declareExternalSig("suki_taskgroup_wait_all",
                    { i8p }, voidTy), { g });
                owner_->b_->CreateRetVoid();
                owner_->methodFns_[mkey] = f;
                return;
            }
            if (mf->name != "addTask") return;

            llvm::StructType* clTy = llvm::cast<llvm::StructType>(
                owner_->layout_->closureTy(llvm::FunctionType::get(voidTy, { i8p }, false)));
            llvm::FunctionType* fty = llvm::FunctionType::get(voidTy, { selfTy, clTy }, false);
            llvm::Function* f = llvm::Function::Create(fty,
                llvm::GlobalValue::InternalLinkage, mkey, owner_->module_.get());
            owner_->b_->SetInsertPoint(llvm::BasicBlock::Create(*owner_->ctx_, "entry", f));
            llvm::Value* g = loadHandle(f->getArg(0), selfTy);
            llvm::Value* fut = startClosureTask(f->getArg(1), f, elemLLVM);
            owner_->b_->CreateCall(owner_->declareExternalSig("suki_taskgroup_add",
                { i8p, i8p }, voidTy), { g, fut });
            owner_->b_->CreateRetVoid();
            owner_->methodFns_[mkey] = f;
        }

        llvm::StructType* handleStructTy(const std::string& name) {
            if (llvm::StructType* t = llvm::StructType::getTypeByName(
                    owner_->module_->getContext(), "suki." + name))
                return t;
            return llvm::StructType::create(*owner_->ctx_,
                { llvm::PointerType::get(*owner_->ctx_, 0) }, "suki." + name);
        }
        llvm::Value* makeHandleValue(llvm::StructType* sty, llvm::Value* handle) {
            llvm::Value* slot = owner_->b_->CreateAlloca(sty, nullptr, "handle.tmp");
            owner_->b_->CreateStore(llvm::Constant::getNullValue(sty), slot);
            llvm::Value* hp = owner_->b_->CreateStructGEP(sty, slot, 0, "handle.addr");
            owner_->b_->CreateStore(handle, hp);
            return owner_->b_->CreateLoad(sty, slot);
        }
        llvm::Value* loadHandle(llvm::Value* selfVal, llvm::Type* selfTy) {
            llvm::Value* slot = owner_->b_->CreateAlloca(selfTy, nullptr, "self.tmp");
            owner_->b_->CreateStore(selfVal, slot);
            llvm::Value* hp = owner_->b_->CreateStructGEP(selfTy, slot, 0, "handle.addr");
            return owner_->b_->CreateLoad(llvm::PointerType::get(*owner_->ctx_, 0), hp, "handle");
        }
        // 在独立线程上启动闭包体，返回完成时完成的 Future。
        llvm::Value* startClosureTask(llvm::Value* cv, llvm::Function* cur,
                                      llvm::Type* resultTy) {
            llvm::PointerType* i8p = llvm::PointerType::get(*owner_->ctx_, 0);
            llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
            llvm::Type* voidTy = llvm::Type::getVoidTy(*owner_->ctx_);
            llvm::Value* ctxv = owner_->b_->CreateExtractValue(cv, {0}, "task.ctx");
            llvm::Value* fnv  = owner_->b_->CreateExtractValue(cv, {1}, "task.fn");
            const int64_t rsz = resultTy ? (int64_t)owner_->sizeOf(resultTy) : 0;
            llvm::Value* fut = owner_->b_->CreateCall(asyncFutureCreateFn(),
                { llvm::ConstantInt::get(i64, rsz) });

            llvm::StructType* cctxTy = llvm::StructType::get(*owner_->ctx_,
                { i8p, i8p, i8p }, false);
            llvm::FunctionType* tt = llvm::FunctionType::get(voidTy, { i8p }, false);
            llvm::Function* tramp = llvm::Function::Create(tt,
                llvm::GlobalValue::InternalLinkage,
                "suki_closure_tramp_" + std::to_string(++owner_->trampCounter_),
                owner_->module_.get());
            llvm::BasicBlock* savedBB = owner_->b_->GetInsertBlock();
            {
                owner_->b_->SetInsertPoint(llvm::BasicBlock::Create(*owner_->ctx_, "entry", tramp));
                llvm::Value* raw = tramp->getArg(0);
                llvm::Value* cp = owner_->b_->CreateBitCast(raw,
                    llvm::PointerType::getUnqual(cctxTy), "cctx");
                llvm::Value* cctx = owner_->b_->CreateLoad(i8p,
                    owner_->b_->CreateStructGEP(cctxTy, cp, 0));
                llvm::Value* cfn = owner_->b_->CreateLoad(i8p,
                    owner_->b_->CreateStructGEP(cctxTy, cp, 1));
                llvm::Value* cfut = owner_->b_->CreateLoad(i8p,
                    owner_->b_->CreateStructGEP(cctxTy, cp, 2));
                llvm::FunctionType* cfty = resultTy
                    ? llvm::FunctionType::get(resultTy, { i8p }, false)
                    : llvm::FunctionType::get(voidTy, { i8p }, false);
                llvm::Value* fp = owner_->b_->CreatePointerCast(cfn, cfty->getPointerTo());
                if (resultTy) {
                    llvm::Value* v = owner_->b_->CreateCall(cfty, fp, { cctx });
                    llvm::Value* slot = owner_->b_->CreateAlloca(resultTy, nullptr, "result");
                    owner_->b_->CreateStore(v, slot);
                    owner_->b_->CreateCall(owner_->declareExternalSig("suki_future_store",
                        { i8p, i8p }, voidTy), { cfut, owner_->b_->CreateBitCast(slot, i8p) });
                } else {
                    owner_->b_->CreateCall(cfty, fp, { cctx });
                }
                owner_->b_->CreateCall(asyncFreeFn(), { raw });
                owner_->b_->CreateRetVoid();
                owner_->b_->SetInsertPoint(savedBB);
            }

            llvm::Value* cctxPtr = owner_->b_->CreateCall(asyncAllocFn(),
                { llvm::ConstantInt::get(i64, (int64_t)owner_->sizeOf(cctxTy)) });
            llvm::Value* cp = owner_->b_->CreateBitCast(cctxPtr,
                llvm::PointerType::getUnqual(cctxTy));
            owner_->b_->CreateStore(ctxv, owner_->b_->CreateStructGEP(cctxTy, cp, 0));
            owner_->b_->CreateStore(fnv,  owner_->b_->CreateStructGEP(cctxTy, cp, 1));
            owner_->b_->CreateStore(fut,  owner_->b_->CreateStructGEP(cctxTy, cp, 2));

            llvm::Function* startFn = owner_->declareExternalSig("suki_closure_thread_start",
                { i8p, i8p, i8p }, llvm::Type::getInt32Ty(*owner_->ctx_));
            llvm::Value* st = owner_->b_->CreateCall(startFn, { tramp, cctxPtr, fut });
            llvm::Value* failed = owner_->b_->CreateICmpNE(st,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*owner_->ctx_), 0));
            llvm::BasicBlock* inlineBB = llvm::BasicBlock::Create(*owner_->ctx_, "task.inline", cur);
            llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*owner_->ctx_, "task.done", cur);
            owner_->b_->CreateCondBr(failed, inlineBB, doneBB);
            owner_->b_->SetInsertPoint(inlineBB);
            owner_->b_->CreateCall(tramp, { cctxPtr });
            owner_->b_->CreateCall(owner_->declareExternalSig("suki_future_finish",
                { i8p }, voidTy), { fut });
            owner_->b_->CreateBr(doneBB);
            owner_->b_->SetInsertPoint(doneBB);
            return fut;
        }

        llvm::Value* genTaskInit(CallExpr* e) {
            Node* clNode = e->arguments.empty() ? nullptr : e->arguments[0].get();
            if (!clNode || clNode->kind != NodeKind::ClosureExpr) return nullptr;
            llvm::Function* cur = owner_->b_->GetInsertBlock()->getParent();
            llvm::Value* cv = owner_->emitClosure(static_cast<ClosureExpr*>(clNode));
            if (!cv) return nullptr;
            llvm::Value* fut = startClosureTask(cv, cur, nullptr);
            return makeHandleValue(handleStructTy("Task"), fut);
        }

        llvm::Value* genTaskGroupInit(const Type* elemTy) {
            llvm::PointerType* i8p = llvm::PointerType::get(*owner_->ctx_, 0);
            llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
            const int64_t esz = elemTy ? (int64_t)owner_->sizeOf(owner_->layout_->lower(elemTy)) : 0;
            llvm::Function* cf = owner_->declareExternalSig("suki_taskgroup_create", { i64 }, i8p);
            return makeHandleValue(handleStructTy("TaskGroup"),
                owner_->b_->CreateCall(cf, { llvm::ConstantInt::get(i64, esz) }));
        }

        // 第三个参数 elemTy 是 TaskGroup 的子结果类型 T。addTask 的闭包返回 T，
        // 必须用它的大小去创建能装下结果的 Future（否则 resultSize=0，运行时
        // suki_future_store 因 size<=0 直接返回、result 为 NULL，for-await 读到
        // 未初始化垃圾值）。group 在标准库中登记为裸 TaskGroup（elements 为空），
        // 因此 T 由调用点从闭包返回类型推断后传进来。把它并入 cache key，避免
        // 同一 base.mem 在不同 T 下共享同一份（大小不同的）IR。
        void genConcurrencyBuiltinMethod(const std::string& base, const std::string& mem,
                                         const Type* elemTy = nullptr) {
            std::string key = base + "." + mem;
            if (elemTy) key += "/" + (elemTy->name.empty() ? std::string("?") : elemTy->name);
            if (owner_->methodFns_.count(key)) return;
            llvm::PointerType* i8p = llvm::PointerType::get(*owner_->ctx_, 0);
            llvm::Type* voidTy = llvm::Type::getVoidTy(*owner_->ctx_);
            llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
            llvm::StructType* taskSty = handleStructTy("Task");
            llvm::StructType* grpSty = handleStructTy("TaskGroup");

            if (base == "Task" && mem == "wait") {
                llvm::FunctionType* fty = llvm::FunctionType::get(voidTy, { taskSty }, false);
                llvm::Function* f = llvm::Function::Create(fty,
                    llvm::GlobalValue::InternalLinkage, "Task.wait", owner_->module_.get());
                owner_->b_->SetInsertPoint(llvm::BasicBlock::Create(*owner_->ctx_, "entry", f));
                llvm::Value* fut = loadHandle(f->getArg(0), taskSty);
                llvm::Value* tmp = owner_->b_->CreateAlloca(i64, nullptr, "wait.tmp");
                owner_->b_->CreateCall(owner_->declareExternalSig("suki_future_await",
                    { i8p, i8p }, voidTy), { fut, owner_->b_->CreateBitCast(tmp, i8p) });
                owner_->b_->CreateRetVoid();
                owner_->methodFns_["Task.wait"] = f;
                return;
            }
            if (base == "TaskGroup" && mem == "waitForAll") {
                llvm::FunctionType* fty = llvm::FunctionType::get(voidTy, { grpSty }, false);
                llvm::Function* f = llvm::Function::Create(fty,
                    llvm::GlobalValue::InternalLinkage, "TaskGroup.waitForAll",
                    owner_->module_.get());
                owner_->b_->SetInsertPoint(llvm::BasicBlock::Create(*owner_->ctx_, "entry", f));
                llvm::Value* g = loadHandle(f->getArg(0), grpSty);
                owner_->b_->CreateCall(owner_->declareExternalSig("suki_taskgroup_wait_all",
                    { i8p }, voidTy), { g });
                owner_->b_->CreateRetVoid();
                owner_->methodFns_["TaskGroup.waitForAll"] = f;
                return;
            }
            if (base == "TaskGroup" && mem == "addTask") {
                llvm::StructType* clTy = llvm::cast<llvm::StructType>(
                    owner_->layout_->closureTy(llvm::FunctionType::get(voidTy, { i8p }, false)));
                llvm::FunctionType* fty = llvm::FunctionType::get(voidTy, { grpSty, clTy }, false);
                llvm::Function* f = llvm::Function::Create(fty,
                    llvm::GlobalValue::InternalLinkage, "TaskGroup.addTask",
                    owner_->module_.get());
                owner_->b_->SetInsertPoint(llvm::BasicBlock::Create(*owner_->ctx_, "entry", f));
                llvm::Value* g = loadHandle(f->getArg(0), grpSty);
                // 子结果类型 T 的大小：nullptr 等价于 void（不存结果）。
                llvm::Type* elemLLVM = elemTy ? owner_->layout_->lower(elemTy) : nullptr;
                llvm::Value* fut = startClosureTask(f->getArg(1), f, elemLLVM);
                owner_->b_->CreateCall(owner_->declareExternalSig("suki_taskgroup_add",
                    { i8p, i8p }, voidTy), { g, fut });
                owner_->b_->CreateRetVoid();
                owner_->methodFns_[key] = f;
                return;
            }
        }

        void genSleepSpawn() {
            if (owner_->fns_.count("sleep_spawn")) return;
            llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
            llvm::Type* voidTy = llvm::Type::getVoidTy(*owner_->ctx_);
            llvm::PointerType* i8p = llvm::PointerType::get(*owner_->ctx_, 0);

            llvm::Function* sleepFn = owner_->declareExternalSig("suki_sleep", { i64 }, voidTy);
            llvm::StructType* ctxTy = llvm::StructType::get(*owner_->ctx_, { i64, i8p }, false);
            llvm::Function* finishFn = owner_->declareExternalSig("suki_future_finish",
                { i8p }, voidTy);

            llvm::FunctionType* trampTy = llvm::FunctionType::get(i8p, { i8p }, false);
            llvm::Function* tramp = llvm::Function::Create(trampTy,
                llvm::GlobalValue::InternalLinkage, "sleep_tramp", owner_->module_.get());
            {
                llvm::BasicBlock* tb = llvm::BasicBlock::Create(*owner_->ctx_, "entry", tramp);
                owner_->b_->SetInsertPoint(tb);
                llvm::Value* rawCtx = tramp->getArg(0);
                llvm::Value* cp = owner_->b_->CreateBitCast(rawCtx,
                    llvm::PointerType::getUnqual(ctxTy), "ctx");
                llvm::Value* msAddr = owner_->b_->CreateStructGEP(ctxTy, cp, 0, "ms.addr");
                llvm::Value* ms = owner_->b_->CreateLoad(i64, msAddr, "ms");
                llvm::Value* futAddr = owner_->b_->CreateStructGEP(ctxTy, cp, 1, "fut.addr");
                llvm::Value* fut = owner_->b_->CreateLoad(i8p, futAddr, "fut");
                owner_->b_->CreateCall(sleepFn, { ms });
                owner_->b_->CreateCall(finishFn, { fut });
                owner_->b_->CreateCall(asyncFreeFn(), { rawCtx });
                owner_->b_->CreateRet(llvm::ConstantPointerNull::get(i8p));
            }

            llvm::FunctionType* sft = llvm::FunctionType::get(i8p, { i64 }, false);
            llvm::Function* f = llvm::Function::Create(sft,
                llvm::GlobalValue::InternalLinkage, "sleep_spawn", owner_->module_.get());
            {
                llvm::BasicBlock* sb = llvm::BasicBlock::Create(*owner_->ctx_, "entry", f);
                owner_->b_->SetInsertPoint(sb);
                llvm::Value* fut = owner_->b_->CreateCall(asyncFutureCreateFn(),
                    { llvm::ConstantInt::get(i64, 0) });
                llvm::Value* rawCtx = owner_->b_->CreateCall(asyncAllocFn(),
                    { llvm::ConstantInt::get(i64, (int64_t)owner_->sizeOf(ctxTy)) });
                llvm::Value* cp = owner_->b_->CreateBitCast(rawCtx,
                    llvm::PointerType::getUnqual(ctxTy), "ctx");
                llvm::Value* msAddr = owner_->b_->CreateStructGEP(ctxTy, cp, 0, "ms.addr");
                owner_->b_->CreateStore(f->getArg(0), msAddr);
                llvm::Value* futAddr = owner_->b_->CreateStructGEP(ctxTy, cp, 1, "fut.addr");
                owner_->b_->CreateStore(fut, futAddr);

                llvm::Function* startFn = owner_->declareExternalSig("suki_thread_start",
                    { llvm::PointerType::getUnqual(trampTy), i8p },
                    llvm::Type::getInt32Ty(*owner_->ctx_));
                llvm::Value* started = owner_->b_->CreateCall(startFn, { tramp, rawCtx }, "started");
                llvm::Value* failed = owner_->b_->CreateICmpNE(started,
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(*owner_->ctx_), 0));
                llvm::BasicBlock* inlineBB = llvm::BasicBlock::Create(*owner_->ctx_, "inline", f);
                llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*owner_->ctx_, "spawned", f);
                owner_->b_->CreateCondBr(failed, inlineBB, doneBB);
                owner_->b_->SetInsertPoint(inlineBB);
                owner_->b_->CreateCall(tramp, { rawCtx });
                owner_->b_->CreateBr(doneBB);
                owner_->b_->SetInsertPoint(doneBB);
                owner_->b_->CreateRet(fut);
            }
            owner_->fns_["sleep_spawn"] = f;
        }

        void genWithTaskGroupSpawn() {
            if (owner_->fns_.count("withTaskGroup_spawn")) return;
            llvm::PointerType* i8p = llvm::PointerType::get(*owner_->ctx_, 0);
            llvm::Type* voidTy = llvm::Type::getVoidTy(*owner_->ctx_);
            llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
            llvm::StructType* grpSty = handleStructTy("TaskGroup");
            llvm::StructType* clTy = llvm::cast<llvm::StructType>(
                owner_->layout_->closureTy(llvm::FunctionType::get(voidTy, { i8p }, false)));

            llvm::FunctionType* bodyTy = llvm::FunctionType::get(voidTy, { clTy }, false);
            llvm::Function* body = llvm::Function::Create(bodyTy,
                llvm::GlobalValue::InternalLinkage, "withTaskGroup", owner_->module_.get());
            {
                owner_->b_->SetInsertPoint(llvm::BasicBlock::Create(*owner_->ctx_, "entry", body));
                llvm::Value* cv = body->getArg(0);
                llvm::Value* g = owner_->b_->CreateCall(
                    owner_->declareExternalSig("suki_taskgroup_create", { i64 }, i8p),
                    { llvm::ConstantInt::get(i64, 0) });
                llvm::Value* gv = makeHandleValue(grpSty, g);
                llvm::Value* ctxv = owner_->b_->CreateExtractValue(cv, {0}, "body.ctx");
                llvm::Value* fnv = owner_->b_->CreateExtractValue(cv, {1}, "body.fn");
                llvm::FunctionType* cfty = llvm::FunctionType::get(voidTy, { i8p, grpSty }, false);
                llvm::Value* fp = owner_->b_->CreatePointerCast(fnv, cfty->getPointerTo());
                owner_->b_->CreateCall(cfty, fp, { ctxv, gv });
                owner_->b_->CreateCall(owner_->declareExternalSig("suki_taskgroup_wait_all",
                    { i8p }, voidTy), { g });
                owner_->b_->CreateCall(owner_->declareExternalSig("suki_taskgroup_free",
                    { i8p }, voidTy), { g });
                owner_->b_->CreateRetVoid();
            }
            owner_->fns_["withTaskGroup"] = body;

            llvm::StructType* ctxTy = llvm::StructType::get(*owner_->ctx_, { clTy, i8p }, false);
            llvm::FunctionType* trampTy = llvm::FunctionType::get(i8p, { i8p }, false);
            llvm::Function* tramp = llvm::Function::Create(trampTy,
                llvm::GlobalValue::InternalLinkage, "withTaskGroup_tramp", owner_->module_.get());
            {
                owner_->b_->SetInsertPoint(llvm::BasicBlock::Create(*owner_->ctx_, "entry", tramp));
                llvm::Value* raw = tramp->getArg(0);
                llvm::Value* cp = owner_->b_->CreateBitCast(raw,
                    llvm::PointerType::getUnqual(ctxTy), "ctx");
                llvm::Value* cvAddr = owner_->b_->CreateStructGEP(ctxTy, cp, 0, "cl.addr");
                llvm::Value* cv = owner_->b_->CreateLoad(clTy, cvAddr);
                llvm::Value* futAddr = owner_->b_->CreateStructGEP(ctxTy, cp, 1, "fut.addr");
                llvm::Value* fut = owner_->b_->CreateLoad(i8p, futAddr, "fut");
                owner_->b_->CreateCall(body, { cv });
                owner_->b_->CreateCall(owner_->declareExternalSig("suki_future_finish",
                    { i8p }, voidTy), { fut });
                owner_->b_->CreateCall(asyncFreeFn(), { raw });
                owner_->b_->CreateRet(llvm::ConstantPointerNull::get(i8p));
            }

            llvm::FunctionType* sft = llvm::FunctionType::get(i8p, { clTy }, false);
            llvm::Function* sp = llvm::Function::Create(sft,
                llvm::GlobalValue::InternalLinkage, "withTaskGroup_spawn", owner_->module_.get());
            {
                owner_->b_->SetInsertPoint(llvm::BasicBlock::Create(*owner_->ctx_, "entry", sp));
                llvm::Value* fut = owner_->b_->CreateCall(asyncFutureCreateFn(),
                    { llvm::ConstantInt::get(i64, 0) });
                llvm::Value* raw = owner_->b_->CreateCall(asyncAllocFn(),
                    { llvm::ConstantInt::get(i64, (int64_t)owner_->sizeOf(ctxTy)) });
                llvm::Value* cp = owner_->b_->CreateBitCast(raw,
                    llvm::PointerType::getUnqual(ctxTy), "ctx");
                llvm::Value* cvAddr = owner_->b_->CreateStructGEP(ctxTy, cp, 0, "cl.addr");
                owner_->b_->CreateStore(sp->getArg(0), cvAddr);
                llvm::Value* futAddr = owner_->b_->CreateStructGEP(ctxTy, cp, 1, "fut.addr");
                owner_->b_->CreateStore(fut, futAddr);
                llvm::Function* startFn = owner_->declareExternalSig("suki_thread_start",
                    { llvm::PointerType::getUnqual(trampTy), i8p },
                    llvm::Type::getInt32Ty(*owner_->ctx_));
                llvm::Value* st = owner_->b_->CreateCall(startFn, { tramp, raw }, "started");
                llvm::Value* failed = owner_->b_->CreateICmpNE(st,
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(*owner_->ctx_), 0));
                llvm::BasicBlock* ib = llvm::BasicBlock::Create(*owner_->ctx_, "inline", sp);
                llvm::BasicBlock* db = llvm::BasicBlock::Create(*owner_->ctx_, "spawned", sp);
                owner_->b_->CreateCondBr(failed, ib, db);
                owner_->b_->SetInsertPoint(ib);
                owner_->b_->CreateCall(tramp, { raw });
                owner_->b_->CreateBr(db);
                owner_->b_->SetInsertPoint(db);
                owner_->b_->CreateRet(fut);
            }
            owner_->fns_["withTaskGroup_spawn"] = sp;
        }
    };

    // ── ARC 处理组件 ───────────────────────────────────────────────────────────
    // 引用计数遵循分析器保证的所有权规则：新建对象已持有一份引用（计数为 1），
    // 因此「共享」已有引用要 retain，「覆写」某位置要释放它此前持有的值。所有
    // 运行时入口内部使用 acquire/release 序，绝不使用 relaxed。
    class ARCPass {
    public:
        explicit ARCPass(Impl* o) : owner_(o) {}
        Impl* owner_;

        llvm::Function* arcRetain() {
            llvm::Type* p = llvm::PointerType::get(*owner_->ctx_, 0);
            return owner_->declareExternalSig("suki_arc_retain", { p },
                                              llvm::Type::getInt64Ty(*owner_->ctx_));
        }
        llvm::Function* arcRelease() {
            llvm::Type* p = llvm::PointerType::get(*owner_->ctx_, 0);
            return owner_->declareExternalSig("suki_arc_release", { p },
                                              llvm::Type::getInt64Ty(*owner_->ctx_));
        }
        // 只对受管引用 retain/release；其余类型一律 no-op，调用方可放心传任意值。
        void arcRetainIfRef(llvm::Value* v, const Type* t) {
            if (!v || !t || !owner_->layout_->isReferenceType(t)) return;
            owner_->b_->CreateCall(arcRetain(), { v });
        }
        void arcReleaseIfRef(llvm::Value* v, const Type* t) {
            if (!v || !t || !owner_->layout_->isReferenceType(t)) return;
            owner_->b_->CreateCall(arcRelease(), { v });
        }
    };

    // ── 异常 lowering 组件 ───────────────────────────────────────────────────────
    // 错误模型：调用约定追加一个 `i8**` 错误槽；throw 把错误写入堆单元格并跳转到
    // 最近 catch 分发器或返回传播；do/catch 用独立分发块按声明顺序匹配各子句。
    class ExceptionLowerer {
    public:
        explicit ExceptionLowerer(Impl* o) : owner_(o) {}
        Impl* owner_;

        // 抛出调用应写入的错误槽。
        llvm::Value* errorSlotForCall() {
            llvm::PointerType* i8p = llvm::PointerType::get(*owner_->ctx_, 0);
            if (owner_->optionalTrySlot_) return owner_->optionalTrySlot_;
            if (owner_->currentErrorSlot_) return owner_->currentErrorSlot_;
            llvm::Value* tmp = owner_->b_->CreateAlloca(i8p, nullptr, "err.tmp");
            owner_->b_->CreateStore(llvm::ConstantPointerNull::get(i8p), tmp);
            return tmp;
        }

        // 抛出调用之后：跳转到就近 handler，或在错误传播时返回。
        void finishThrowingCall(llvm::Value* slot) {
            if (!slot) return;
            llvm::PointerType* i8p = llvm::PointerType::get(*owner_->ctx_, 0);
            if (owner_->optionalTrySlot_) return;
            llvm::Function* f = owner_->b_->GetInsertBlock()->getParent();
            llvm::Value* err = owner_->b_->CreateLoad(i8p, slot, "err");
            llvm::Value* has = owner_->b_->CreateIsNotNull(err, "hasError");
            llvm::BasicBlock* land = llvm::BasicBlock::Create(*owner_->ctx_, "try.fail", f);
            llvm::BasicBlock* cont = llvm::BasicBlock::Create(*owner_->ctx_, "try.cont", f);
            owner_->b_->CreateCondBr(has, land, cont);
            owner_->b_->SetInsertPoint(land);
            if (!owner_->catchTargets_.empty()) {
                owner_->b_->CreateBr(owner_->catchTargets_.back());
            } else {
                if (owner_->currentErrorSlot_) owner_->b_->CreateStore(err, owner_->currentErrorSlot_);
                if (owner_->currentRet_ && !owner_->currentRet_->isVoidTy())
                    owner_->b_->CreateRet(llvm::Constant::getNullValue(owner_->currentRet_));
                else
                    owner_->b_->CreateRetVoid();
            }
            owner_->b_->SetInsertPoint(cont);
        }

        // `throw e`：构造错误单元、记录并离开函数（或跳到就近 catch）。
        llvm::Value* genThrow(ThrowStmt* t) {
            llvm::PointerType* i8p = llvm::PointerType::get(*owner_->ctx_, 0);
            llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
            llvm::Value* v = t->value ? owner_->genExpr(t->value.get()) : nullptr;
            llvm::Function* alloc = owner_->declareExternalSig("suki_alloc", { i64 }, i8p);
            llvm::Value* cell = owner_->b_->CreateCall(alloc, { llvm::ConstantInt::get(i64, 24) });
            llvm::Value* typed = owner_->b_->CreatePointerCast(cell, owner_->errorTy()->getPointerTo());
            const std::string typeName =
                (t->value && t->value->semaType && t->value->semaType->record)
                    ? t->value->semaType->record->name
                    : std::string("Error");
            owner_->b_->CreateStore(owner_->b_->CreateGlobalStringPtr(typeName),
                                    owner_->b_->CreateStructGEP(owner_->errorTy(), typed, 0));
            llvm::Value* tag = llvm::ConstantInt::get(i64, 0);
            llvm::Value* payload = llvm::ConstantPointerNull::get(i8p);
            if (v && owner_->isTaggedUnion(v->getType())) {
                tag = owner_->b_->CreateExtractValue(v, {0});
                payload = owner_->b_->CreateExtractValue(v, {1});
            } else if (v && v->getType()->isIntegerTy(64)) {
                tag = v;
            }
            owner_->b_->CreateStore(tag, owner_->b_->CreateStructGEP(owner_->errorTy(), typed, 1));
            owner_->b_->CreateStore(payload, owner_->b_->CreateStructGEP(owner_->errorTy(), typed, 2));
            llvm::Value* err = owner_->b_->CreatePointerCast(typed, i8p);
            if (!owner_->catchTargets_.empty()) {
                owner_->b_->CreateStore(err, owner_->currentErrorSlot_);
                owner_->b_->CreateBr(owner_->catchTargets_.back());
            } else if (owner_->currentErrorSlot_) {
                owner_->b_->CreateStore(err, owner_->currentErrorSlot_);
                if (owner_->currentRet_ && !owner_->currentRet_->isVoidTy())
                    owner_->b_->CreateRet(llvm::Constant::getNullValue(owner_->currentRet_));
                else
                    owner_->b_->CreateRetVoid();
            } else {
                owner_->b_->CreateCall(owner_->declareExternalSig("panic", { i8p }),
                                       { owner_->b_->CreateGlobalStringPtr(
                                           "error thrown from a non-throwing context") });
                if (owner_->currentRet_ && !owner_->currentRet_->isVoidTy())
                    owner_->b_->CreateRet(llvm::Constant::getNullValue(owner_->currentRet_));
                else
                    owner_->b_->CreateRetVoid();
            }
            return nullptr;
        }

        // `do { … } catch { … }`：body 运行于全新错误槽；任何错误分支到分发器，
        // 分发器按声明顺序遍历 catch 子句。
        void genDoStmt(DoStmt* d) {
            llvm::PointerType* i8p = llvm::PointerType::get(*owner_->ctx_, 0);
            llvm::Function* f = owner_->b_->GetInsertBlock()->getParent();
            llvm::Value* slot = owner_->b_->CreateAlloca(i8p, nullptr, "err.slot");
            owner_->b_->CreateStore(llvm::ConstantPointerNull::get(i8p), slot);
            llvm::BasicBlock* dispatch = llvm::BasicBlock::Create(*owner_->ctx_, "catch.dispatch", f);
            llvm::BasicBlock* done = llvm::BasicBlock::Create(*owner_->ctx_, "do.cont", f);
            llvm::Value* savedSlot = owner_->currentErrorSlot_;
            owner_->currentErrorSlot_ = slot;
            owner_->catchTargets_.push_back(dispatch);
            for (auto& st : d->body) owner_->genStmt(st.get());
            owner_->catchTargets_.pop_back();
            owner_->currentErrorSlot_ = savedSlot;
            if (!owner_->b_->GetInsertBlock()->getTerminator()) owner_->b_->CreateBr(done);
            owner_->b_->SetInsertPoint(dispatch);
            llvm::Value* err = owner_->b_->CreateLoad(i8p, slot, "err");
            for (auto& c : d->catches) {
                auto* cc = static_cast<CatchClause*>(c.get());
                llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*owner_->ctx_, "catch.body", f);
                llvm::BasicBlock* nextBB = llvm::BasicBlock::Create(*owner_->ctx_, "catch.next", f);
                bool unconditional = true;
                if (cc->pattern && cc->pattern->kind == NodeKind::VarDecl) {
                    auto* vd = static_cast<VarDecl*>(cc->pattern.get());
                    std::string want = owner_->typeReprName(vd->type.get());
                    if (!want.empty()) {
                        llvm::Value* typed = owner_->b_->CreatePointerCast(err, owner_->errorTy()->getPointerTo());
                        llvm::Value* tn = owner_->b_->CreateLoad(
                            i8p, owner_->b_->CreateStructGEP(owner_->errorTy(), typed, 0), "err.type");
                        llvm::Function* cmp = owner_->declareExternalSig(
                            "strcmp", { i8p, i8p }, llvm::Type::getInt32Ty(*owner_->ctx_));
                        llvm::Value* eq = owner_->b_->CreateICmpEQ(
                            owner_->b_->CreateCall(cmp, { tn, owner_->b_->CreateGlobalStringPtr(want) }),
                            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*owner_->ctx_), 0));
                        owner_->b_->CreateCondBr(eq, bodyBB, nextBB);
                        unconditional = false;
                    }
                }
                if (unconditional) owner_->b_->CreateBr(bodyBB);
                owner_->b_->SetInsertPoint(bodyBB);
                if (cc->pattern && cc->pattern->kind == NodeKind::VarDecl) {
                    auto* vd = static_cast<VarDecl*>(cc->pattern.get());
                    if (!vd->name.empty()) {
                        llvm::Value* bs = owner_->b_->CreateAlloca(i8p, nullptr, vd->name);
                        owner_->b_->CreateStore(err, bs);
                        owner_->locals_[vd->name] = bs;
                    }
                }
                if (cc->whereExpr) owner_->genExpr(cc->whereExpr.get());
                for (auto& st : cc->body) owner_->genStmt(st.get());
                if (!owner_->b_->GetInsertBlock()->getTerminator()) owner_->b_->CreateBr(done);
                owner_->b_->SetInsertPoint(nextBB);
            }
            if (owner_->currentErrorSlot_) owner_->b_->CreateStore(err, owner_->currentErrorSlot_);
            owner_->b_->CreateBr(done);
            owner_->b_->SetInsertPoint(done);
        }
    };

    // ── 类型注册组件 ───────────────────────────────────────────────────────────
    // 负责把 Suki 的类型体系落到 LLVM：名字→基础类型、声明/登记函数、对外签名、
    // 语义类型下沉、声明类型下沉、以及强制转换。类型缓存（kindCache_/typeCache_）
    // 仍驻留 Impl，组件经 owner_-> 访问，避免跨组件状态归属混乱。
    class TypeRegistry {
    public:
        explicit TypeRegistry(Impl* o) : owner_(o) {}
        Impl* owner_;

        llvm::Type* lowerSema(const Type* t) {
            if (!t || !owner_->layout_) return nullptr;
            return owner_->layout_->lower(t);
        }
        // Conservative gate: only scalars take the semaType route for now.
        bool semaIsScalar(const Type* t) {
            return t && owner_->layout_ && !owner_->layout_->isAggregate(t) &&
                   !owner_->layout_->isReferenceType(t);
        }
        llvm::Type* tyByName(const std::string& n) {
            auto& C = *owner_->ctx_;
            if (n.empty() || n == "Void") return llvm::Type::getVoidTy(C);
            if (n == "Bool") return llvm::Type::getInt1Ty(C);
            if (n == "Int8" || n == "UInt8") return llvm::Type::getInt8Ty(C);
            if (n == "Int16" || n == "UInt16") return llvm::Type::getInt16Ty(C);
            if (n == "Int32" || n == "UInt32") return llvm::Type::getInt32Ty(C);
            if (n == "Int64" || n == "UInt64" || n == "Int" || n == "UInt" ||
                n == "ISize" || n == "USize")
                return llvm::Type::getInt64Ty(C);
            if (n == "Float" || n == "Float32") return llvm::Type::getFloatTy(C);
            if (n == "Double" || n == "Float64") return llvm::Type::getDoubleTy(C);
            // String/Char are i8* in the default address space (0).
            if (n == "String" || n == "Char") return llvm::PointerType::get(C, 0);
            return llvm::Type::getInt64Ty(C);
        }

        // Declare (or reuse) the LLVM function for a Suki FunctionDecl under a
        // specific symbol name. Shared by `declare()` and the monomorphiser.
        llvm::Function* declareAs(FunctionDecl* fn, const std::string& symName) {
            auto it = owner_->fns_.find(symName);
            if (it != owner_->fns_.end()) return it->second;
            bool isMain = (symName == "main") || hasAttr(fn, "main");
            // `main` is the C entry point: it cannot carry the hidden error slot.
            const bool throws = fn->isThrows && !isMain;
            std::vector<llvm::Type*> params;
            if (!isMain)
                for (auto& p : fn->params)
                    params.push_back(lowerDeclType(p.semaType, p.type.get()));
            if (throws) params.push_back(llvm::PointerType::get(*owner_->ctx_, 0));
            // Opaque return types (spec 5.5): only when the return type is
            // `some P` do we prefer the Sema-inferred concrete type; generics
            // keep using fn->returnType->semaType to avoid signature mismatch.
            const Type* rt =
                (fn->returnType && fn->returnType->kind == NodeKind::OptionalType &&
                 static_cast<OptionalType*>(fn->returnType.get())->isOpaque &&
                 fn->semaType && fn->semaType->kind == TypeKind::Function && fn->semaType->ret)
                    ? fn->semaType->ret
                    : (fn->returnType ? fn->returnType->semaType : nullptr);
            llvm::Type* ret = isMain
                ? llvm::Type::getInt32Ty(*owner_->ctx_)
                : (rt ? lowerDeclType(rt, fn->returnType.get())
                       : llvm::Type::getVoidTy(*owner_->ctx_));
            owner_->fnThrows_[symName] = throws;
            llvm::Function* f = llvm::Function::Create(
                llvm::FunctionType::get(ret, params, false),
                llvm::GlobalValue::ExternalLinkage, symName,
                owner_->module_.get());
            // @_cdecl("name"): foreign functions use the given C symbol name.
            if (fn->isForeign && !fn->cdeclName.empty()) f->setName(fn->cdeclName);
            // @convention(c|stdcall)（规范 §6.3）：为声明（含 foreign）函数设置
            // 匹配的 LLVM calling convention。显式声明的约定（c/stdcall）优先；未
            // 声明时 foreign 函数默认采用 C 约定。stdcall 在 32 位 x86 / Windows
            // 上对应 X86_StdCall，其它目标上由后端按默认 ABI 处理。
            if (fn->convention == "stdcall")
                f->setCallingConv(llvm::CallingConv::X86_StdCall);
            else
                f->setCallingConv(llvm::CallingConv::C);
            owner_->fns_[symName] = f;
            owner_->fnDecls_[fn->name] = fn;
            owner_->pendingBodies_.emplace_back(fn, symName);
            owner_->isMainFns_[symName] = isMain;
            return f;
        }

        llvm::Function* declare(FunctionDecl* fn) {
            return declareAs(fn, fn->name);
        }

        // Declare (or reuse) an external function with an explicit signature.
        llvm::Function* declareExternalSig(const std::string& name,
                                           const std::vector<llvm::Type*>& params,
                                           llvm::Type* ret = nullptr) {
            auto it = owner_->fns_.find(name);
            if (it != owner_->fns_.end()) return it->second;
            llvm::Function* f = llvm::Function::Create(
                llvm::FunctionType::get(
                    ret ? ret : llvm::Type::getVoidTy(*owner_->ctx_),
                    params, false),
                llvm::GlobalValue::ExternalLinkage, name,
                owner_->module_.get());
            owner_->fns_[name] = f;
            return f;
        }

        // Lower a type for a declaration position: prefer the Sema-annotated
        // type, fall back to the syntactic name when Sema did not annotate.
        llvm::Type* lowerDeclType(const Type* sema, Node* typeRepr) {
            if (sema) return owner_->layout_->lower(sema);
            return tyByName(typeReprName(typeRepr));
        }

        // Explicit numeric/string conversion (`Int(x)`, `Double(n)`,
        // `Char("A")`). Unlike the implicit `coerce`, a cast to a narrower
        // integer truncates and a cast to `String` goes through the runtime.
        llvm::Value* coerceForCast(llvm::Value* v, llvm::Type* target,
                                   const Type* to) {
            if (!v || !target) return v;
            llvm::Type* from = v->getType();
            if (from == target) return v;
            if (target->isIntegerTy() && from->isIntegerTy()) {
                unsigned fb = from->getIntegerBitWidth(),
                         tb = target->getIntegerBitWidth();
                // Char is a Unicode scalar (i32) and must not be sign-extended
                // through a signed 8/16-bit detour; widen from the source.
                return fb < tb ? owner_->b_->CreateZExt(v, target)
                               : owner_->b_->CreateTrunc(v, target);
            }
            if (target->isFloatingPointTy() && from->isIntegerTy())
                return owner_->b_->CreateSIToFP(v, target);
            if (target->isIntegerTy() && from->isFloatingPointTy())
                return owner_->b_->CreateFPToSI(v, target);
            // `Int("42")` / `Double("1.5")`: parse the String through runtime.
            if ((target->isIntegerTy() || target->isFloatingPointTy()) &&
                from->isStructTy()) {
                llvm::Type* sty = owner_->layout_->stringTy();
                if (from == sty) {
                    bool wantFloat = target->isFloatingPointTy();
                    llvm::Function* f = declareExternalSig(
                        wantFloat ? "suki_str_to_double" : "suki_str_to_int",
                        { sty },
                        wantFloat ? llvm::Type::getDoubleTy(*owner_->ctx_)
                                  : llvm::Type::getInt64Ty(*owner_->ctx_));
                    llvm::Value* r = owner_->b_->CreateCall(f, { v });
                    return coerce(r, target);
                }
            }
            // `String(x)` from a scalar: format through the runtime.
            if (target->isStructTy() && from->isIntegerTy()) {
                bool fromChar = from->getIntegerBitWidth() == 32;
                llvm::Function* f = declareExternalSig(
                    fromChar ? "suki_char_to_string" : "suki_int_to_string",
                    { llvm::Type::getInt64Ty(*owner_->ctx_) },
                    owner_->layout_->stringTy());
                return owner_->b_->CreateCall(
                    f, { coerce(v, llvm::Type::getInt64Ty(*owner_->ctx_)) });
            }
            return coerce(v, target);
        }

        // Implicit value coercion (widening, int<->float, and boxing into a
        // pointer-typed slot).
        llvm::Value* coerce(llvm::Value* v, llvm::Type* to) {
            if (!v || !to) return v;
            llvm::Type* from = v->getType();
            if (from == to) return v;
            if (to->isIntegerTy() && from->isIntegerTy()) {
                unsigned fb = from->getIntegerBitWidth(),
                         tb = to->getIntegerBitWidth();
                if (fb < tb) return owner_->b_->CreateSExt(v, to);
                if (fb > tb) return owner_->b_->CreateTrunc(v, to);
                return v;
            }
            if (to->isFloatingPointTy() && from->isIntegerTy())
                return owner_->b_->CreateSIToFP(v, to);
            if (to->isIntegerTy() && from->isFloatingPointTy())
                return owner_->b_->CreateFPToSI(v, to);
            if (to->isFloatingPointTy() && from->isFloatingPointTy())
                return owner_->b_->CreateFPExt(v, to);
            // Boxing: a value landing in a pointer slot is copied to the heap
            // and its address is returned (Optional payload for aggregates).
            if (to->isPointerTy() && !from->isPointerTy()) {
                llvm::Value* slot = owner_->b_->CreateAlloca(from, nullptr, "box");
                owner_->b_->CreateStore(v, slot);
                return owner_->b_->CreatePointerCast(slot, to);
            }
            return v;
        }
    };

    // 表达式/声明编排组件：genStmt / genExpr / genVarDecl 的承载。
    class ExprGen {
    public:
        explicit ExprGen(Impl* o) : owner_(o) {}
        Impl* owner_;

        // ── 类型化非托管指针特化（规范 §6.5 / §8.1）────────────────────────────
        // 取 base 变量地址 → GEP 到 raw 字段（索引 0）→ load int64 地址位。
        llvm::Value* genUnsafeRawAddr(const Type* baseTy, Node* baseNode) {
            llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
            llvm::Value* baseAddr = owner_->genAddr(baseNode);
            if (!baseAddr) return nullptr;
            llvm::Type* sty = owner_->layout_->lower(baseTy);
            // 结构体字段 GEP 必须使用 i32 字段索引（CreateStructGEP 已处理）。
            llvm::Value* gep = owner_->b_->CreateStructGEP(sty, baseAddr, 0, "raw.addr");
            return owner_->b_->CreateLoad(i64, gep, "rawaddr");
        }
        // 计算元素地址：inttoptr(raw + idx*stride, elemTy*)。idx 为空时即 pointee。
        llvm::Value* genUnsafeElemPtr(const Type* baseTy, Node* baseNode,
                                      const Type* elemTy, llvm::Value* idx) {
            llvm::Value* raw = genUnsafeRawAddr(baseTy, baseNode);
            if (!raw) return nullptr;
            llvm::Type* et = owner_->layout_->lower(elemTy);
            llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
            if (idx) {
                uint64_t stride = owner_->sizeOf(et);
                llvm::Value* off = owner_->b_->CreateMul(
                    idx, llvm::ConstantInt::get(i64, (uint64_t)stride), "offset");
                raw = owner_->b_->CreateAdd(raw, off, "addr");
            }
            return owner_->b_->CreateIntToPtr(
                raw, llvm::PointerType::getUnqual(et), "elemptr");
        }

        // 变量声明（含元组解构）的生成。
        void genVarDecl(VarDecl* vd) {
            // 元组解构 `let (a, b) = (1, 2)`：为每个名字建槽，逐分量 extract。
            if (!vd->tupleNames.empty()) {
                llvm::Value* init = vd->initializer ? owner_->genExpr(vd->initializer.get())
                                                    : nullptr;
                llvm::Type* ity = init ? init->getType()
                                       : llvm::Type::getInt64Ty(*owner_->ctx_);
                auto* ist = llvm::dyn_cast<llvm::StructType>(ity);
                if (!ist) return;
                for (size_t i = 0; i < vd->tupleNames.size(); ++i) {
                    unsigned idx = static_cast<unsigned>(i);
                    if (idx >= ist->getNumElements()) break;
                    llvm::Type* et = ist->getElementType(idx);
                    llvm::Value* slot =
                        owner_->b_->CreateAlloca(et, nullptr, vd->tupleNames[i]);
                    owner_->b_->CreateStore(
                        owner_->b_->CreateExtractValue(init, {idx}), slot);
                    owner_->locals_[vd->tupleNames[i]] = slot;
                }
                return;
            }
            llvm::Value* init = vd->initializer ? owner_->genExpr(vd->initializer.get())
                                                : nullptr;
            // Prefer the analyser-resolved type; fall back to syntactic inference.
            llvm::Type* ty = nullptr;
            const Type* st = vd->semaType;
            // The analyser-resolved type is authoritative for every type.
            if (st && owner_->layout_) ty = owner_->layout_->lower(st);
            if (!ty) {
                std::string tname = typeReprName(vd->type.get());
                ty = tname.empty()
                         ? (init ? init->getType()
                                 : llvm::Type::getInt64Ty(*owner_->ctx_))
                         : owner_->tyByName(tname);
            }
            if (init) init = owner_->coerce(init, ty);
            // A freshly constructed object already starts at count 1, so it must
            // not be retained again; binding an existing reference must.
            bool needsRetain = false;
            if (init && vd->semaType &&
                owner_->layout_->isReferenceType(vd->semaType) && vd->initializer) {
                Node* src = vd->initializer.get();
                bool isConstruction = false;
                if (src->kind == NodeKind::CallExpr) {
                    auto* ce = static_cast<CallExpr*>(src);
                    if (ce->callee && ce->callee->kind == NodeKind::IdentExpr)
                        isConstruction =
                            owner_->classTypes_.count(
                                static_cast<IdentExpr*>(ce->callee.get())->name) > 0;
                }
                needsRetain = !isConstruction;
            }
            if (needsRetain) owner_->arcRetainIfRef(init, vd->semaType);
            llvm::Value* slot = owner_->b_->CreateAlloca(ty, nullptr, vd->name);
            if (init) owner_->b_->CreateStore(init, slot);
            owner_->locals_[vd->name] = slot;
            // A strong reference held by this local must be given back at scope exit.
            owner_->trackScopedRef(vd->name, vd->semaType);
        }

        // 语句编排：分发到各语句节点的代码生成路径。
        void genStmt(Node* s) {
            if (!s) return;
            switch (s->kind) {
                case NodeKind::VarDecl: owner_->genVarDecl(static_cast<VarDecl*>(s)); break;
                case NodeKind::BlockStmt: {
                    // A nested block ends the lifetime of anything it declared.
                    owner_->scopeMarks_.push_back(owner_->scopeRefs_.size());
                    for (auto& st : static_cast<BlockStmt*>(s)->statements)
                        owner_->genStmt(st.get());
                    owner_->scopeMarks_.push_back(owner_->scopeRefs_.size());
                    owner_->releaseScopeTo(owner_->scopeMarks_.back());
                    owner_->scopeMarks_.pop_back();
                    owner_->scopeMarks_.pop_back();
                    break;
                }
                    break;
                case NodeKind::UnsafeStmt: {
                    // `unsafe` 仅影响语义检查（规范 8.6），codegen 直接生成块内语句。
                    auto* us = static_cast<UnsafeStmt*>(s);
                    for (auto& st : us->body) owner_->genStmt(st.get());
                    break;
                }
                case NodeKind::ExprStmt: owner_->genExpr(static_cast<ExprStmt*>(s)->expr.get()); break;
                case NodeKind::ThrowStmt: owner_->genThrow(static_cast<ThrowStmt*>(s)); return;
                case NodeKind::DoStmt: owner_->genDoStmt(static_cast<DoStmt*>(s)); return;
                case NodeKind::BreakStmt: {
                    // `break` 跳出最内层循环/switch；带标签时跳到对应循环出口。
                    auto* b = static_cast<BreakStmt*>(s);
                    if (!b->label.empty()) {
                        auto it = owner_->labelTargets_.find(b->label);
                        if (it != owner_->labelTargets_.end()) {
                            owner_->b_->CreateBr(it->second.first);
                            return;
                        }
                    }
                    if (!owner_->breakTargets_.empty())
                        owner_->b_->CreateBr(owner_->breakTargets_.back());
                    return;
                }
                case NodeKind::ContinueStmt: {
                    // `continue` 跳到最内层循环步进位；带标签时跳到对应循环头。
                    auto* c = static_cast<ContinueStmt*>(s);
                    if (!c->label.empty()) {
                        auto it = owner_->labelTargets_.find(c->label);
                        if (it != owner_->labelTargets_.end()) {
                            owner_->b_->CreateBr(it->second.second);
                            return;
                        }
                    }
                    if (!owner_->continueTargets_.empty())
                        owner_->b_->CreateBr(owner_->continueTargets_.back());
                    return;
                }
                case NodeKind::RepeatWhileStmt: {
                    // `repeat { … } while cond` — the body always runs at least once.
                    auto* rw = static_cast<RepeatWhileStmt*>(s);
                    llvm::Function* f = owner_->b_->GetInsertBlock()->getParent();
                    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*owner_->ctx_, "repeat.body", f);
                    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(*owner_->ctx_, "repeat.cond", f);
                    llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(*owner_->ctx_, "repeat.exit", f);
                    owner_->b_->CreateBr(bodyBB);
                    owner_->b_->SetInsertPoint(bodyBB);
                    owner_->breakTargets_.push_back(exitBB);
                    owner_->continueTargets_.push_back(condBB);
                    for (auto& st : rw->body) owner_->genStmt(st.get());
                    owner_->breakTargets_.pop_back();
                    owner_->continueTargets_.pop_back();
                    if (!owner_->b_->GetInsertBlock()->getTerminator()) owner_->b_->CreateBr(condBB);
                    owner_->b_->SetInsertPoint(condBB);
                    llvm::Value* c = rw->condition ? owner_->genExpr(rw->condition.get()) : nullptr;
                    if (!c) { owner_->b_->CreateBr(exitBB); owner_->b_->SetInsertPoint(exitBB); return; }
                    c = owner_->coerce(c, llvm::Type::getInt1Ty(*owner_->ctx_));
                    owner_->b_->CreateCondBr(c, bodyBB, exitBB);
                    owner_->b_->SetInsertPoint(exitBB);
                    return;
                }
                case NodeKind::ReturnStmt: {
                    auto* r = static_cast<ReturnStmt*>(s);
                    llvm::Value* v = r->value ? owner_->genExpr(r->value.get()) : nullptr;
                    if (v && !owner_->currentRet_->isVoidTy()) v = owner_->coerce(v, owner_->currentRet_);
                    // Scoped references die here; the returned value keeps its own ref.
                    owner_->releaseScopeTo(owner_->currentScopeMark_);
                    if (owner_->currentRet_ && owner_->currentRet_->isVoidTy()) { owner_->b_->CreateRetVoid(); return; }
                    if (!v) v = llvm::Constant::getNullValue(owner_->currentRet_);
                    owner_->b_->CreateRet(v);
                    return;
                }
                case NodeKind::IfStmt: {
                    auto* ifs = static_cast<IfStmt*>(s);
                    // `if let x = opt { … }`: the condition is a binding.
                    VarDecl* binding = ifs->condition &&
                                       ifs->condition->kind == NodeKind::VarDecl
                                           ? static_cast<VarDecl*>(ifs->condition.get())
                                           : nullptr;
                    llvm::Value* payload = nullptr;
                    llvm::Value* c = binding ? owner_->genOptionalBinding(binding, payload)
                                             : (ifs->condition ? owner_->genExpr(ifs->condition.get())
                                                               : nullptr);
                    if (!c) return;
                    c = owner_->coerce(c, llvm::Type::getInt1Ty(*owner_->ctx_));
                    llvm::Function* f = owner_->b_->GetInsertBlock()->getParent();
                    auto* tBB = llvm::BasicBlock::Create(*owner_->ctx_, "then", f);
                    auto* eBB = llvm::BasicBlock::Create(*owner_->ctx_, "else", f);
                    auto* mBB = llvm::BasicBlock::Create(*owner_->ctx_, "ifcont", f);
                    owner_->b_->CreateCondBr(c, tBB, eBB);
                    owner_->b_->SetInsertPoint(tBB);
                    if (binding) owner_->bindOptionalPayload(binding, payload);
                    for (auto& st : ifs->thenBody) owner_->genStmt(st.get());
                    if (!owner_->b_->GetInsertBlock()->getTerminator()) owner_->b_->CreateBr(mBB);
                    owner_->b_->SetInsertPoint(eBB);
                    if (ifs->elseBranch) owner_->genStmt(ifs->elseBranch.get());
                    if (!owner_->b_->GetInsertBlock()->getTerminator()) owner_->b_->CreateBr(mBB);
                    owner_->b_->SetInsertPoint(mBB);
                    return;
                }
                case NodeKind::GuardStmt: {
                    // `guard cond else { … }` — the else branch must transfer control.
                    auto* g = static_cast<GuardStmt*>(s);
                    VarDecl* binding = g->condition &&
                                       g->condition->kind == NodeKind::VarDecl
                                           ? static_cast<VarDecl*>(g->condition.get())
                                           : nullptr;
                    llvm::Value* payload = nullptr;
                    llvm::Value* c = binding ? owner_->genOptionalBinding(binding, payload)
                                             : (g->condition ? owner_->genExpr(g->condition.get())
                                                             : nullptr);
                    if (!c) return;
                    c = owner_->coerce(c, llvm::Type::getInt1Ty(*owner_->ctx_));
                    llvm::Function* f = owner_->b_->GetInsertBlock()->getParent();
                    auto* fBB = llvm::BasicBlock::Create(*owner_->ctx_, "guard.cont", f);
                    auto* eBB = llvm::BasicBlock::Create(*owner_->ctx_, "guard.else", f);
                    owner_->b_->CreateCondBr(c, fBB, eBB);
                    owner_->b_->SetInsertPoint(eBB);
                    for (auto& st : g->elseBody) owner_->genStmt(st.get());
                    if (!owner_->b_->GetInsertBlock()->getTerminator()) {
                        owner_->b_->CreateUnreachable();
                    }
                    owner_->b_->SetInsertPoint(fBB);
                    if (binding) owner_->bindOptionalPayload(binding, payload);
                    return;
                }
                case NodeKind::LoopStmt: {
                    auto* l = static_cast<LoopStmt*>(s);
                    llvm::Function* f = owner_->b_->GetInsertBlock()->getParent();
                    llvm::BasicBlock* headBB =
                        llvm::BasicBlock::Create(*owner_->ctx_, "loop.head", f);
                    llvm::BasicBlock* exitBB =
                        llvm::BasicBlock::Create(*owner_->ctx_, "loop.exit", f);
                    owner_->b_->CreateBr(headBB);
                    owner_->b_->SetInsertPoint(headBB);
                    if (!l->label.empty())
                        owner_->labelTargets_[l->label] = {exitBB, headBB};
                    owner_->breakTargets_.push_back(exitBB);
                    owner_->continueTargets_.push_back(headBB);
                    for (auto& st : l->body) owner_->genStmt(st.get());
                    owner_->breakTargets_.pop_back();
                    owner_->continueTargets_.pop_back();
                    if (!l->label.empty())
                        owner_->labelTargets_.erase(l->label);
                    if (!owner_->b_->GetInsertBlock()->getTerminator())
                        owner_->b_->CreateBr(headBB);
                    owner_->b_->SetInsertPoint(exitBB);
                    return;
                }
                case NodeKind::WhileStmt: {
                    auto* w = static_cast<WhileStmt*>(s);
                    llvm::Function* f = owner_->b_->GetInsertBlock()->getParent();
                    auto* cBB = llvm::BasicBlock::Create(*owner_->ctx_, "while.cond", f);
                    auto* bBB = llvm::BasicBlock::Create(*owner_->ctx_, "while.body", f);
                    auto* xBB = llvm::BasicBlock::Create(*owner_->ctx_, "while.exit", f);
                    owner_->b_->CreateBr(cBB);
                    owner_->b_->SetInsertPoint(cBB);
                    llvm::Value* c = w->condition ? owner_->genExpr(w->condition.get()) : nullptr;
                    if (!c) { owner_->b_->CreateBr(xBB); owner_->b_->SetInsertPoint(xBB); return; }
                    c = owner_->coerce(c, llvm::Type::getInt1Ty(*owner_->ctx_));
                    owner_->b_->CreateCondBr(c, bBB, xBB);
                    owner_->b_->SetInsertPoint(bBB);
                    owner_->breakTargets_.push_back(xBB);
                    owner_->continueTargets_.push_back(cBB);
                    for (auto& st : w->body) owner_->genStmt(st.get());
                    owner_->breakTargets_.pop_back();
                    owner_->continueTargets_.pop_back();
                    if (!owner_->b_->GetInsertBlock()->getTerminator()) owner_->b_->CreateBr(cBB);
                    owner_->b_->SetInsertPoint(xBB);
                    return;
                }
                case NodeKind::ForInStmt: {
                    auto* fr = static_cast<ForInStmt*>(s);
                    if (fr->isAsync) { owner_->conc_.genForAwait(fr); return; }
                    llvm::Function* f = owner_->b_->GetInsertBlock()->getParent();
                    llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
                    llvm::Value* seqSlot = nullptr;
                    llvm::Type* seqTy = nullptr;
                    llvm::Value* seq = nullptr;
                    llvm::Value* lo = nullptr;
                    llvm::Value* hi = nullptr;
                    bool halfOpen = true;
                    bool dictLike = false;
                    if (fr->sequence && fr->sequence->kind == NodeKind::RangeExpr) {
                        auto* r = static_cast<RangeExpr*>(fr->sequence.get());
                        halfOpen = r->halfOpen;
                        lo = owner_->genExpr(r->lower.get());
                        hi = owner_->genExpr(r->upper.get());
                        if (lo) lo = owner_->coerce(lo, i64);
                        if (hi) hi = owner_->coerce(hi, i64);
                    } else {
                        seq = fr->sequence ? owner_->genExpr(fr->sequence.get()) : nullptr;
                        if (!seq) return;
                        seqTy = seq->getType();
                        seqSlot = owner_->b_->CreateAlloca(seqTy, nullptr, "for.seq");
                        owner_->b_->CreateStore(seq, seqSlot);
                        const Type* seqType0 = fr->sequence ? fr->sequence->semaType : nullptr;
                        dictLike = seqType0 &&
                            (seqType0->kind == TypeKind::Dict ||
                             seqType0->kind == TypeKind::Set);
                    }
                    VarDecl* pat = fr->pattern && fr->pattern->kind == NodeKind::VarDecl
                                       ? static_cast<VarDecl*>(fr->pattern.get())
                                       : nullptr;
                    std::vector<std::string> names;
                    if (pat) {
                        if (!pat->tupleNames.empty()) names = pat->tupleNames;
                        else if (!pat->name.empty()) names.push_back(pat->name);
                    } else if (fr->pattern && fr->pattern->kind == NodeKind::TupleExpr) {
                        for (auto& el : static_cast<TupleExpr*>(fr->pattern.get())->elements) {
                            if (!el) { names.emplace_back(); continue; }
                            if (el->kind == NodeKind::IdentExpr)
                                names.push_back(static_cast<IdentExpr*>(el.get())->name);
                            else if (el->kind == NodeKind::VarDecl)
                                names.push_back(static_cast<VarDecl*>(el.get())->name);
                            else names.emplace_back();
                        }
                    }
                    llvm::BasicBlock* headBB = llvm::BasicBlock::Create(*owner_->ctx_, "for.head", f);
                    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*owner_->ctx_, "for.body", f);
                    llvm::BasicBlock* stepBB = llvm::BasicBlock::Create(*owner_->ctx_, "for.step", f);
                    llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(*owner_->ctx_, "for.exit", f);
                    llvm::Value* idx = owner_->b_->CreateAlloca(i64, nullptr, "for.i");
                    owner_->b_->CreateStore(lo ? lo : llvm::ConstantInt::get(i64, 0), idx);
                    owner_->b_->CreateBr(headBB);
                    llvm::Type* elemTy = i64;
                    llvm::Type* keyTy = i64;
                    llvm::Type* valTy = i64;
                    const Type* seqType = fr->sequence ? fr->sequence->semaType : nullptr;
                    if (seqType) {
                        if (seqType->kind == TypeKind::Array && seqType->element)
                            elemTy = owner_->layout_->lower(seqType->element);
                        else if (seqType->kind == TypeKind::Set && seqType->element) {
                            elemTy = owner_->layout_->lower(seqType->element);
                            keyTy = elemTy;
                        } else if (seqType->kind == TypeKind::Dict) {
                            if (seqType->key) keyTy = owner_->layout_->lower(seqType->key);
                            if (seqType->value) valTy = owner_->layout_->lower(seqType->value);
                        } else if (seqType->kind == TypeKind::String)
                            elemTy = llvm::Type::getInt32Ty(*owner_->ctx_);
                    }
                    owner_->b_->SetInsertPoint(headBB);
                    llvm::Value* i = owner_->b_->CreateLoad(i64, idx);
                    llvm::Value* more;
                    if (lo && hi) {
                        more = halfOpen ? owner_->b_->CreateICmpSLT(i, hi, "for.more")
                                        : owner_->b_->CreateICmpSLE(i, hi, "for.more");
                    } else {
                        llvm::Value* coll = owner_->b_->CreateLoad(seqTy, seqSlot, "for.coll");
                        llvm::Value* count;
                        // Set/Dict iterate through the shared hash-table runtime.
                        // Their variables are stored by pointer (see TypeLayout),
                        // so we hand the runtime the sequence pointer directly.
                        // We classify by semantic type rather than by inspecting
                        // the value: with opaque LLVM pointers the struct shape is
                        // no longer recoverable from the value alone.
                        if (dictLike) {
                            count = owner_->b_->CreateCall(
                                owner_->declareExternalSig("suki_dict_entry_count",
                                    { llvm::PointerType::get(*owner_->ctx_, 0) }, i64),
                                { seqSlot });
                        } else if (seqType && seqType->kind == TypeKind::String) {
                            count = owner_->b_->CreateCall(
                                owner_->declareExternalSig("suki_str_length",
                                    { owner_->layout_->stringTy() }, i64), { coll });
                        } else {
                            count = owner_->b_->CreateExtractValue(coll, {1}, "for.count");
                        }
                        more = owner_->b_->CreateICmpSLT(i, count, "for.more");
                    }
                    owner_->b_->CreateCondBr(more, bodyBB, exitBB);
                    owner_->b_->SetInsertPoint(bodyBB);
                    if (lo) {
                        if (!names.empty()) {
                            owner_->locals_[names[0]] = owner_->b_->CreateAlloca(i64, nullptr, names[0]);
                            owner_->b_->CreateStore(i, owner_->locals_[names[0]]);
                        }
                    } else {
                        llvm::Value* coll = owner_->b_->CreateLoad(seqTy, seqSlot, "for.coll");
                        if (dictLike) {
                            llvm::Value* keySlot = owner_->b_->CreateAlloca(
                                keyTy, nullptr, names.empty() ? "k" : names[0]);
                            llvm::Value* valSlot = owner_->b_->CreateAlloca(
                                valTy, nullptr, names.size() > 1 ? names[1] : "v");
                            owner_->b_->CreateCall(
                                owner_->declareExternalSig("suki_dict_entry_at",
                                    { llvm::PointerType::get(*owner_->ctx_, 0), i64,
                                      llvm::PointerType::get(*owner_->ctx_, 0),
                                      llvm::PointerType::get(*owner_->ctx_, 0) },
                                    llvm::Type::getInt32Ty(*owner_->ctx_)),
                                { seqSlot, i, keySlot, valSlot });
                            if (!names.empty()) owner_->locals_[names[0]] = keySlot;
                            if (names.size() > 1) owner_->locals_[names[1]] = valSlot;
                        } else if (seqType && seqType->kind == TypeKind::String) {
                            llvm::Value* scalar = owner_->b_->CreateCall(
                                owner_->declareExternalSig("suki_str_utf8_get",
                                    { owner_->layout_->stringTy(), llvm::Type::getInt32Ty(*owner_->ctx_) },
                                    llvm::Type::getInt32Ty(*owner_->ctx_)),
                                { coll, owner_->b_->CreateSExt(i, llvm::Type::getInt32Ty(*owner_->ctx_)) });
                            if (!names.empty()) {
                                owner_->locals_[names[0]] = owner_->b_->CreateAlloca(
                                    llvm::Type::getInt32Ty(*owner_->ctx_), nullptr, names[0]);
                                owner_->b_->CreateStore(scalar, owner_->locals_[names[0]]);
                            }
                        } else {
                            llvm::Value* data =
                                owner_->b_->CreateExtractValue(coll, {0}, "for.data");
                            llvm::Value* slot = owner_->b_->CreateAlloca(
                                elemTy, nullptr, names.empty() ? "for.elem" : names[0]);
                            owner_->b_->CreateStore(
                                owner_->b_->CreateLoad(elemTy, owner_->b_->CreateGEP(elemTy, data, {i})),
                                slot);
                            if (!names.empty()) {
                                owner_->locals_[names[0]] = slot;
                                owner_->trackScopedRef(names[0],
                                    pat && pat->semaType ? pat->semaType : nullptr);
                            }
                        }
                    }
                    owner_->breakTargets_.push_back(exitBB);
                    owner_->continueTargets_.push_back(stepBB);
                    for (auto& st : fr->body) owner_->genStmt(st.get());
                    owner_->breakTargets_.pop_back();
                    owner_->continueTargets_.pop_back();
                    if (!owner_->b_->GetInsertBlock()->getTerminator()) owner_->b_->CreateBr(stepBB);
                    owner_->b_->SetInsertPoint(stepBB);
                    llvm::Value* next = owner_->b_->CreateAdd(owner_->b_->CreateLoad(i64, idx),
                                                              llvm::ConstantInt::get(i64, 1));
                    owner_->b_->CreateStore(next, idx);
                    owner_->b_->CreateBr(headBB);
                    owner_->b_->SetInsertPoint(exitBB);
                    return;
                }
                case NodeKind::SwitchStmt: {
                    auto* sw = static_cast<SwitchStmt*>(s);
                    if (sw->isSelect) { owner_->conc_.genSelect(sw); return; }
                    llvm::Value* subject = sw->subject ? owner_->genExpr(sw->subject.get()) : nullptr;
                    if (!subject) return;
                    llvm::Value* switchVal = subject;
                    if (llvm::StructType* st = llvm::dyn_cast<llvm::StructType>(
                            subject->getType())) {
                        if (st->getNumElements() == 2 &&
                            st->getElementType(0)->isIntegerTy(64) &&
                            st->getElementType(1)->isPointerTy()) {
                            switchVal = owner_->b_->CreateExtractValue(subject, {0}, "tag");
                        }
                    }
                    llvm::Function* f = owner_->b_->GetInsertBlock()->getParent();
                    llvm::BasicBlock* entry = owner_->b_->GetInsertBlock();
                    llvm::BasicBlock* merge = llvm::BasicBlock::Create(*owner_->ctx_, "sw.cont", f);
                    std::vector<CaseClause*> arms;
                    CaseClause* defaultArm = nullptr;
                    for (auto& c : sw->cases) {
                        if (!c || c->kind != NodeKind::CaseClause) continue;
                        auto* cc = static_cast<CaseClause*>(c.get());
                        if (cc->isDefault) defaultArm = cc;
                        else arms.push_back(cc);
                    }
                    std::vector<std::pair<int64_t, llvm::BasicBlock*>> armsInt;
                    auto caseTagValue = [this](CaseClause* cc) -> int64_t {
                        if (!cc || cc->whereExpr || cc->isBindingPattern ||
                            !cc->bindings.empty() || !cc->alternatives.empty() ||
                            endsWithFallthrough(cc) || !cc->pattern)
                            return -1;
                        if (cc->pattern->kind == NodeKind::IntLitExpr)
                            return parseIntLiteral(static_cast<IntLitExpr*>(cc->pattern.get())->value);
                        if (cc->pattern->kind == NodeKind::MemberExpr)
                            return owner_->enumCaseTag(static_cast<MemberExpr*>(cc->pattern.get()));
                        return -1;
                    };
                    bool intSwitch = !arms.empty() && switchVal->getType()->isIntegerTy() &&
                                     switchVal->getType()->getIntegerBitWidth() <= 64;
                    for (CaseClause* cc : arms) {
                        if (caseTagValue(cc) < 0) { intSwitch = false; break; }
                    }
                    if (intSwitch) {
                        llvm::BasicBlock* defTarget = merge;
                        if (defaultArm) {
                            defTarget = llvm::BasicBlock::Create(*owner_->ctx_, "sw.default", f);
                            owner_->b_->SetInsertPoint(defTarget);
                            owner_->bindCaseBindings(defaultArm, subject,
                                sw->subject ? sw->subject->semaType : nullptr);
                            for (auto& st : defaultArm->body) owner_->genStmt(st.get());
                            if (!owner_->b_->GetInsertBlock()->getTerminator()) owner_->b_->CreateBr(merge);
                        }
                        for (CaseClause* cc : arms) {
                            llvm::BasicBlock* armBB =
                                llvm::BasicBlock::Create(*owner_->ctx_, "sw.arm", f);
                            owner_->b_->SetInsertPoint(armBB);
                            for (auto& st : cc->body) owner_->genStmt(st.get());
                            if (!owner_->b_->GetInsertBlock()->getTerminator()) owner_->b_->CreateBr(merge);
                            armsInt.push_back({caseTagValue(cc), armBB});
                        }
                        owner_->b_->SetInsertPoint(entry);
                        llvm::SwitchInst* si = owner_->b_->CreateSwitch(
                            switchVal, defTarget, static_cast<unsigned>(armsInt.size()));
                        for (auto& kv : armsInt) {
                            si->addCase(llvm::cast<llvm::ConstantInt>(
                                            llvm::ConstantInt::get(switchVal->getType(),
                                                                  kv.first)),
                                        kv.second);
                        }
                        owner_->b_->SetInsertPoint(merge);
                        return;
                    }
                    std::vector<llvm::BasicBlock*> tests, bodies;
                    for (size_t i = 0; i < arms.size(); ++i) {
                        tests.push_back(llvm::BasicBlock::Create(*owner_->ctx_, "sw.test", f));
                        bodies.push_back(llvm::BasicBlock::Create(*owner_->ctx_, "sw.arm", f));
                    }
                    llvm::BasicBlock* defBody = defaultArm
                        ? llvm::BasicBlock::Create(*owner_->ctx_, "sw.default", f)
                        : nullptr;
                    auto continuation = [&](size_t i) -> llvm::BasicBlock* {
                        if (i + 1 < tests.size()) return tests[i + 1];
                        return defBody ? defBody : merge;
                    };
                    for (size_t i = 0; i < arms.size(); ++i) {
                        CaseClause* cc = arms[i];
                        owner_->b_->SetInsertPoint(bodies[i]);
                        owner_->bindCaseBindings(cc, subject,
                            sw->subject ? sw->subject->semaType : nullptr);
                        if (cc->whereExpr) {
                            llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*owner_->ctx_, "sw.ok", f);
                            llvm::Value* w = owner_->genExpr(cc->whereExpr.get());
                            owner_->b_->CreateCondBr(w ? owner_->toBool(w) : owner_->trueVal(), okBB,
                                             i + 1 < bodies.size() ? bodies[i + 1]
                                                                   : (defBody ? defBody : merge));
                            owner_->b_->SetInsertPoint(okBB);
                        }
                        for (auto& st : cc->body) owner_->genStmt(st.get());
                        if (!owner_->b_->GetInsertBlock()->getTerminator()) {
                            llvm::BasicBlock* next = i + 1 < bodies.size() ? bodies[i + 1]
                                                        : (defBody ? defBody : merge);
                            owner_->b_->CreateBr(endsWithFallthrough(cc) ? next : merge);
                        }
                    }
                    if (defaultArm) {
                        owner_->b_->SetInsertPoint(defBody);
                        owner_->bindCaseBindings(defaultArm, subject);
                        if (defaultArm->whereExpr) {
                            llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*owner_->ctx_, "sw.ok", f);
                            llvm::Value* w = owner_->genExpr(defaultArm->whereExpr.get());
                            owner_->b_->CreateCondBr(w ? owner_->toBool(w) : owner_->trueVal(), okBB, merge);
                            owner_->b_->SetInsertPoint(okBB);
                        }
                        for (auto& st : defaultArm->body) owner_->genStmt(st.get());
                        if (!owner_->b_->GetInsertBlock()->getTerminator()) owner_->b_->CreateBr(merge);
                    }
                    owner_->b_->SetInsertPoint(entry);
                    if (!tests.empty()) owner_->b_->CreateBr(tests[0]);
                    else if (defBody) owner_->b_->CreateBr(defBody);
                    else owner_->b_->CreateBr(merge);
                    for (size_t i = 0; i < arms.size(); ++i) {
                        owner_->b_->SetInsertPoint(tests[i]);
                        llvm::Value* cond = owner_->genCaseCondition(arms[i], switchVal);
                        owner_->b_->CreateCondBr(cond ? cond : owner_->trueVal(), bodies[i],
                                         continuation(i));
                    }
                    owner_->b_->SetInsertPoint(merge);
                    return;
                }
                default: owner_->genExpr(s); break;
            }
        }

    llvm::Value* genExpr(Node* e) {
        if (!e) return nullptr;
        switch (e->kind) {
            case NodeKind::IntLitExpr: {
                return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*owner_->ctx_),
                    parseIntLiteral(static_cast<IntLitExpr*>(e)->value));
            }
            case NodeKind::FloatLitExpr:
                return llvm::ConstantFP::get(llvm::Type::getDoubleTy(*owner_->ctx_),
                    std::strtod(static_cast<FloatLitExpr*>(e)->value.c_str(), nullptr));
            case NodeKind::BoolLitExpr:
                return llvm::ConstantInt::get(llvm::Type::getInt1Ty(*owner_->ctx_),
                                              static_cast<BoolLitExpr*>(e)->value ? 1 : 0);
            case NodeKind::StrLitExpr: {
                // A String value is `{ i8* data, i64 length }`, not a bare
                // pointer: the length is what makes embedded NUL bytes and
                // slice/UTF-8 operations well defined.
                auto* s = static_cast<StrLitExpr*>(e);
                llvm::Type* sty = owner_->layout_->stringTy();
                auto makeStr = [&](const std::string& text) -> llvm::Value* {
                    llvm::Value* p = owner_->b_->CreateGlobalStringPtr(text);
                    llvm::Value* v = llvm::UndefValue::get(sty);
                    v = owner_->b_->CreateInsertValue(v, p, {0});
                    v = owner_->b_->CreateInsertValue(
                        v, llvm::ConstantInt::get(llvm::Type::getInt64Ty(*owner_->ctx_),
                                                  static_cast<uint64_t>(text.size())),
                        {1});
                    return v;
                };
                if (s->expressions.empty()) {
                    std::string all;
                    for (auto& seg : s->segments) all += seg;
                    // In a `Char`-typed context a one-scalar string literal
                    // denotes a character (规范 1.5): the value is the Unicode
                    // scalar (i32), not a String.
                    if (e->semaType && e->semaType->kind == TypeKind::Char)
                        return owner_->charScalarOf(makeStr(all));
                    return makeStr(all);
                }
                // Interpolated literal: fold `segments[i] + expr[i] + ...` with
                // runtime concatenation so any length is handled.
                llvm::Function* cat = owner_->declareExternalSig(
                    "suki_str_concat", {sty, sty}, sty);
                llvm::Value* acc = makeStr(s->segments.empty() ? "" : s->segments[0]);
                for (size_t i = 0; i < s->expressions.size(); ++i) {
                    llvm::Value* ev = genExpr(s->expressions[i].get());
                    if (ev) acc = owner_->b_->CreateCall(cat, {acc, owner_->interpolateToString(ev, s->expressions[i]->semaType)});
                    if (i + 1 < s->segments.size())
                        acc = owner_->b_->CreateCall(cat, {acc, makeStr(s->segments[i + 1])});
                }
                return acc;
            }
            case NodeKind::IdentExpr: {
                const std::string& n = static_cast<IdentExpr*>(e)->name;
                auto it = owner_->locals_.find(n);
                if (it != owner_->locals_.end()) {
                    // A local slot is usually an alloca, but a captured
                    // variable inside a closure body is a pointer produced by
                    // reinterpreting the capture context, so its element type
                    // has to be recovered from the semantic type instead of
                    // assuming the value is an AllocaInst.
                    llvm::Value* slot = it->second;
                    // `self` of a `mutating`-owning type is bound directly to the
                    // incoming pointer argument (see `genAccessorBody`); its slot
                    // is an `Argument`, not an `AllocaInst`, and `self` usually
                    // carries no semantic type. The argument already *is* the
                    // address/value the rest of codegen expects (a class pointer,
                    // or a `T*` for a value type), so return it verbatim instead
                    // of trying to recover a type and load through it — doing the
                    // latter returns null and silently breaks `self.prop` inside a
                    // computed getter (e.g. `Box.tripled` calling `self.doubled`).
                    if (llvm::isa<llvm::Argument>(slot))
                        return slot;
                    // inout 形参：槽中存放的是调用方地址 T*，读取时需再解引用
                    // 一次得到值（规范 3.1）。赋值时 owner_->genAddr 直接返回该地址，
                    // 从而写回调用方的变量。
                    if (owner_->inoutLocals_.count(n)) {
                        // inout 形参：槽中存放调用方地址 T*，需多解引用一次取值。
                        llvm::Type* pty = slot->getType();
                        if (!pty->isPointerTy()) return nullptr;
                        llvm::Type* elemTy = (e->semaType && e->semaType->element)
                            ? owner_->layout_->lower(e->semaType->element) : nullptr;
                        if (!elemTy) return nullptr;
                        llvm::Value* addr = owner_->b_->CreateLoad(pty, slot);
                        return owner_->b_->CreateLoad(elemTy, addr);
                    }
                    llvm::Type* slotTy = nullptr;
                    if (auto* ai = llvm::dyn_cast<llvm::AllocaInst>(slot))
                        slotTy = ai->getAllocatedType();
                    else if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(slot))
                        slotTy = gv->getValueType();
                    else if (e->semaType)
                        slotTy = owner_->layout_->lower(e->semaType);
                    if (!slotTy) return nullptr;
                    return owner_->b_->CreateLoad(slotTy, slot);
                }
                auto f = owner_->fns_.find(n);
                if (f != owner_->fns_.end()) return f->second;
                // A module-level variable is a global. It is looked up before
                // the implicit-`self` fallback, so a global is not mistaken for
                // a property of the enclosing type.
                {
                    auto git = owner_->globalsMap_.find(n);
                    if (git != owner_->globalsMap_.end())
                        return owner_->b_->CreateLoad(git->second->getValueType(),
                                              git->second, n);
                }
                // Inside a method body a bare property name means `self.name`
                // (Sema resolves it the same way).
                if (llvm::Value* fld = owner_->genImplicitSelfField(n)) return fld;
                return nullptr;
            }
            case NodeKind::ParenExpr:
                return genExpr(static_cast<ParenExpr*>(e)->expr.get());
            case NodeKind::ForceUnwrapExpr:
                return genExpr(static_cast<UnaryExpr*>(e)->operand.get());
            case NodeKind::OptionalChainExpr:
                // `x?` promotes a value to Optional. The body keeps its own
                // type; only the declared type of the binding changes, so the
                // value is passed through (the flag is set where it is stored).
                return genExpr(static_cast<OptionalChainExpr*>(e)->expr.get());
            case NodeKind::UnaryExpr: {
                auto* u = static_cast<UnaryExpr*>(e);
                // `try?` turns a thrown error into nil instead of propagating
                // it (规范 9.2): the error is captured, then folded into the
                // Optional result.
                if (u->isOptionalTry) {
                    llvm::PointerType* i8p = llvm::PointerType::get(*owner_->ctx_, 0);
                    llvm::Value* slot = owner_->b_->CreateAlloca(i8p, nullptr, "try.slot");
                    owner_->b_->CreateStore(llvm::ConstantPointerNull::get(i8p), slot);
                    llvm::Value* saved = owner_->optionalTrySlot_;
                    owner_->optionalTrySlot_ = slot;
                    llvm::Value* v = genExpr(u->operand.get());
                    owner_->optionalTrySlot_ = saved;
                    llvm::Value* err = owner_->b_->CreateLoad(i8p, slot, "try.err");
                    llvm::Value* failed = owner_->b_->CreateIsNotNull(err, "try.failed");
                    llvm::Type* optTy = e->semaType && e->semaType->kind == TypeKind::Optional
                                            ? owner_->layout_->lower(e->semaType)
                                            : (v ? owner_->layout_->optionalTy(v->getType()) : nullptr);
                    if (!optTy) return nullptr;
                    llvm::Type* payloadTy = optTy->getStructElementType(0);
                    llvm::Value* some = llvm::UndefValue::get(optTy);
                    some = owner_->b_->CreateInsertValue(
                        some, v ? owner_->coerce(v, payloadTy)
                                : llvm::Constant::getNullValue(payloadTy), {0});
                    some = owner_->b_->CreateInsertValue(some, owner_->trueVal(), {1});
                    llvm::Value* none = llvm::Constant::getNullValue(optTy);
                    return owner_->b_->CreateSelect(failed, none, some);
                }
                // `try!` asserts that nothing is thrown: an error panics.
                if (u->isForcedTry) {
                    llvm::PointerType* i8p = llvm::PointerType::get(*owner_->ctx_, 0);
                    llvm::Value* slot = owner_->b_->CreateAlloca(i8p, nullptr, "try.slot");
                    owner_->b_->CreateStore(llvm::ConstantPointerNull::get(i8p), slot);
                    llvm::Value* saved = owner_->optionalTrySlot_;
                    owner_->optionalTrySlot_ = slot;
                    llvm::Value* v = genExpr(u->operand.get());
                    owner_->optionalTrySlot_ = saved;
                    llvm::Value* err = owner_->b_->CreateLoad(i8p, slot, "try.err");
                    llvm::Value* failed = owner_->b_->CreateIsNotNull(err, "try.failed");
                    llvm::Function* f = owner_->b_->GetInsertBlock()->getParent();
                    llvm::BasicBlock* bad = llvm::BasicBlock::Create(*owner_->ctx_, "try.bang.fail", f);
                    llvm::BasicBlock* ok = llvm::BasicBlock::Create(*owner_->ctx_, "try.bang.cont", f);
                    owner_->b_->CreateCondBr(failed, bad, ok);
                    owner_->b_->SetInsertPoint(bad);
                    owner_->b_->CreateCall(owner_->declareExternalSig("panic", { i8p }),
                                   { owner_->b_->CreateGlobalStringPtr(
                                       "try! unexpectedly raised an error") });
                    owner_->b_->CreateUnreachable();
                    owner_->b_->SetInsertPoint(ok);
                    return v;
                }
                llvm::Value* v = genExpr(u->operand.get());
                if (!v) return nullptr;
                // `try` passes the operand value through unchanged. `await` expects a
                // Future handle (i8*) already produced by the async call and unboxes
                // the boxed result of the awaited type.
                if (u->isTry) return v;
                if (u->isAwait) {
                    if (!v) return nullptr;
                    // 规范 §7.4 重入：在 actor 隔离域内，`await` 挂起前先释放隔离，
                    // 让队列中的其他任务得以进入；await 返回后再重新排队取回。
                    if (llvm::Value* isoLock = owner_->currentActorIsolationLock()) {
                        llvm::PointerType* i64p = llvm::PointerType::getUnqual(
                            llvm::Type::getInt64Ty(*owner_->ctx_));
                        owner_->b_->CreateCall(
                            owner_->declareExternalSig("suki_actor_leave", { i64p },
                                llvm::Type::getVoidTy(*owner_->ctx_)),
                            { owner_->b_->CreateBitCast(isoLock, i64p) });
                    }
                    llvm::Type* voidTy = llvm::Type::getVoidTy(*owner_->ctx_);
                    llvm::PointerType* i8p = llvm::PointerType::get(*owner_->ctx_, 0);
                    llvm::Function* awaitFn = owner_->declareExternalSig(
                        "suki_future_await", { i8p, i8p }, voidTy);
                    llvm::Function* freeFn = owner_->declareExternalSig(
                        "suki_future_free", { i8p }, voidTy);
                    const Type* rt = u->semaType;
                    if (!rt || rt->kind == TypeKind::Void ||
                        rt->kind == TypeKind::Unknown) {
                        // Awaiting a `Void` async call still waits for completion,
                        // then releases the handle; no value is produced.
                        llvm::Value* tmp = owner_->b_->CreateAlloca(
                            llvm::Type::getInt64Ty(*owner_->ctx_), nullptr, "await.void");
                        owner_->b_->CreateCall(awaitFn, { v, owner_->b_->CreateBitCast(tmp, i8p) });
                        // 重入：await 完成，重新获取 actor 隔离。
                        if (llvm::Value* isoLock = owner_->currentActorIsolationLock()) {
                            llvm::PointerType* i64p = llvm::PointerType::getUnqual(
                                llvm::Type::getInt64Ty(*owner_->ctx_));
                            owner_->b_->CreateCall(
                                owner_->declareExternalSig("suki_actor_enter", { i64p },
                                    llvm::Type::getVoidTy(*owner_->ctx_)),
                                { owner_->b_->CreateBitCast(isoLock, i64p) });
                        }
                        owner_->b_->CreateCall(freeFn, { v });
                        return llvm::Constant::getNullValue(
                            llvm::Type::getInt32Ty(*owner_->ctx_));
                    }
                    llvm::Type* rty = owner_->layout_->lower(rt);
                    if (!rty) return nullptr;
                    llvm::Value* out = owner_->b_->CreateAlloca(rty, nullptr, "awaited");
                    owner_->b_->CreateCall(awaitFn, { v, owner_->b_->CreateBitCast(out, i8p) });
                    // 重入：await 完成，重新获取 actor 隔离。
                    if (llvm::Value* isoLock = owner_->currentActorIsolationLock()) {
                        llvm::PointerType* i64p = llvm::PointerType::getUnqual(
                            llvm::Type::getInt64Ty(*owner_->ctx_));
                        owner_->b_->CreateCall(
                            owner_->declareExternalSig("suki_actor_enter", { i64p },
                                llvm::Type::getVoidTy(*owner_->ctx_)),
                            { owner_->b_->CreateBitCast(isoLock, i64p) });
                    }
                    owner_->b_->CreateCall(freeFn, { v });
                    return owner_->b_->CreateLoad(rty, out);
                }
                switch (u->op) {
                    case PunctuatorID::Minus:
                        return v->getType()->isFloatingPointTy() ? owner_->b_->CreateFNeg(v)
                                                                 : owner_->b_->CreateNeg(v);
                    case PunctuatorID::Bang:
                        return owner_->b_->CreateNot(owner_->coerce(v, llvm::Type::getInt1Ty(*owner_->ctx_)));
                    case PunctuatorID::Tilde: return owner_->b_->CreateNot(v);
                    // `&x` 取地址：inout 参数据此按引用传递（规范 3.1）。
                    case PunctuatorID::Amp: return owner_->genAddr(u->operand.get());
                    default: return v;
                }
            }
            case NodeKind::BinaryExpr: {
                auto* b = static_cast<BinaryExpr*>(e);
                // `x == nil` / `x != nil` compares an Optional's flag rather
                // than its payload: `nil` has no value of its own to compare.
                if ((b->op == PunctuatorID::EqualEqual ||
                     b->op == PunctuatorID::BangEqual) && b->lhs && b->rhs) {
                    Node* nilSide = b->rhs->kind == NodeKind::NilLitExpr ? b->rhs.get()
                                  : b->lhs->kind == NodeKind::NilLitExpr ? b->lhs.get()
                                                                         : nullptr;
                    if (nilSide) {
                        Node* other = nilSide == b->rhs.get() ? b->lhs.get() : b->rhs.get();
                        if (llvm::Value* v = genExpr(other)) {
                            if (isOptionalShape(v->getType())) {
                                llvm::Value* has = owner_->b_->CreateExtractValue(v, {1}, "hasValue");
                                // `!= nil` is "has a value"; `== nil` is its negation.
                                return b->op == PunctuatorID::BangEqual
                                           ? has : owner_->b_->CreateNot(has, "isNil");
                            }
                            // A reference type is nil exactly when the pointer is null.
                            if (v->getType()->isPointerTy()) {
                                llvm::Value* nonNull = owner_->b_->CreateIsNotNull(v, "nonNull");
                                return b->op == PunctuatorID::BangEqual
                                           ? nonNull : owner_->b_->CreateNot(nonNull, "isNil");
                            }
                        }
                    }
                }
                // `+` on Strings is concatenation, not arithmetic.
                if (b->op == PunctuatorID::Plus && b->lhs && b->rhs &&
                    b->lhs->semaType && b->rhs->semaType &&
                    b->lhs->semaType->kind == TypeKind::String &&
                    b->rhs->semaType->kind == TypeKind::String) {
                    llvm::Value* l = genExpr(b->lhs.get());
                    llvm::Value* r = genExpr(b->rhs.get());
                    if (!l || !r) return nullptr;
                    llvm::Type* sty = owner_->layout_->stringTy();
                    return owner_->b_->CreateCall(
                        owner_->declareExternalSig("suki_str_concat",
                                           {sty, sty}, sty),
                        {owner_->coerce(l, sty), owner_->coerce(r, sty)});
                }
                return owner_->genBinary(b);
            }
            case NodeKind::ClosureExpr:
                return owner_->emitClosure(static_cast<ClosureExpr*>(e));
            case NodeKind::CallExpr: {
                // `unsafeBitCast<From, To>(value)`（规范 §6.3 C 互操作）：将 value
                // 的位模式按 To 类型重新解释。通过 alloca 中转（以 From 存入、以
                // To 读出）对任意同尺寸类型（含聚合 / 指针）都安全，避免 LLVM
                // bitcast 对尺寸不等类型的限制。
                if (auto* ub = static_cast<CallExpr*>(e); ub->isUnsafeBitCast) {
                    if (llvm::Value* v = genExpr(ub->arguments[0].get())) {
                        llvm::Type* dst = owner_->layout_->lower(ub->bitCastTo);
                        if (dst) {
                            llvm::Type* src = owner_->layout_->lower(ub->bitCastFrom);
                            if (src) {
                                llvm::Value* slot = owner_->b_->CreateAlloca(src);
                                owner_->b_->CreateStore(v, slot);
                                llvm::Value* p = owner_->b_->CreateBitCast(
                                    slot, llvm::PointerType::get(dst, 0));
                                return owner_->b_->CreateLoad(dst, p);
                            }
                            return owner_->b_->CreateBitCast(v, dst);
                        }
                    }
                    return nullptr;
                }
                // A built-in scalar type in callee position is a conversion:
                // `Int(x)`, `Double(n)`, `Char("A")`. Sema has already typed the
                // expression as the target type; here it becomes the actual cast.
                if (auto* c = static_cast<CallExpr*>(e);
                    c->callee && c->callee->kind == NodeKind::IdentExpr &&
                    c->arguments.size() == 1 && e->semaType) {
                    const std::string& n =
                        static_cast<IdentExpr*>(c->callee.get())->name;
                    static const char* kScalars[] = {
                        "Int", "UInt", "Int8", "Int16", "Int32", "Int64", "UInt8",
                        "UInt16", "UInt32", "UInt64", "Float", "Double", "Char",
                        "Bool", "String"};
                    bool isScalar = false;
                    for (const char* s : kScalars)
                        if (n == s) { isScalar = true; break; }
                    if (isScalar && !owner_->classTypes_.count(n)) {
                        llvm::Value* v = genExpr(c->arguments[0].get());
                        if (!v) return nullptr;
                        llvm::Type* target = owner_->layout_->lower(e->semaType);
                        // Char keeps its i32 width; other scalars owner_->coerce to their
                        // declared width (i64 for Int, i32 for Int8, ...).
                        if (target->isIntegerTy() && target->getIntegerBitWidth() == 32 &&
                            e->semaType->kind != TypeKind::Char) {
                            // A narrower integer keeps its own width; the value
                            // is sign/zero extended by owner_->coerce below.
                        }
                        return owner_->coerceForCast(v, target, e->semaType);
                    }
                }
                // 类型化非托管指针 `ptr.deallocate()`（规范 §6.5 / §8.1）：编译器特化
                // 发射 `suki_ptr_drop(ptr.raw)`。泛型方法体内的 `self` 字段访问在单态化
                // 后尚未正确生成，故此处合成调用，不依赖标准库方法体。
                if (auto* c = static_cast<CallExpr*>(e);
                    c->callee && c->callee->kind == NodeKind::MemberExpr) {
                    auto* m = static_cast<MemberExpr*>(c->callee.get());
                    if (m->isUnsafeDeallocate && m->base) {
                        const Type* bt = m->base->semaType;
                        if (bt && bt->kind == TypeKind::Named) {
                            llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
                            if (llvm::Value* raw = genUnsafeRawAddr(bt, m->base.get())) {
                                llvm::FunctionCallee drop = owner_->declareExternalSig(
                                    "suki_ptr_drop", {i64}, i64);
                                owner_->b_->CreateCall(drop, {raw});
                            }
                            return nullptr; // void
                        }
                    }
                }
                // `Enum.case(payload...)` builds a tagged union: a tag plus a
                // heap box holding the case's payload fields.
                if (auto* c = static_cast<CallExpr*>(e);
                    c->callee && c->callee->kind == NodeKind::MemberExpr) {
                    auto* m = static_cast<MemberExpr*>(c->callee.get());
                    int64_t tag = -1;
                    if (const TypeRecord* rec = owner_->enumCaseOf(m, &tag)) {
                        const Type* et = nullptr;
                        if (m->base && m->base->kind == NodeKind::IdentExpr) {
                            const std::string& bn =
                                static_cast<IdentExpr*>(m->base.get())->name;
                            auto eit = owner_->enumTypes_.find(bn);
                            if (eit != owner_->enumTypes_.end()) et = eit->second;
                        }
                        // 泛型枚举构造 `Box<Int>.value(42)`：基类是 GenericExpr，
                        // 取其 semaType（实例化类型）以获得正确的 {i64,i8*} 类型。
                        if (!et && m->base && m->base->semaType)
                            et = m->base->semaType;
                        llvm::Type* ety = et ? owner_->layout_->lower(et) : llvm::Type::getInt64Ty(*owner_->ctx_);
                        llvm::Value* boxed = nullptr;
                        if (llvm::StructType* pt = owner_->casePayloadType(
                                rec, (size_t)tag, e->semaType)) {
                            // 载荷盒子必须堆分配：枚举值持有其指针，若用栈 alloca
                            // 会在构造所在栈帧结束后悬垂（枚举值被函数返回/存入变量
                            // 时取到垃圾）。改为 suki_alloc 使其在枚举值生命周期内存活。
                            llvm::Value* sz = llvm::ConstantInt::get(
                                llvm::Type::getInt64Ty(*owner_->ctx_),
                                (uint64_t)owner_->sizeOf(pt));
                            llvm::Value* raw = owner_->b_->CreateCall(
                                owner_->declareExternalSig(
                                    "suki_alloc",
                                    { llvm::Type::getInt64Ty(*owner_->ctx_) },
                                    llvm::PointerType::get(*owner_->ctx_, 0)),
                                { sz }, "payload.heap");
                            llvm::Value* buf = owner_->b_->CreateBitCast(
                                raw, llvm::PointerType::get(pt, 0));
                            for (size_t i = 0; i < c->arguments.size(); ++i) {
                                llvm::Value* av = genExpr(c->arguments[i].get());
                                if (!av) continue;
                                unsigned fi = static_cast<unsigned>(i);
                                if (fi >= pt->getNumElements()) break;
                                owner_->b_->CreateStore(av, owner_->b_->CreateStructGEP(
                                    pt, buf, fi));
                            }
                            boxed = raw;
                        }
                        llvm::Value* v = llvm::UndefValue::get(ety);
                        v = owner_->b_->CreateInsertValue(v, owner_->tagConstant(tag), {0});
                        v = owner_->b_->CreateInsertValue(v,
                            boxed ? boxed
                                  : llvm::ConstantPointerNull::get(
                                        llvm::PointerType::get(*owner_->ctx_, 0)), {1});
                        return v;
                    }
                }
                return owner_->genCall(static_cast<CallExpr*>(e));
            }
            case NodeKind::ArrayLitExpr: {
                // Build via repeated runtime push so one code path serves both
                // literals and `append`.
                auto* a = static_cast<ArrayLitExpr*>(e);
                // A bare `[]` whose annotation is a Set/Dict is an empty (or
                // key-only) hash table, not an array. Emitting it via
                // `suki_array_new` would mismatch the table's 4-field struct and
                // corrupt the capacity fields, so lower it as a dictionary.
                if (a->semaType &&
                    (a->semaType->kind == TypeKind::Set ||
                     a->semaType->kind == TypeKind::Dict)) {
                    llvm::Type* sty = owner_->layout_->lower(a->semaType);
                    if (!sty || !sty->isStructTy()) return nullptr;
                    llvm::Type* kTy = llvm::Type::getInt64Ty(*owner_->ctx_);
                    llvm::Type* vTy = llvm::Type::getInt64Ty(*owner_->ctx_);
                    if (a->semaType->kind == TypeKind::Set && a->semaType->element)
                        kTy = owner_->layout_->lower(a->semaType->element);
                    else if (a->semaType->kind == TypeKind::Dict && a->semaType->key)
                        kTy = owner_->layout_->lower(a->semaType->key);
                    if (a->semaType->kind == TypeKind::Dict && a->semaType->value)
                        vTy = owner_->layout_->lower(a->semaType->value);
                    int64_t ksz = (int64_t)owner_->sizeOf(kTy);
                    int64_t vsz = (int64_t)owner_->sizeOf(vTy);
                    llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
                    llvm::Value* box = owner_->b_->CreateAlloca(sty, nullptr, "set.tmp");
                    owner_->b_->CreateCall(
                        owner_->declareExternalSig("suki_dict_new",
                            { i64, i64, i64, llvm::PointerType::getUnqual(sty) }),
                        { llvm::ConstantInt::get(i64, ksz),
                          llvm::ConstantInt::get(i64, vsz),
                          llvm::ConstantInt::get(i64, (int64_t)a->elements.size()),
                          box });
                    for (auto& el : a->elements) {
                        llvm::Value* v = genExpr(el.get());
                        if (!v) continue;
                        llvm::Value* ka = owner_->b_->CreateAlloca(kTy);
                        owner_->b_->CreateStore(owner_->coerce(v, kTy), ka);
                        llvm::Value* va = owner_->b_->CreateAlloca(vTy);
                        owner_->b_->CreateStore(llvm::Constant::getNullValue(vTy), va);
                        owner_->b_->CreateCall(
                            owner_->declareExternalSig("suki_dict_set",
                                { llvm::PointerType::getUnqual(sty), i64, i64,
                                  llvm::PointerType::getUnqual(kTy),
                                  llvm::PointerType::getUnqual(vTy) }),
                            { box, llvm::ConstantInt::get(i64, ksz),
                              llvm::ConstantInt::get(i64, vsz), ka, va });
                    }
                    return owner_->b_->CreateLoad(sty, box);
                }
                llvm::Type* aty = owner_->layout_->lower(a->semaType);
                if (!aty || !aty->isStructTy()) return nullptr;
                const Type* elemT = a->semaType && a->semaType->kind == TypeKind::Array
                    ? a->semaType->element : nullptr;
                llvm::Type* elemTy = elemT ? owner_->layout_->lower(elemT)
                                           : llvm::Type::getInt64Ty(*owner_->ctx_);
                int64_t esz = owner_->sizeOf(elemTy);
                llvm::Value* cap = llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(*owner_->ctx_),
                    a->elements.empty() ? 0 : (int64_t)a->elements.size());
                llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
                // The array aggregate lives in a temporary slot; runtime entry
                // points take it by pointer (see runtime.h for why).
                llvm::Value* slot = owner_->b_->CreateAlloca(aty, nullptr, "arr.tmp");
                owner_->b_->CreateCall(
                    owner_->declareExternalSig("suki_array_new",
                        { i64, i64, llvm::PointerType::getUnqual(aty) }),
                    { llvm::ConstantInt::get(i64, esz), cap, slot });
                llvm::Function* push = owner_->declareExternalSig("suki_array_push",
                    { llvm::PointerType::getUnqual(aty), i64,
                      llvm::PointerType::getUnqual(elemTy) });
                for (auto& el : a->elements) {
                    llvm::Value* v = genExpr(el.get());
                    if (!v) continue;
                    llvm::Value* ev = owner_->b_->CreateAlloca(elemTy);
                    owner_->b_->CreateStore(owner_->coerce(v, elemTy), ev);
                    owner_->b_->CreateCall(push, { slot,
                        llvm::ConstantInt::get(i64, esz), ev });
                }
                return owner_->b_->CreateLoad(aty, slot);
            }
            case NodeKind::DictLitExpr: {
                // Lower a dictionary literal by allocating the table once and
                // inserting each pair, mirroring the array-literal path.
                auto* dl = static_cast<DictLitExpr*>(e);
                llvm::Type* dty = owner_->layout_->lower(dl->semaType);
                if (!dty || !dty->isStructTy()) return nullptr;
                const Type* bt = dl->semaType;
                llvm::Type* kTy = (bt->kind == TypeKind::Dict && bt->key)
                    ? owner_->layout_->lower(bt->key) : llvm::Type::getInt64Ty(*owner_->ctx_);
                llvm::Type* vTy = (bt->kind == TypeKind::Dict && bt->value)
                    ? owner_->layout_->lower(bt->value) : llvm::Type::getInt64Ty(*owner_->ctx_);
                int64_t ksz = (int64_t)(kTy->getPrimitiveSizeInBits() + 7) / 8;
                int64_t vsz = (int64_t)(vTy->getPrimitiveSizeInBits() + 7) / 8;
                if (ksz <= 0) ksz = 1;
                llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
                llvm::Value* slot = owner_->b_->CreateAlloca(dty, nullptr, "dict.tmp");
                // Size the table to the literal's pair count; the runtime grows
                // it automatically if the estimate is too small.
                owner_->b_->CreateCall(
                    owner_->declareExternalSig("suki_dict_new",
                        { i64, i64, i64, llvm::PointerType::getUnqual(dty) }),
                    { llvm::ConstantInt::get(i64, ksz),
                      llvm::ConstantInt::get(i64, vsz),
                      llvm::ConstantInt::get(i64, (int64_t)dl->keys.size()),
                      slot });
                for (size_t i = 0; i < dl->keys.size() && i < dl->values.size(); ++i) {
                    llvm::Value* k = genExpr(dl->keys[i].get());
                    llvm::Value* v = genExpr(dl->values[i].get());
                    if (!k || !v) continue;
                    llvm::Value* ka = owner_->b_->CreateAlloca(kTy);
                    owner_->b_->CreateStore(owner_->coerce(k, kTy), ka);
                    llvm::Value* va = owner_->b_->CreateAlloca(vTy);
                    owner_->b_->CreateStore(owner_->coerce(v, vTy), va);
                    owner_->b_->CreateCall(
                        owner_->declareExternalSig("suki_dict_set",
                            { llvm::PointerType::getUnqual(dty), i64, i64,
                              llvm::PointerType::getUnqual(kTy),
                              llvm::PointerType::getUnqual(vTy) }),
                        { slot, llvm::ConstantInt::get(i64, ksz),
                          llvm::ConstantInt::get(i64, vsz), ka, va });
                }
                return owner_->b_->CreateLoad(dty, slot);
            }
            case NodeKind::SetLitExpr: {
                // A set is the runtime's hash table with no value payload, so
                // each element is inserted for its uniqueness alone.
                auto* sl = static_cast<SetLitExpr*>(e);
                const Type* bt = sl->semaType;
                if (!bt || bt->kind != TypeKind::Set) return nullptr;
                llvm::Type* sty = owner_->layout_->lower(bt);
                llvm::Type* kTy = bt->element ? owner_->layout_->lower(bt->element)
                                              : llvm::Type::getInt64Ty(*owner_->ctx_);
                llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
                llvm::Value* box = owner_->b_->CreateAlloca(sty, nullptr, "set.tmp");
                owner_->b_->CreateCall(
                    owner_->declareExternalSig("suki_dict_new",
                        { i64, i64, i64, llvm::PointerType::getUnqual(sty) }),
                    { llvm::ConstantInt::get(i64, owner_->sizeOf(kTy)),
                      llvm::ConstantInt::get(i64, 0),
                      llvm::ConstantInt::get(i64, (int64_t)sl->elements.size()),
                      box });
                for (auto& el : sl->elements) {
                    llvm::Value* v = genExpr(el.get());
                    if (!v) continue;
                    llvm::Value* ka = owner_->b_->CreateAlloca(kTy);
                    owner_->b_->CreateStore(owner_->coerce(v, kTy), ka);
                    owner_->b_->CreateCall(
                        owner_->declareExternalSig("suki_dict_set",
                            { llvm::PointerType::getUnqual(sty), i64, i64,
                              llvm::PointerType::getUnqual(kTy),
                              llvm::PointerType::getUnqual(kTy) }),
                        { box, llvm::ConstantInt::get(i64, owner_->sizeOf(kTy)),
                          llvm::ConstantInt::get(i64, 0), ka, ka });
                }
                return owner_->b_->CreateLoad(sty, box);
            }
            case NodeKind::TernaryExpr: {
                // `c ? a : b` — a real branch, because only one side may run.
                // The result travels through a temporary slot instead of a PHI:
                // for a *nested* conditional the PHI would be defined in a block
                // that does not dominate the outer merge, while a slot always
                // holds a value on every path.
                auto* t = static_cast<TernaryExpr*>(e);
                llvm::Value* c = genExpr(t->condition.get());
                if (!c) return nullptr;
                c = owner_->coerce(c, llvm::Type::getInt1Ty(*owner_->ctx_));
                llvm::Type* rt = nullptr;
                if (const Type* st = t->semaType) rt = owner_->layout_->lower(st);
                if (!rt) rt = llvm::Type::getInt64Ty(*owner_->ctx_);
                llvm::Function* f = owner_->b_->GetInsertBlock()->getParent();
                const unsigned cid = ++owner_->condCounter_;
                const std::string tag = "." + std::to_string(cid);
                llvm::BasicBlock* tBB =
                    llvm::BasicBlock::Create(*owner_->ctx_, "cond.true" + tag, f);
                llvm::BasicBlock* eBB =
                    llvm::BasicBlock::Create(*owner_->ctx_, "cond.false" + tag, f);
                llvm::BasicBlock* mBB =
                    llvm::BasicBlock::Create(*owner_->ctx_, "cond.end" + tag, f);
                llvm::Value* slot = owner_->b_->CreateAlloca(rt, nullptr, "cond.tmp");
                owner_->b_->CreateCondBr(c, tBB, eBB);

                owner_->b_->SetInsertPoint(tBB);
                if (llvm::Value* tv = genExpr(t->thenValue.get()))
                    owner_->b_->CreateStore(owner_->coerce(tv, rt), slot);
                owner_->b_->CreateBr(mBB);

                owner_->b_->SetInsertPoint(eBB);
                if (llvm::Value* ev = genExpr(t->elseValue.get()))
                    owner_->b_->CreateStore(owner_->coerce(ev, rt), slot);
                owner_->b_->CreateBr(mBB);

                owner_->b_->SetInsertPoint(mBB);
                return owner_->b_->CreateLoad(rt, slot, "cond");
            }
            case NodeKind::TupleExpr: {
                // Build a tuple value by inserting each element in turn.
                auto* t = static_cast<TupleExpr*>(e);
                if (!t->semaType || t->semaType->kind != TypeKind::Tuple) return nullptr;
                llvm::Type* ty = owner_->layout_->lower(t->semaType);
                llvm::Value* v = llvm::UndefValue::get(ty);
                for (size_t i = 0; i < t->elements.size(); ++i) {
                    if (i >= t->semaType->elements.size()) break;
                    llvm::Value* el = genExpr(t->elements[i].get());
                    if (!el) continue;
                    v = owner_->b_->CreateInsertValue(v, el, {static_cast<unsigned>(i)});
                }
                return v;
            }
            case NodeKind::SubscriptExpr: {
                // `xs[i]` — the array is a runtime structure, so element access
                // goes through the runtime rather than a GEP.
                auto* sx = static_cast<SubscriptExpr*>(e);
                const Type* bt = sx->base ? sx->base->semaType : nullptr;
                if (sx->indices.empty()) return nullptr;
                llvm::Value* recv = genExpr(sx->base.get());
                if (!recv) return nullptr;
                llvm::Value* idx = genExpr(sx->indices[0].get());
                if (!idx) return nullptr;
                // 类型化缓冲指针下标读取（规范 §6.5 / §8.1）：
                // inttoptr(base.raw + idx*stride, T*) 后 load。
                if (sx->isUnsafeBufferSubscript) {
                    const Type* et = sx->semaType;
                    if (bt && bt->kind == TypeKind::Named && et) {
                        if (llvm::Value* ep = genUnsafeElemPtr(bt, sx->base.get(), et, idx))
                            return owner_->b_->CreateLoad(
                                owner_->layout_->lower(et), ep, "buf.elem");
                    }
                    return nullptr;
                }
                if (bt && bt->kind == TypeKind::Array) {
                    llvm::Type* aty = owner_->layout_->lower(bt);
                    const Type* et = bt->element;
                    llvm::Type* elemTy = et ? owner_->layout_->lower(et)
                                            : llvm::Type::getInt64Ty(*owner_->ctx_);
                    llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
                    recv = owner_->coerce(recv, aty);
                    llvm::Value* slot = owner_->b_->CreateAlloca(aty, nullptr, "arr.tmp");
                    owner_->b_->CreateStore(recv, slot);
                    llvm::Value* raw = owner_->b_->CreateCall(
                        owner_->declareExternalSig("suki_array_get",
                            { llvm::PointerType::getUnqual(aty), i64 },
                            llvm::PointerType::getUnqual(elemTy)),
                        { slot, owner_->coerce(idx, i64) });
                    if (!raw) return nullptr;
                    return owner_->b_->CreateLoad(elemTy, raw, "elem");
                }
                if (bt && (bt->kind == TypeKind::Dict || bt->kind == TypeKind::Set)) {
                    // Dictionaries and sets share the runtime hash table.
                    llvm::Type* dty = owner_->layout_->lower(bt);
                    llvm::Type* kTy = bt->kind == TypeKind::Dict
                        ? owner_->layout_->lower(bt->key) : owner_->layout_->lower(bt->element);
                    llvm::Type* vTy = bt->kind == TypeKind::Dict
                        ? owner_->layout_->lower(bt->value) : kTy;
                    if (!kTy || !vTy) return nullptr;
                    llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
                    int64_t ksz = owner_->sizeOf(kTy);
                    int64_t vsz = owner_->sizeOf(vTy);
                    llvm::Value* box = owner_->b_->CreateAlloca(dty, nullptr, "dict.tmp");
                    owner_->b_->CreateStore(owner_->coerce(recv, dty), box);
                    llvm::Value* ka = owner_->b_->CreateAlloca(kTy);
                    owner_->b_->CreateStore(owner_->coerce(idx, kTy), ka);
                    llvm::Value* out = owner_->b_->CreateAlloca(vTy, nullptr, "dict.out");
                    owner_->b_->CreateStore(llvm::Constant::getNullValue(vTy), out);
                    owner_->b_->CreateCall(
                        owner_->declareExternalSig("suki_dict_get",
                            { llvm::PointerType::getUnqual(dty), i64, i64,
                              llvm::PointerType::getUnqual(kTy),
                              llvm::PointerType::getUnqual(vTy) },
                            llvm::Type::getInt32Ty(*owner_->ctx_)),
                        { box, llvm::ConstantInt::get(i64, ksz),
                          llvm::ConstantInt::get(i64, vsz), ka, out });
                    return owner_->b_->CreateLoad(vTy, out);
                }
                // 自定义下标（规范 3.1）：在具名类型上查找 subscript 成员的 getter。
                if (bt && bt->kind == TypeKind::Named) {
                    std::string key = bt->name + ".subscript.get";
                    auto sit = owner_->methodFns_.find(key);
                    if (sit != owner_->methodFns_.end() && sit->second) {
                        llvm::Function* gf = sit->second;
                        llvm::FunctionType* gfty = gf->getFunctionType();
                        llvm::Value* recv = nullptr;
                        bool selfByPointer = gfty->getNumParams() &&
                            gfty->getParamType(0)->isPointerTy();
                        if (selfByPointer && !owner_->layout_->isReferenceType(bt))
                            recv = owner_->genAddr(sx->base.get());
                        else
                            recv = genExpr(sx->base.get());
                        if (!recv) return nullptr;
                        std::vector<llvm::Value*> a{recv};
                        for (auto& ix : sx->indices) {
                            llvm::Value* iv = genExpr(ix.get());
                            if (iv) a.push_back(iv);
                        }
                        for (size_t i = 0; i < a.size() && i < gfty->getNumParams(); ++i)
                            a[i] = owner_->coerce(a[i], gfty->getParamType(i));
                        return owner_->b_->CreateCall(gf, a);
                    }
                }
                return nullptr;
            }
            case NodeKind::AsmExpr: {
                auto* a = static_cast<AsmExpr*>(e);
                // 输入操作数求值（rvalue）。
                std::vector<llvm::Value*> inVals;
                std::vector<llvm::Type*> inTys;
                for (auto& inp : a->inputs) {
                    llvm::Value* v = genExpr(inp.expr.get());
                    if (!v) return nullptr;
                    inVals.push_back(v);
                    inTys.push_back(v->getType());
                }
                // 约束串：输出在前（含 '='），输入在后，逗号分隔。
                std::string constraints;
                bool first = true;
                std::vector<llvm::Type*> outTys;
                for (auto& o : a->outputs) {
                    llvm::Type* ot = o.expr->semaType ? owner_->layout_->lower(o.expr->semaType)
                                                      : nullptr;
                    if (!ot) return nullptr;
                    outTys.push_back(ot);
                    if (!first) constraints += ",";
                    constraints += o.constraint;
                    first = false;
                }
                for (auto& inp : a->inputs) {
                    if (!first) constraints += ",";
                    constraints += inp.constraint;
                    first = false;
                }
                llvm::Type* retTy;
                if (outTys.empty()) retTy = llvm::Type::getVoidTy(*owner_->ctx_);
                else if (outTys.size() == 1) retTy = outTys[0];
                else retTy = llvm::StructType::get(*owner_->ctx_, outTys);
                llvm::FunctionType* ft = llvm::FunctionType::get(retTy, inTys, false);
                llvm::InlineAsm* ia = llvm::InlineAsm::get(
                    ft, a->templateStr, constraints, /*hasSideEffects=*/true);
                llvm::Value* call = owner_->b_->CreateCall(ia, inVals);
                // 把内联汇编的输出写回对应变量（暂支持 IdentExpr 输出目标）。
                for (size_t i = 0; i < a->outputs.size(); ++i) {
                    llvm::Value* ov = (a->outputs.size() == 1)
                                          ? call
                                          : owner_->b_->CreateExtractValue(call, {(unsigned)i});
                    llvm::Value* addr = nullptr;
                    if (a->outputs[i].expr->kind == NodeKind::IdentExpr) {
                        auto it = owner_->locals_.find(
                            static_cast<IdentExpr*>(a->outputs[i].expr.get())->name);
                        if (it != owner_->locals_.end()) addr = it->second;
                    }
                    if (addr) owner_->b_->CreateStore(ov, addr);
                }
                if (retTy->isVoidTy()) return llvm::UndefValue::get(retTy);
                return call;
            }
            case NodeKind::MemberExpr: {
                auto* m = static_cast<MemberExpr*>(e);
                // MemoryLayout<T>.size / .stride / .alignment（规范 P4.5）：编译期
                // 布局常量。依据 Sema 标记出的关联类型 T 查 DataLayout 生成常量。
                if (m->isMemoryLayoutQuery && m->memoryLayoutType) {
                    if (llvm::Type* lt = owner_->layout_->lower(m->memoryLayoutType)) {
                        const llvm::DataLayout& dl = owner_->module_->getDataLayout();
                        uint64_t sz = dl.getTypeAllocSize(lt);
                        uint64_t al = dl.getABITypeAlign(lt).value();
                        uint64_t v = (m->member == "alignment") ? al : sz; // size/stride 均含尾部填充
                        return owner_->tagConstant((int64_t)v);
                    }
                }
                // 类型化非托管指针 pointee 读取（规范 §6.5 / §8.1）：
                // inttoptr(base.raw, T*) 后 load。base.raw 为保存地址的 int64 字段。
                if (m->isUnsafePointee) {
                    const Type* bt = m->base ? m->base->semaType : nullptr;
                    const Type* et = m->semaType;
                    if (bt && bt->kind == TypeKind::Named && et) {
                        if (llvm::Value* ep = genUnsafeElemPtr(bt, m->base.get(), et, nullptr))
                            return owner_->b_->CreateLoad(
                                owner_->layout_->lower(et), ep, "pointee");
                    }
                    return nullptr;
                }
                // A labelled tuple element (`pair.code`) is indexed by the label's
                // position, not looked up as a field. Sema records the element
                // type on the expression, so the index comes from there.
                            if (m->base && m->base->semaType &&
                                m->base->semaType->kind == TypeKind::Tuple &&
                                !isNumericIndex(m->member)) {
                                if (llvm::Value* tv = genExpr(m->base.get())) {
                                    unsigned idx = owner_->tupleLabelIndex(m->base.get(), m->member);
                                    auto* tst = llvm::dyn_cast<llvm::StructType>(tv->getType());
                                    if (tst && idx < tst->getNumElements())
                                        return owner_->b_->CreateExtractValue(tv, {idx});
                                }
                            }
                // Aggregate field access lowers to a GEP into the value's
                // storage followed by a load of the field's type.
                // A computed property is a function, not a field: call `get`.
                if (const Type* pt = owner_->selfReceiverType(m->base.get());
                    pt && pt->kind == TypeKind::Named && pt->record) {
                    std::string key = pt->name + "." + m->member;
                    auto cit = owner_->computedProps_.find(key);
                    if (cit != owner_->computedProps_.end()) {
                        llvm::Type* dummy = nullptr;
                        llvm::Function* getter = owner_->accessorFn(
                            pt->record, m->member,
                            AccessorDecl::Kind::Getter, &dummy);
                        if (getter) {
                            // A computed property of a `mutating`-owning type is
                            // declared with `self` by pointer; pass the caller's
                            // address so the getter reads the live storage. Other
                            // getters receive the value (a fresh copy is harmless).
                            llvm::FunctionType* gfty = getter->getFunctionType();
                            bool selfByPointer = gfty->getNumParams() &&
                                gfty->getParamType(0)->isPointerTy();
                            llvm::Value* recv;
                            if (selfByPointer && !owner_->layout_->isReferenceType(pt))
                                recv = owner_->genAddr(m->base.get());
                            else
                                recv = genExpr(m->base.get());
                            if (!recv) return nullptr;
                            return owner_->b_->CreateCall(getter,
                                {owner_->coerce(recv, gfty->getParamType(0))});
                        }
                    }
                }
                {
                    const Type* rt = owner_->selfReceiverType(m->base.get());
                    if (rt && rt->kind == TypeKind::Named && rt->record &&
                        (rt->record->kind == TypeDeclKind::Struct ||
                         rt->record->kind == TypeDeclKind::Class ||
                         rt->record->kind == TypeDeclKind::Actor)) {
                        if (llvm::Value* fv = owner_->genMemberLoad(m)) return fv;
                    }
                }
                // `Enum.case` used as a value denotes that case's tag. A
                // negative tag means this is not an enum case, so fall through.
                if (int64_t tag = owner_->enumCaseTag(m); tag >= 0) {
                    const Type* et = owner_->enumTypeOf(m);
                    if (et && owner_->layout_->lower(et)->isStructTy()) {
                        // Payload-carrying enum: build `{ tag, null }` so the
                        // value matches the enum's representation exactly.
                        llvm::Value* v = llvm::UndefValue::get(owner_->layout_->lower(et));
                        v = owner_->b_->CreateInsertValue(v, owner_->tagConstant(tag), {0});
                        v = owner_->b_->CreateInsertValue(
                            v, llvm::ConstantPointerNull::get(
                                   llvm::PointerType::get(*owner_->ctx_, 0)), {1});
                        return v;
                    }
                    return owner_->tagConstant(tag);
                }
                // Parameterless String properties (`s.length`, `s.count`,
                // `s.isEmpty`) are reads rather than calls, so they are
                // materialised here; genStringMethod only sees real calls.
                if (m->base && m->base->semaType &&
                    m->base->semaType->kind == TypeKind::String &&
                    (m->member == "length" || m->member == "count" ||
                     m->member == "isEmpty")) {
                    if (llvm::Value* recv = genExpr(m->base.get())) {
                        llvm::Type* sty = owner_->layout_->stringTy();
                        llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
                        recv = owner_->coerce(recv, sty);
                        llvm::Value* n;
                        if (m->member == "count") {
                            // The runtime reports a scalar count as int32;
                            // widen to Int so it compares against Int literals.
                            n = owner_->b_->CreateSExt(
                                owner_->b_->CreateCall(
                                    owner_->declareExternalSig("suki_str_utf8_count", { sty },
                                        llvm::Type::getInt32Ty(*owner_->ctx_)), { recv }),
                                i64);
                        } else {
                            n = owner_->b_->CreateCall(
                                owner_->declareExternalSig("suki_str_length", { sty }, i64),
                                { recv });
                        }
                        if (m->member == "isEmpty")
                            return owner_->b_->CreateICmpEQ(
                                n, llvm::ConstantInt::get(i64, 0));
                        return n;
                    }
                }
                // Parameterless collection properties (`a.count`, `a.isEmpty`)
                // are reads, not calls, so they are materialised here.
                if (m->base && m->base->semaType &&
                    (m->base->semaType->kind == TypeKind::Array ||
                     m->base->semaType->kind == TypeKind::Dict)) {
                    const Type* bt = m->base->semaType;
                    if (m->member == "count" || m->member == "isEmpty") {
                        llvm::Value* recv = genExpr(m->base.get());
                        if (recv) {
                            llvm::Type* cty = owner_->layout_->lower(bt);
                            llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
                            recv = owner_->coerce(recv, cty);
                            const char* lenFn = bt->kind == TypeKind::Array
                                ? "suki_array_len" : "suki_dict_len";
                            llvm::Value* box = owner_->b_->CreateAlloca(cty, nullptr,
                                                                 "recv.tmp");
                            owner_->b_->CreateStore(recv, box);
                            llvm::Value* n = owner_->b_->CreateCall(
                                owner_->declareExternalSig(lenFn,
                                    { llvm::PointerType::getUnqual(cty) }, i64),
                                { box });
                            if (m->member == "count") return n;
                            return owner_->b_->CreateICmpEQ(
                                n, llvm::ConstantInt::get(i64, 0));
                        }
                    }
                }
                // Tuple element access `t.0` extracts the field at that index.
                if (m->base && m->base->semaType &&
                    m->base->semaType->kind == TypeKind::Tuple &&
                    !m->member.empty()) {
                    size_t idx = 0;
                    bool numeric = true;
                    for (char c : m->member) {
                        if (c < '0' || c > '9') { numeric = false; break; }
                        idx = idx * 10 + static_cast<size_t>(c - '0');
                    }
                    if (numeric) {
                        if (llvm::Value* base = genExpr(m->base.get())) {
                            if (auto* st = llvm::dyn_cast<llvm::StructType>(
                                    base->getType())) {
                                if (idx < st->getNumElements())
                                    return owner_->b_->CreateExtractValue(
                                        base, {static_cast<unsigned>(idx)},
                                        "tuple." + m->member);
                            }
                        }
                    }
                }
                return nullptr;
            }
            case NodeKind::AssignmentExpr: {
                auto* a = static_cast<AssignmentExpr*>(e);
                // Field assignment stores through the field's GEP, which keeps
                // the aggregate's value semantics (write in place, no copy).
                if (a->lhs && a->lhs->kind == NodeKind::MemberExpr) {
                    auto* m = static_cast<MemberExpr*>(a->lhs.get());
                    // 类型化非托管指针 pointee 写入（规范 §6.5 / §8.1）。
                    if (m->isUnsafePointee) {
                        const Type* bt = m->base ? m->base->semaType : nullptr;
                        const Type* et = m->semaType;
                        llvm::Value* v = genExpr(a->rhs.get());
                        if (bt && bt->kind == TypeKind::Named && et && v) {
                            if (llvm::Value* ep =
                                    genUnsafeElemPtr(bt, m->base.get(), et, nullptr)) {
                                owner_->b_->CreateStore(
                                    owner_->coerce(v, owner_->layout_->lower(et)), ep);
                                return v;
                            }
                        }
                        return v ? v : nullptr;
                    }
                    if (llvm::GEPOperator* fp = owner_->genFieldPtr(m)) {
                        llvm::Value* v = genExpr(a->rhs.get());
                        if (v) {
                            v = owner_->coerce(v, fp->getResultElementType());
                            if (a->isCompound) {
                                PunctuatorID cop = a->compoundOp;
                                llvm::Value* old =
                                    owner_->b_->CreateLoad(fp->getResultElementType(), fp);
                                llvm::Value* nv = owner_->coerce(
                                    owner_->applyCompound(old, cop, v),
                                    fp->getResultElementType());
                                owner_->b_->CreateStore(nv, fp);
                                return nv;
                            }
                            const Type* ft = m->semaType;
                            // Only a property that actually declared observers
                            // takes this path; everything else falls through to
                            // the ARC-aware store below.
                            if (const Type* rt = owner_->selfReceiverType(m->base.get())) {
                                if (owner_->hasObservers(rt->record, m->member)) {
                                    // Read the outgoing value so `didSet` can see
                                    // it, then let the observers bracket the store.
                                    llvm::Value* oldVal = owner_->b_->CreateLoad(
                                        fp->getResultElementType(), fp);
                                    owner_->fireObservers(rt->record, m->member,
                                        owner_->genAddr(m->base.get()),
                                        v, fp->getResultElementType(), oldVal);
                                    owner_->b_->CreateStore(v, fp);
                                    return v;
                                }
                            }
                            if (ft && owner_->layout_->isReferenceType(ft)) {
                                llvm::Value* old =
                                    owner_->b_->CreateLoad(fp->getResultElementType(), fp);
                                owner_->arcRetainIfRef(v, ft);
                                owner_->b_->CreateStore(v, fp);
                                owner_->arcReleaseIfRef(old, ft);
                            } else {
                                owner_->b_->CreateStore(v, fp);
                            }
                        }
                        return v;
                    }
                    // A computed property has no storage to point at, so the
                    // assignment must dispatch to its `set` accessor instead of
                    // emitting a store. The receiver follows the same calling
                    // convention as a `mutating` method: by pointer (the
                    // accessor's declared first parameter) for a `mutating`-owning
                    // value type, otherwise by value. Skipping this path silently
                    // drops `p.prop = v` on the floor, which is how an unsafe
                    // pointer's `pointee = x` used to no-op.
                    if (const Type* bt = owner_->selfReceiverType(m->base.get())) {
                        if (bt->kind == TypeKind::Named && bt->record) {
                            std::string key = bt->name + "." + m->member;
                            auto cit = owner_->computedProps_.find(key);
                            if (cit != owner_->computedProps_.end() && cit->second) {
                                llvm::Type* dummy = nullptr;
                                llvm::Function* setFn = owner_->accessorFn(
                                    bt->record, m->member,
                                    AccessorDecl::Kind::Setter, &dummy);
                                if (setFn) {
                                    llvm::FunctionType* sfty =
                                        setFn->getFunctionType();
                                    bool selfByPointer =
                                        sfty->getNumParams() &&
                                        sfty->getParamType(0)->isPointerTy();
                                    llvm::Value* recv;
                                    if (selfByPointer &&
                                        !owner_->layout_->isReferenceType(bt))
                                        recv = owner_->genAddr(m->base.get());
                                    else
                                        recv = genExpr(m->base.get());
                                    if (!recv) return nullptr;
                                    llvm::Value* nv = genExpr(a->rhs.get());
                                    if (!nv) return nullptr;
                                    nv = owner_->coerce(nv, sfty->getParamType(1));
                                    owner_->b_->CreateCall(setFn, {recv, nv});
                                    return nv;
                                }
                            }
                        }
                    }
                }
                if (a->lhs && a->lhs->kind == NodeKind::IdentExpr) {
                    const std::string& n = static_cast<IdentExpr*>(a->lhs.get())->name;
                    auto it = owner_->locals_.find(n);
                    if (it != owner_->locals_.end()) {
                        llvm::Value* slot = it->second;
                        // inout 形参：slot 即调用方地址 T*，赋值直接写回该地址，
                        // 复合赋值先解引用读旧值再写回（规范 3.1）。
                        if (owner_->inoutLocals_.count(n)) {
                            llvm::Type* elemTy = (a->lhs->semaType && a->lhs->semaType->element)
                                ? owner_->layout_->lower(a->lhs->semaType->element) : nullptr;
                            if (!elemTy) return nullptr;
                            llvm::Value* v = genExpr(a->rhs.get());
                            if (v) {
                                if (a->isCompound) {
                                    llvm::Value* old = owner_->b_->CreateLoad(elemTy, slot);
                                    llvm::Value* nv = owner_->coerce(owner_->applyCompound(old, a->compoundOp,
                                        owner_->coerce(v, elemTy)), elemTy);
                                    owner_->b_->CreateStore(nv, slot);
                                    return nv;
                                }
                                owner_->b_->CreateStore(owner_->coerce(v, elemTy), slot);
                            }
                            return v;
                        }
                        // As with a load, the slot may be a captured variable
                        // reached through the closure context rather than an
                        // alloca, so fall back to the semantic type.
                        llvm::Type* sty = nullptr;
                        if (auto* ai = llvm::dyn_cast<llvm::AllocaInst>(slot))
                            sty = ai->getAllocatedType();
                        else if (a->lhs->semaType)
                            sty = owner_->layout_->lower(a->lhs->semaType);
                        llvm::Value* v = genExpr(a->rhs.get());
                        if (v && sty) {
                            if (a->isCompound) {
                                PunctuatorID cop = a->compoundOp;
                                // Read-modify-write for `x += y` and friends.
                                llvm::Value* old = owner_->b_->CreateLoad(sty, slot);
                                llvm::Value* nv = owner_->coerce(
                                    owner_->applyCompound(old, cop, owner_->coerce(v, sty)), sty);
                                owner_->b_->CreateStore(nv, slot);
                                return nv;
                            }
                            const Type* lt = a->lhs->semaType;
                            if (lt && owner_->layout_->isReferenceType(lt)) {
                                // Read the outgoing reference before overwriting
                                // the slot, then release it after the store.
                                llvm::Value* old = owner_->b_->CreateLoad(sty, slot);
                                owner_->arcRetainIfRef(v, lt);
                                owner_->b_->CreateStore(owner_->coerce(v, sty), slot);
                                owner_->arcReleaseIfRef(old, lt);
                            } else {
                                owner_->b_->CreateStore(owner_->coerce(v, sty), slot);
                            }
                        }
                        return v;
                    }
                    // A module-level variable is written through its global.
                    if (auto git = owner_->globalsMap_.find(n); git != owner_->globalsMap_.end()) {
                        llvm::Value* v = genExpr(a->rhs.get());
                        if (!v) return nullptr;
                        llvm::Type* gty = git->second->getValueType();
                        if (a->isCompound) {
                            llvm::Value* old = owner_->b_->CreateLoad(gty, git->second);
                            v = owner_->coerce(owner_->applyCompound(old, a->compoundOp,
                                                     owner_->coerce(v, gty)), gty);
                        } else {
                            v = owner_->coerce(v, gty);
                        }
                        owner_->b_->CreateStore(v, git->second);
                        return v;
                    }
                    // `name = expr` inside a method writes the property in place
                    // through the receiver, which is what makes `mutating`
                    // methods observable to the caller.
                    if (llvm::GEPOperator* fp = owner_->genSelfFieldPtr(n)) {
                        llvm::Value* v = genExpr(a->rhs.get());
                        if (v) {
                            v = owner_->coerce(v, fp->getResultElementType());
                            owner_->b_->CreateStore(v, fp);
                        }
                        return v;
                    }
                }
                // 下标写入（规范 3.1）：`a[i] = v` 经运行时 set 写入（数组/字典），
                // 或经自定义下标的 setter。
                if (a->lhs && a->lhs->kind == NodeKind::SubscriptExpr) {
                    auto* sx = static_cast<SubscriptExpr*>(a->lhs.get());
                    const Type* bt = sx->base ? sx->base->semaType : nullptr;
                    if (!bt) return genExpr(a->rhs.get());
                    llvm::Value* recv = genExpr(sx->base.get());
                    llvm::Value* idx = sx->indices.empty() ? nullptr
                                                         : genExpr(sx->indices[0].get());
                    llvm::Value* val = genExpr(a->rhs.get());
                    if (!recv || !idx || !val) return genExpr(a->rhs.get());
                    // 类型化缓冲指针下标写入（规范 §6.5 / §8.1）。
                    if (sx->isUnsafeBufferSubscript) {
                        const Type* et = sx->semaType;
                        if (bt && bt->kind == TypeKind::Named && et) {
                            if (llvm::Value* ep =
                                    genUnsafeElemPtr(bt, sx->base.get(), et, idx)) {
                                owner_->b_->CreateStore(
                                    owner_->coerce(val, owner_->layout_->lower(et)), ep);
                                return val;
                            }
                        }
                        return val;
                    }
                    if (bt->kind == TypeKind::Array) {
                        llvm::Type* aty = owner_->layout_->lower(bt);
                        const Type* et = bt->element;
                        llvm::Type* elemTy = et ? owner_->layout_->lower(et)
                                                : llvm::Type::getInt64Ty(*owner_->ctx_);
                        llvm::Type* i64 = llvm::Type::getInt64Ty(*owner_->ctx_);
                        recv = owner_->coerce(recv, llvm::PointerType::getUnqual(aty));
                        llvm::Value* vptr = owner_->b_->CreateAlloca(elemTy);
                        owner_->b_->CreateStore(owner_->coerce(val, elemTy), vptr);
                        owner_->b_->CreateCall(
                            owner_->declareExternalSig("suki_array_set",
                                { llvm::PointerType::getUnqual(aty), i64,
                                  llvm::PointerType::getUnqual(elemTy) },
                                llvm::Type::getVoidTy(*owner_->ctx_)),
                            { recv, owner_->coerce(idx, i64), vptr });
                        return val;
                    }
                    if (bt->kind == TypeKind::Named) {
                        std::string key = bt->name + ".subscript.set";
                        auto sit = owner_->methodFns_.find(key);
                        if (sit != owner_->methodFns_.end() && sit->second) {
                            llvm::Function* sf = sit->second;
                            llvm::FunctionType* sfty = sf->getFunctionType();
                            llvm::Value* srecv = nullptr;
                            bool selfByPointer = sfty->getNumParams() &&
                                sfty->getParamType(0)->isPointerTy();
                            if (selfByPointer && !owner_->layout_->isReferenceType(bt))
                                srecv = owner_->genAddr(sx->base.get());
                            else
                                srecv = genExpr(sx->base.get());
                            if (!srecv) return genExpr(a->rhs.get());
                            std::vector<llvm::Value*> a2{srecv,
                                owner_->coerce(idx, sfty->getParamType(1)),
                                owner_->coerce(val, sfty->getParamType(2))};
                            return owner_->b_->CreateCall(sf, a2);
                        }
                    }
                    return genExpr(a->rhs.get());
                }
                return genExpr(a->rhs.get());
            }
            // `move x` (规范 4.3 / 6.4)：所有权转移表达式。语义层已在 Sema 中把
            // 源标记为 moved（禁止再次使用）；代码生成层面它只产生操作数的右值——
            // 调用方（如 `return move data`）直接拿到值，源处于“已移走”状态由语义
            // 约束保证，运行期无需额外动作（值类型按位移动，引用类型转移所有权）。
            case NodeKind::MoveExpr: {
                auto* m = static_cast<MoveExpr*>(e);
                return genExpr(m->operand.get());
            }
            default: return nullptr;
        }
    }


    };

    // 并发 lowering 组件实例（async / Channel / TaskGroup / actor）。
    ConcurrencyLowerer conc_{this};
    // 表达式/声明编排组件实例。
    ExprGen exprGen_{this};
    // ARC 处理组件实例。
    ARCPass arc_{this};
    // 异常 lowering 组件实例。
    ExceptionLowerer exc_{this};
    // 类型注册组件实例。
    TypeRegistry typeReg_{this};

    // Lower a semantic type through the shared layout. Returns null when Sema
    // did not annotate the node, letting callers fall back to syntactic
    // inference so older paths keep working.
    // 类型下沉/名字解析已迁移至 TypeRegistry，此处为转发桩。
    llvm::Type* lowerSema(const Type* t) { return typeReg_.lowerSema(t); }
    bool semaIsScalar(const Type* t) { return typeReg_.semaIsScalar(t); }
    llvm::Type* tyByName(const std::string& n) { return typeReg_.tyByName(n); }

    static std::string typeReprName(Node* t) {
        if (!t) return "";
        switch (t->kind) {
            case NodeKind::NamedType: return static_cast<NamedType*>(t)->name;
            case NodeKind::OptionalType:
                return typeReprName(static_cast<OptionalType*>(t)->wrapped.get());
            case NodeKind::ArrayType:
                return typeReprName(static_cast<ArrayType*>(t)->element.get());
            case NodeKind::DictType:
                return typeReprName(static_cast<DictType*>(t)->key.get());
            default: return "";
        }
    }

    static bool hasAttr(Node* n, const char* a) {
        for (auto& s : n->attributes) if (s == a) return true;
        return false;
    }

    // The symbol a monomorphised instance is emitted under: `name<T1,T2>`.
    // The angle brackets are legal in an LLVM symbol, which keeps the instance
    // readable in IR dumps and impossible to confuse with the generic original.
    static std::string monoSymbol(const std::string& base,
                                  const std::vector<std::string>& targs) {
        std::string sym = base + "<";
        for (size_t i = 0; i < targs.size(); ++i) {
            if (i) sym += ",";
            sym += targs[i];
        }
        return sym + ">";
    }

    // 函数声明下沉已迁移至 TypeRegistry，此处为转发桩。
    llvm::Function* declareAs(FunctionDecl* fn, const std::string& symName) {
        return typeReg_.declareAs(fn, symName);
    }
    llvm::Function* declare(FunctionDecl* fn) {
        return typeReg_.declare(fn);
    }
    llvm::Function* declareExternalSig(const std::string& name,
                                       const std::vector<llvm::Type*>& params,
                                       llvm::Type* ret = nullptr) {
        return typeReg_.declareExternalSig(name, params, ret);
    }

    llvm::Function* declareExternal(const std::string& name) {
        auto it = fns_.find(name);
        if (it != fns_.end()) return it->second;
        llvm::Type* p = llvm::PointerType::get(*ctx_, 0);
        llvm::Function* f = nullptr;
        // Runtime primitives have a fixed C signature; anything else is treated
        // as a printf-like variadic returning int.
        if (name == "print" || name == "println" || name == "panic" ||
            name == "suki_alloc" || name == "suki_free") {
            f = llvm::Function::Create(
                llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx_), {p}, false),
                llvm::GlobalValue::ExternalLinkage, name, module_.get());
        } else {
            f = llvm::Function::Create(
                llvm::FunctionType::get(llvm::Type::getInt32Ty(*ctx_), {p}, true),
                llvm::GlobalValue::ExternalLinkage, name, module_.get());
        }
        fns_[name] = f;
        return f;
    }

    // Emit `suki.global_init()`: it stores every module-level variable's
    // initial value. It runs before anything else so globals are observable
    // from the first statement of `main`.
    // ── error-handling state ───────────────────────────────────────────────
    // Error slot of the function being emitted; null when it is not `throws`.
    llvm::Value* currentErrorSlot_ = nullptr;
    // Innermost `catch` dispatcher — where a raised error branches to.
    std::vector<llvm::BasicBlock*> catchTargets_;
    // Slot of the `try?` currently being evaluated: errors land there instead
    // of propagating.
    llvm::Value* optionalTrySlot_ = nullptr;
    // Whether a symbol throws, so a call site knows to pass an error slot.
    std::map<std::string, bool> fnThrows_;
    std::map<std::string, bool> methodThrows_;

    // Counter giving every closure body a unique symbol.
    int closureCounter_ = 0;
    // Counter giving every worker trampoline (`suki_closure_tramp_N`) a unique,
    // deterministic symbol. A member field (not a function-static) so it resets
    // per compile and does not accumulate across multiple TU compiled in one
    // process — a second TU would otherwise see `_3`, `_5`, … instead of `_1`.
    int trampCounter_ = 0;
    // True while emitting `suki.global_init`: a closure built there outlives
    // the frame, so its capture context must be heap-allocated.
    bool inGlobalInit_ = false;

    void genGlobalInitialisers() {
        llvm::FunctionType* ft = llvm::FunctionType::get(
            llvm::Type::getVoidTy(*ctx_), false);
        llvm::Function* init = llvm::Function::Create(
            ft, llvm::GlobalValue::InternalLinkage, "suki.global_init",
            module_.get());
        globalInitFn_ = init;
        llvm::BasicBlock* bb = llvm::BasicBlock::Create(*ctx_, "entry", init);
        b_->SetInsertPoint(bb);
        bool savedMain = isMain_;
        isMain_ = false;
        inGlobalInit_ = true;
        for (auto& gp : globalOrder_) {
            auto sit = globalsMap_.find(gp.first->name);
            if (sit == globalsMap_.end()) continue;
            if (llvm::Value* v = genExpr(gp.first->initializer.get()))
                b_->CreateStore(coerce(v, sit->second->getValueType()),
                                sit->second);
        }
        inGlobalInit_ = false;
        if (!bb->getTerminator()) b_->CreateRetVoid();
        isMain_ = savedMain;
    }

    void generateBodyAs(FunctionDecl* fn, const std::string& symName) {
        llvm::Function* f = fns_[symName];
        if (!f || !f->empty()) return;
        isMain_ = isMainFns_[symName];
        // 自由函数体不在 actor 隔离域内：清空标记，避免上一次 actor 方法生成
        // 时设置的值残留并错误地释放他人的隔离（规范 §7.4）。
        actorIsolationOwner_ = nullptr;
        // 不透明返回类型（规范 5.5）：仅当返回类型为 `some P` 时才优先使用 Sema
        // 推断出的底层具体类型（fn->semaType->ret）；泛型等仍走 fn->returnType->semaType，
        // 避免泛型实例化的克隆 fn->semaType 携带泛型 ret 导致签名不匹配。
        const Type* rt =
            (fn->returnType && fn->returnType->kind == NodeKind::OptionalType &&
             static_cast<OptionalType*>(fn->returnType.get())->isOpaque &&
             fn->semaType && fn->semaType->kind == TypeKind::Function && fn->semaType->ret)
                ? fn->semaType->ret
                : (fn->returnType ? fn->returnType->semaType : nullptr);
        currentRet_ = isMain_ ? llvm::Type::getInt32Ty(*ctx_)
                              : lowerDeclType(rt, fn->returnType.get());
        locals_.clear();
        inoutLocals_.clear();
        scopeRefs_.clear();
        scopeMarks_.clear();
        currentScopeMark_ = 0;
        b_->SetInsertPoint(llvm::BasicBlock::Create(*ctx_, "entry", f));

        if (isMain_ && globalInitFn_) {
            // Globals must hold their initial values before main observes them.
            b_->CreateCall(globalInitFn_, {});
        }

        // A throwing function's hidden trailing parameter is its error slot;
        // clearing it on entry is what lets the caller tell "no error" from an
        // error the body raised.
        llvm::Value* savedErrorSlot = currentErrorSlot_;
        currentErrorSlot_ = nullptr;
        if (fn->isThrows && !isMain_ && f->arg_size() == fn->params.size() + 1) {
            currentErrorSlot_ = f->getArg(f->arg_size() - 1);
            b_->CreateStore(
                llvm::ConstantPointerNull::get(llvm::PointerType::get(*ctx_, 0)),
                currentErrorSlot_);
        }

        unsigned idx = 0;
        if (!isMain_) {
            for (auto& p : fn->params) {
                llvm::Type* pt = lowerDeclType(p.semaType, p.type.get());
                if (p.isInout) {
                    // 按引用传参：形参槽直接持有调用方传入的地址（T*），读取
                    // 经该地址取 val，赋值经该地址写回（规范 3.1）。
                    locals_[p.internalName] = f->getArg(idx);
                    inoutLocals_.insert(p.internalName);
                } else {
                    llvm::Value* slot = b_->CreateAlloca(pt, nullptr, p.internalName);
                    b_->CreateStore(f->getArg(idx), slot);
                    locals_[p.internalName] = slot;
                }
                ++idx;
            }
        }
        for (auto& s : fn->body) genStmt(s.get());
        if (!b_->GetInsertBlock()->getTerminator()) {
            if (currentRet_->isVoidTy()) b_->CreateRetVoid();
            else b_->CreateRet(llvm::Constant::getNullValue(currentRet_));
        }
        currentErrorSlot_ = savedErrorSlot;
    }

    void genStmt(Node* s) { exprGen_.genStmt(s); }

    // 变量声明生成已迁移至 ExprGen，此处为转发桩。
    void genVarDecl(VarDecl* vd) { exprGen_.genVarDecl(vd); }

    // Tag value of an `Enum.case` member reference, or -1 when the expression is
    // not an enum case. Raw-valued enums are represented purely by their tag.
    int64_t enumCaseTag(MemberExpr* m) {
        if (!m || !m->base) return -1;
        // A type name used as an expression (`Color.red`) resolves to Unknown in
        // Sema, so consult the enum table by name first.
        const TypeRecord* rec = nullptr;
        if (m->base->kind == NodeKind::IdentExpr) {
            const std::string& bn = static_cast<IdentExpr*>(m->base.get())->name;
            auto eit = enumTypes_.find(bn);
            if (eit != enumTypes_.end() && eit->second) rec = eit->second->record;
        }
        if (!rec && m->base->semaType && m->base->semaType->kind == TypeKind::Named)
            rec = m->base->semaType->record;
        if (!rec || rec->kind != TypeDeclKind::Enum) return -1;
        for (size_t i = 0; i < rec->cases.size(); ++i)
            if (rec->cases[i].name == m->member)
                return rec->cases[i].rawValue < 0 ? static_cast<int64_t>(i)
                                                  : rec->cases[i].rawValue;
        return -1;
    }

    llvm::ConstantInt* tagConstant(int64_t tag) {
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), tag);
    }

    // The enum Type behind an `Enum.case` reference, or null.
    const Type* enumTypeOf(MemberExpr* m) {
        if (!m || !m->base || m->base->kind != NodeKind::IdentExpr) return nullptr;
        const std::string& bn = static_cast<IdentExpr*>(m->base.get())->name;
        auto eit = enumTypes_.find(bn);
        return eit == enumTypes_.end() ? nullptr : eit->second;
    }

    // Resolve `Enum.case` to its enum record and tag, or null when the
    // expression is not an enum case reference.
    const TypeRecord* enumCaseOf(MemberExpr* m, int64_t* tagOut) {
        if (!m) return nullptr;
        const TypeRecord* rec = nullptr;
        if (m->base && m->base->kind == NodeKind::IdentExpr) {
            const std::string& bn = static_cast<IdentExpr*>(m->base.get())->name;
            auto eit = enumTypes_.find(bn);
            if (eit != enumTypes_.end() && eit->second) rec = eit->second->record;
        }
        if (!rec && m->base && m->base->semaType &&
            m->base->semaType->kind == TypeKind::Named)
            rec = m->base->semaType->record;
        if (!rec || rec->kind != TypeDeclKind::Enum) return nullptr;
        for (size_t i = 0; i < rec->cases.size(); ++i) {
            if (rec->cases[i].name != m->member) continue;
            if (tagOut)
                *tagOut = rec->cases[i].rawValue < 0 ? static_cast<int64_t>(i)
                                                     : rec->cases[i].rawValue;
            return rec;
        }
        return nullptr;
    }

    // Lower a declared type (parameter or return). The semantic type Sema
    // resolved is authoritative: a syntactic name cannot express aggregate
    // layout, so `Shape` would otherwise be indistinguishable from an integer.
    // The syntactic name remains as a fallback for unannotated code.
    // 声明类型下沉已迁移至 TypeRegistry，此处为转发桩。
    llvm::Type* lowerDeclType(const Type* sema, Node* typeRepr) {
        return typeReg_.lowerDeclType(sema, typeRepr);
    }

    // 把枚举泛型参数按具体实例化类型替换成实参类型（如 `Box<Int>` 的 `T`→`Int`）。
    std::vector<const Type*> resolveAssociated(const TypeRecord* rec,
                                                size_t caseIdx,
                                                const Type* enumType) {
        std::vector<const Type*> out;
        if (caseIdx >= rec->cases.size()) return out;
        const auto& assoc = rec->cases[caseIdx].associated;
        out.reserve(assoc.size());
        std::vector<std::pair<std::string, const Type*>> map;
        if (enumType && enumType->kind == TypeKind::Named) {
            for (size_t i = 0;
                 i < rec->genericParams.size() && i < enumType->elements.size(); ++i)
                map.emplace_back(rec->genericParams[i], enumType->elements[i]);
        }
        for (size_t ai = 0; ai < assoc.size(); ++ai) {
            const Type* at = assoc[ai];
            const Type* resolved = at;
            if (at && at->kind == TypeKind::Named) {
                for (auto& kv : map)
                    if (at->name == kv.first) { resolved = kv.second; break; }
            } else if (at && at->kind == TypeKind::Unknown && !map.empty() &&
                       ai < map.size()) {
                // 声明期泛型参数（如 `T`）被解析为 Unknown；按位置映射到对应
                // 泛型实参（第 ai 个关联值 ⇔ 第 ai 个泛型参数 ⇔ elements[ai]）。
                resolved = enumType->elements[ai];
            }
            out.push_back(resolved);
        }
        return out;
    }

    // LLVM struct type holding one case's payload fields, created on demand.
    // 传入 enumType 时按实例化实参替换泛型关联类型（支持泛型枚举 `Box<Int>` 等）。
    llvm::StructType* casePayloadType(const TypeRecord* rec, size_t caseIdx,
                                      const Type* enumType = nullptr) {
        if (caseIdx >= rec->cases.size()) return nullptr;
        const auto& assoc = rec->cases[caseIdx].associated;
        if (assoc.empty()) return nullptr;
        auto concrete = resolveAssociated(rec, caseIdx, enumType);
        std::vector<llvm::Type*> fields;
        fields.reserve(concrete.size());
        for (const Type* at : concrete) fields.push_back(layout_->lower(at));
        return llvm::StructType::get(*ctx_, fields);
    }

    // ── switch support ──────────────────────────────────────────────────────

    // 强制转换已迁移至 TypeRegistry，此处为转发桩。
    llvm::Value* coerceForCast(llvm::Value* v, llvm::Type* target, const Type* to) {
        return typeReg_.coerceForCast(v, target, to);
    }

    // The i1 constant `true` (IRBuilder has no CreateTrue in LLVM 18).
    llvm::Constant* trueVal() { return llvm::ConstantInt::getTrue(*ctx_); }

    // ── optional support ───────────────────────────────────────────────────
    // An Optional lowers to `{ payload, i1 }` (see TypeLayout::optionalTy), so
    // the flag is always field 1 and the wrapped value field 0.

    // True when `ty` has the shape of a lowered Optional.
    static bool isOptionalShape(llvm::Type* ty) {
        auto* st = llvm::dyn_cast_or_null<llvm::StructType>(ty);
        return st && st->getNumElements() == 2 &&
               st->getElementType(1)->isIntegerTy(1);
    }

    // `f(x)` where the parameter is `T?` but the argument is written as a plain
    // `T`: wrap it so the callee observes `.some(x)`.
    llvm::Value* wrapIntoOptional(llvm::Value* v, llvm::Type* optTy) {
        if (!v || !optTy || v->getType() == optTy) return v;
        if (!isOptionalShape(optTy)) return v;
        llvm::Type* payloadTy = optTy->getStructElementType(0);
        llvm::Value* payload = v->getType() == payloadTy ? v : coerce(v, payloadTy);
        llvm::Value* box = llvm::UndefValue::get(optTy);
        box = b_->CreateInsertValue(box, payload, {0});
        return b_->CreateInsertValue(box, trueVal(), {1});
    }

    // `if let x = opt` / `guard let x = opt`: evaluate the optional once and
    // return its `has value` flag; `payload` receives the unwrapped value.
    llvm::Value* genOptionalBinding(VarDecl* vd, llvm::Value*& payload) {
        payload = nullptr;
        if (!vd || !vd->initializer) return nullptr;
        llvm::Value* src = genExpr(vd->initializer.get());
        if (!src) return nullptr;
        if (!isOptionalShape(src->getType())) {
            // A non-optional initialiser is just a value; the binding always
            // succeeds (Sema has already rejected it as a pattern otherwise).
            payload = src;
            return trueVal();
        }
        payload = b_->CreateExtractValue(src, {0}, "unwrap");
        // Aggregates are boxed, so an Optional's field 0 is a pointer to the
        // value rather than the value itself. Load the boxed value back out,
        // unless the wrapped type is itself a reference (whose pointer *is* the
        // value and must not be dereferenced).
        if (payload->getType()->isPointerTy()) {
            llvm::Type* want = nullptr;
            if (vd->semaType) {
                want = layout_->lower(vd->semaType);
            } else if (vd->initializer && vd->initializer->semaType &&
                       vd->initializer->semaType->kind == TypeKind::Optional &&
                       vd->initializer->semaType->element) {
                // The binding's declared type was lost; recover the unwrapped
                // element type from the initialiser's Optional type.
                want = layout_->lower(vd->initializer->semaType->element);
            }
            if (want && !want->isPointerTy())
                payload = b_->CreateLoad(want, payload, "unbox");
        }
        return b_->CreateExtractValue(src, {1}, "hasValue");
    }

    // Bind the unwrapped value of an optional binding under its own name so the
    // rest of the branch can refer to it.
    void bindOptionalPayload(VarDecl* vd, llvm::Value* payload) {
        if (!vd || !payload || vd->name.empty()) return;
        llvm::Value* slot = b_->CreateAlloca(payload->getType(), nullptr, vd->name);
        b_->CreateStore(payload, slot);
        locals_[vd->name] = slot;
    }

    // `Array` and `Dictionary`/`Set` share the `{ ptr, i64, i64 }` shape, so
    // the value alone cannot tell them apart; the sequence's semantic type is
    // the authority. This reports whether a value is a hash-table aggregate
    // *and* that the analyser typed it as a dictionary.
    bool isDictAggregate(llvm::Value* v, const Type* seqType) {
        if (!seqType || (seqType->kind != TypeKind::Dict && seqType->kind != TypeKind::Set))
            return false;
        auto* st = llvm::dyn_cast_or_null<llvm::StructType>(v ? v->getType() : nullptr);
        // `%SukiDict` is `{ entries, len, cap, keys }`, so the element count is
        // four — a fixed 3 would never match and every dictionary would be
        // iterated as if it were an array.
        if (!st || st->getNumElements() < 3) return false;
        return st->getElementType(0)->isPointerTy() &&
               st->getElementType(1)->isIntegerTy();
    }

    // Integer literal text (`42`, `0xFF`, `42i8`) as an int64. The type suffix
    // that the lexer keeps in the text is stripped first. Overflow (ERANGE) is
    // clamped defensively: Sema already reports the out-of-range error and the
    // driver stops before code generation, but this guards any path that reaches
    // here (e.g. malformed input that slipped past analysis) from feeding an
    // out-of-range value into an LLVM constant.
    static int64_t parseIntLiteral(const std::string& in) {
        std::string t = in;
        while (!t.empty() && (std::isalpha((unsigned char)t.back()) || t.back() == '_'))
            t.pop_back();
        errno = 0;
        unsigned long long v = std::strtoull(t.c_str(), nullptr, 0);
        if (errno == ERANGE)
            return std::numeric_limits<int64_t>::max();
        return static_cast<int64_t>(v);
    }

    // The Unicode scalar behind a one-character string literal. ASCII folds to
    // a constant; anything else is decoded by the runtime, which also validates
    // the encoding.
    llvm::Value* charScalarOf(llvm::Value* str) {
        llvm::Type* sty = layout_->stringTy();
        if (llvm::Constant* c = llvm::dyn_cast<llvm::Constant>(str)) {
            if (auto* g = llvm::dyn_cast<llvm::GlobalVariable>(c->getOperand(0))) {
                if (auto* data = llvm::dyn_cast<llvm::ConstantDataArray>(g->getInitializer())) {
                    if (data->getNumElements() == 2) // one character + NUL
                        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_),
                                                      data->getElementAsInteger(0));
                }
            }
        }
        return b_->CreateCall(
            declareExternalSig("suki_str_utf8_get", { sty, llvm::Type::getInt32Ty(*ctx_) },
                llvm::Type::getInt32Ty(*ctx_)),
            { str, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), 0) });
    }

    // Element index of a labelled tuple access (`pair.code`). The label table
    // lives on the tuple's semantic type; an unknown label yields 0, which the
    // caller bounds-checks.
    static bool isNumericIndex(const std::string& s) {
        if (s.empty()) return false;
        for (char c : s)
            if (c < '0' || c > '9') return false;
        return true;
    }
    unsigned tupleLabelIndex(Node* baseExpr, const std::string& label) {
        const Type* bt = baseExpr ? baseExpr->semaType : nullptr;
        if (!bt || bt->kind != TypeKind::Tuple) return 0;
        for (size_t i = 0; i < bt->labels.size() && i < bt->elements.size(); ++i)
            if (bt->labels[i] == label) return static_cast<unsigned>(i);
        return 0;
    }
    // Normalise a value used as a condition: integers compare against zero,
    // pointers against null. Anything else is assumed to already be i1.
    llvm::Value* toBool(llvm::Value* v) {
        if (!v) return trueVal();
        llvm::Type* t = v->getType();
        if (t->isIntegerTy(1)) return v;
        if (t->isIntegerTy()) return b_->CreateICmpNE(v, llvm::ConstantInt::get(t, 0));
        if (t->isPointerTy())
            // LLVM 18 has opaque pointers: compare against a null of the
            // same pointer type instead of reconstructing the pointee type.
            return b_->CreateICmpNE(
                v, llvm::ConstantPointerNull::get(llvm::PointerType::get(
                       *ctx_, cast<llvm::PointerType>(t)->getAddressSpace())));
        if (t->isFloatingPointTy())
            return b_->CreateFCmpONE(v, llvm::ConstantFP::get(t, 0.0));
        return trueVal();
    }

    // True when the arm's last statement is `fallthrough`, i.e. control must
    // continue into the following arm instead of leaving the switch.
    static bool endsWithFallthrough(const CaseClause* cc) {
        if (!cc || cc->body.empty()) return false;
        const Node* last = cc->body.back().get();
        return last && last->kind == NodeKind::FallthroughStmt;
    }

    // Introduce the names a case pattern binds. Two forms exist:
    //   * `case E.c(let r)` — unbox the payload field into a local;
    //   * `case let v`       — bind the subject itself (value-binding pattern).
    void bindCaseBindings(CaseClause* cc, llvm::Value* subject,
                          const Type* subjectType = nullptr) {
        if (!cc) return;
        if (cc->isBindingPattern) {
            // `case let v`: the subject becomes `v` for the body and the guard.
            for (const std::string& n : cc->bindings) {
                llvm::Value* slot = b_->CreateAlloca(subject->getType(), nullptr, n);
                b_->CreateStore(subject, slot);
                locals_[n] = slot;
            }
            return;
        }
        if (cc->bindings.empty() || !cc->pattern ||
            cc->pattern->kind != NodeKind::MemberExpr)
            return;
        auto* pm = static_cast<MemberExpr*>(cc->pattern.get());
        int64_t ptag = -1;
        const TypeRecord* prec = enumCaseOf(pm, &ptag);
        if (!prec) return;
        // Prefer the switch subject's resolved type (it carries the generic
        // instantiation, e.g. `Box<Int>`), falling back to the pattern's base
        // type. A bare enum reference in the pattern resolves to the generic
        // form `Box<T>` whose payload would wrongly stay a pointer.
        const Type* baseType = subjectType
            ? subjectType
            : (pm->base ? pm->base->semaType : nullptr);
        llvm::StructType* pt = casePayloadType(prec, (size_t)ptag, baseType);
        if (!pt) return;
        llvm::Value* p = b_->CreateExtractValue(subject, {1}, "payload.ptr");
        llvm::Value* buf = b_->CreateBitCast(p, llvm::PointerType::getUnqual(pt));
        for (size_t i = 0; i < cc->bindings.size() && i < pt->getNumElements(); ++i) {
            unsigned fi = static_cast<unsigned>(i);
            llvm::Type* et = pt->getElementType(fi);
            if (!et) break;
            llvm::Value* slot = b_->CreateAlloca(et, nullptr, cc->bindings[i]);
            b_->CreateStore(
                b_->CreateLoad(et, b_->CreateStructGEP(pt, buf, fi)), slot);
            locals_[cc->bindings[i]] = slot;
        }
    }

    // Equality of a subject against a case pattern, as an i1. Supports enum
    // tags, scalar literals, ranges (`case 1..<10`) and alternative patterns
    // (`case 1, 2, 3`). A value-binding pattern (`case let v`) always matches.
    llvm::Value* genCaseCondition(CaseClause* cc, llvm::Value* switchVal) {
        if (!cc) return trueVal();
        // `case let v` matches any value; the body binds the subject.
        if (cc->isBindingPattern) return trueVal();
        // Alternatives share the arm: any of them selects this body.
        llvm::Value* result = genCaseCondition(cc->pattern.get(), switchVal);
        for (auto& alt : cc->alternatives) {
            if (!alt || alt->kind != NodeKind::CaseClause) continue;
            auto* ac = static_cast<CaseClause*>(alt.get());
            llvm::Value* v = genCaseCondition(ac->pattern.get(), switchVal);
            result = result ? b_->CreateOr(result, v) : v;
        }
        return result ? result : trueVal();
    }

    // Match a single pattern node against the (already tag-extracted) subject.
    llvm::Value* genCaseCondition(Node* pattern, llvm::Value* subject) {
        if (!pattern) return trueVal();
        // `case E.c` — compare the enum tag.
        if (pattern->kind == NodeKind::MemberExpr) {
            int64_t tag = enumCaseTag(static_cast<MemberExpr*>(pattern));
            if (tag >= 0) {
                llvm::Value* sv = coerce(subject, llvm::Type::getInt64Ty(*ctx_));
                return b_->CreateICmpEQ(sv, llvm::ConstantInt::get(sv->getType(), tag),
                                        "case.match");
            }
            // Not an enum case: fall through to value equality.
        }
        // `case 1..<10` / `case 1...5` — a range test.
        if (pattern->kind == NodeKind::RangeExpr) {
            auto* r = static_cast<RangeExpr*>(pattern);
            llvm::Value* lo = genExpr(r->lower.get());
            llvm::Value* hi = genExpr(r->upper.get());
            if (!lo || !hi) return trueVal();
            llvm::Value* x = coerce(subject, lo->getType());
            lo = coerce(lo, x->getType());
            hi = coerce(hi, x->getType());
            llvm::Value* lowCmp = b_->CreateICmpUGE(x, lo, "range.lo");
            llvm::Value* highCmp = r->halfOpen ? b_->CreateICmpULT(x, hi, "range.hi")
                                              : b_->CreateICmpULE(x, hi, "range.hi");
            return b_->CreateAnd(lowCmp, highCmp, "range.match");
        }
        // Everything else: equality against the literal/constant expression.
        llvm::Value* pv = genExpr(pattern);
        if (!pv) return trueVal();
        // Strings compare by content through the runtime, not by pointer.
        if (llvm::StructType* ps = llvm::dyn_cast<llvm::StructType>(pv->getType())) {
            if (ps->getNumElements() == 2 && ps->getElementType(0)->isPointerTy() &&
                ps->getElementType(1)->isIntegerTy(64)) {
                llvm::Function* cmp = declareExternalSig(
                    "suki_str_compare", {ps, ps}, llvm::Type::getInt32Ty(*ctx_));
                llvm::Value* sv = coerce(subject, ps);
                return b_->CreateICmpEQ(
                    b_->CreateCall(cmp, {sv, pv}), llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), 0),
                    "case.match");
            }
        }
        if (pv->getType()->isFloatingPointTy() && subject->getType()->isFloatingPointTy())
            return b_->CreateFCmpOEQ(pv, coerce(subject, pv->getType()), "case.match");
        llvm::Value* sv = coerce(subject, pv->getType());
        return b_->CreateICmpEQ(sv, pv, "case.match");
    }

    // Address of an aggregate expression: reuse a local's alloca when the base
    // is a plain variable (avoids a pointless load/store round trip), otherwise
    // spill the value into a temporary.
    llvm::Value* genAddr(Node* base) {
        // A class/actor value is already an object pointer, and that pointer
        // *is* the address field access needs; there is no slot to take the
        // address of. This is what makes `a.b.c` chains work.
        if (base && base->semaType && layout_->isReferenceType(base->semaType))
            return genExpr(base);
        if (base && base->kind == NodeKind::IdentExpr) {
            auto it = locals_.find(static_cast<IdentExpr*>(base)->name);
            if (it != locals_.end()) return it->second;
            // Module-level variables also have their address taken here, so an
            // `&counter` reference resolves to the global slot (detached_async.suki
            // passes the global to `atomicAdd`/`mutexLock`).
            auto git = globalsMap_.find(static_cast<IdentExpr*>(base)->name);
            if (git != globalsMap_.end()) return git->second;
        }
        llvm::Value* v = genExpr(base);
        if (!v || !v->getType()->isAggregateType()) return nullptr;
        llvm::Value* p = b_->CreateAlloca(v->getType(), nullptr, "tmp.agg");
        b_->CreateStore(v, p);
        return p;
    }

    // 由若干元素就地构造一个数组（变长参数打包 / 集合字面量复用，规范 3.1）。
    llvm::Value* genArrayFromElems(const Type* arrTy,
                                   std::vector<llvm::Value*> elems) {
        llvm::Type* aty = layout_->lower(arrTy);
        if (!aty || !aty->isStructTy()) return nullptr;
        const Type* elemT = arrTy->element;
        llvm::Type* elemTy = elemT ? layout_->lower(elemT)
                                   : llvm::Type::getInt64Ty(*ctx_);
        int64_t esz = sizeOf(elemTy);
        llvm::Type* i64 = llvm::Type::getInt64Ty(*ctx_);
        llvm::Value* cap = llvm::ConstantInt::get(i64, (int64_t)elems.size());
        llvm::Value* slot = b_->CreateAlloca(aty, nullptr, "arr.tmp");
        b_->CreateCall(declareExternalSig("suki_array_new",
                        { i64, i64, llvm::PointerType::getUnqual(aty) }),
                        { llvm::ConstantInt::get(i64, esz), cap, slot });
        llvm::Function* push = declareExternalSig("suki_array_push",
            { llvm::PointerType::getUnqual(aty), i64,
              llvm::PointerType::getUnqual(elemTy) });
        for (auto& ev : elems) {
            if (!ev) continue;
            llvm::Value* e2 = b_->CreateAlloca(elemTy);
            b_->CreateStore(coerce(ev, elemTy), e2);
            b_->CreateCall(push, { slot, llvm::ConstantInt::get(i64, esz), e2 });
        }
        return b_->CreateLoad(aty, slot);
    }

    // Address of `base.member`'s storage, or null when the base is not an
    // aggregate we can index into.
    // A reference-typed variable keeps an object pointer in its stack slot, so
    // the slot address is `T**` while a field GEP needs the `T*` itself. This
    // loads the pointer out of its slot; other values pass through unchanged.
    llvm::Value* genRefBasePtr(const Type* t, llvm::Value* addr) {
        if (!addr || !t || !layout_->isReferenceType(t)) return addr;
        if (auto* ai = llvm::dyn_cast<llvm::AllocaInst>(addr))
            return b_->CreateLoad(ai->getAllocatedType(), ai);
        return addr;
    }

    // The type a member access is performed on: the annotated receiver type, or
    // the enclosing type when the receiver is the implicit `self` (which carries
    // no annotation of its own).
    const Type* selfReceiverType(Node* base) {
        if (base && base->semaType) return base->semaType;
        if (base && base->kind == NodeKind::IdentExpr &&
            static_cast<IdentExpr*>(base)->name == "self")
            return currentOwner_;
        return nullptr;
    }

    llvm::GEPOperator* genFieldPtr(MemberExpr* m) {
        // A computed property has no storage to point at.
        if (const Type* bt = selfReceiverType(m->base.get())) {
            std::string key = bt->name + "." + m->member;
            if (computedProps_.count(key)) return nullptr;
        }
        llvm::Value* basePtr = genAddr(m->base.get());
        if (!basePtr) return nullptr;
        const Type* bt = m->base ? m->base->semaType : nullptr;
        // `self` carries no annotation of its own; inside a type body the
        // enclosing type is the receiver.
        if (!bt && m->base && m->base->kind == NodeKind::IdentExpr &&
            static_cast<IdentExpr*>(m->base.get())->name == "self")
            bt = currentOwner_;
        basePtr = genRefBasePtr(bt, basePtr);
        if (!bt || bt->kind != TypeKind::Named || !bt->record) return nullptr;
        llvm::StructType* st = layout_->objectType(bt->record);
        int idx = layout_->fieldIndex(bt->record, m->member);
        if (!st || idx < 0) return nullptr;
        return llvm::cast<llvm::GEPOperator>(
            b_->CreateStructGEP(st, basePtr, static_cast<unsigned>(idx), m->member));
    }

    // Address of the implicit `self.<name>` field inside a method body.
    llvm::GEPOperator* genSelfFieldPtr(const std::string& name) {
        if (!currentOwner_ || !currentOwner_->record) return nullptr;
        auto it = locals_.find("self");
        if (it == locals_.end()) return nullptr;
        llvm::Value* selfPtr = genRefBasePtr(currentOwner_, it->second);
        if (!selfPtr) return nullptr;
        llvm::StructType* st = layout_->objectType(currentOwner_->record);
        int idx = layout_->fieldIndex(currentOwner_->record, name);
        if (!st || idx < 0) return nullptr;
        return llvm::cast<llvm::GEPOperator>(
            b_->CreateStructGEP(st, selfPtr, static_cast<unsigned>(idx), name));
    }

    // A bare property name inside a method refers to `self.<name>`.
    llvm::Value* genImplicitSelfField(const std::string& name) {
        llvm::GEPOperator* fp = genSelfFieldPtr(name);
        if (!fp) return nullptr;
        return b_->CreateLoad(fp->getResultElementType(), fp, name);
    }

    llvm::Value* genMemberLoad(MemberExpr* m) {
        llvm::GEPOperator* fp = genFieldPtr(m);
        if (!fp) return nullptr;
        return b_->CreateLoad(fp->getResultElementType(), fp, m->member);
    }

    // Allocate a class instance on the heap and run its initialiser.
    //
    // The object header is `{ vtable*, int64 rc }` (mirrored in TypeLayout and
    // runtime.c): the vtable slot receives the class's dispatch table and the
    // count starts at 1, because the freshly produced reference is itself one
    // strong reference. The object is zeroed first so a class whose initialiser
    // does not touch every property still has a defined state.
    // ── ARC ────────────────────────────────────────────────────────────────
    // Reference counting follows the ownership rules the analyser guarantees.
    // A freshly constructed object already owns one reference (its count starts
    // at 1), so *sharing* an existing reference means retain and *overwriting*
    // a location means releasing what it held before. Both runtime entry points
    // use acquire/release ordering internally, never relaxed.
    // ARC 处理已迁移至 ARCPass 组件，此处为转发桩。
    llvm::Function* arcRetain() { return arc_.arcRetain(); }
    llvm::Function* arcRelease() { return arc_.arcRelease(); }
    void arcRetainIfRef(llvm::Value* v, const Type* t) { arc_.arcRetainIfRef(v, t); }
    void arcReleaseIfRef(llvm::Value* v, const Type* t) { arc_.arcReleaseIfRef(v, t); }

    // A compound assignment (`x += y`) reads the current value, applies the
    // binary operator, and writes the result back. The AST stores the operator
    // in `op`; a plain `=` leaves it as Equal.
    llvm::Value* applyCompound(llvm::Value* oldValue, PunctuatorID op,
                               llvm::Value* rhs) {
        if (!oldValue || !rhs || op == PunctuatorID::None) return rhs;
        if (!oldValue->getType()->isIntegerTy() && !oldValue->getType()->isFloatingPointTy())
            return rhs; // not an arithmetic type: keep the assigned value
        switch (op) {
            case PunctuatorID::Plus:  return b_->CreateAdd(oldValue, rhs);
            case PunctuatorID::Minus: return b_->CreateSub(oldValue, rhs);
            case PunctuatorID::Star:  return b_->CreateMul(oldValue, rhs);
            case PunctuatorID::Slash: return b_->CreateSDiv(oldValue, rhs);
            case PunctuatorID::Percent: return b_->CreateSRem(oldValue, rhs);
            default: return rhs; // `=` and non-arithmetic forms
        }
    }

    llvm::Value* genClassInit(const Type* ct, CallExpr* e) {
        const TypeRecord* rec = ct ? ct->record : nullptr;
        if (!rec) return nullptr;
        llvm::StructType* st = layout_->objectType(rec);
        if (!st) return nullptr;
        llvm::Type* ptrTy = llvm::PointerType::getUnqual(st);
        llvm::Type* i64 = llvm::Type::getInt64Ty(*ctx_);
        llvm::PointerType* i8p = llvm::PointerType::get(*ctx_, 0);

        // Heap allocation keeps the instance alive independently of the frame.
        uint64_t bytes = module_->getDataLayout().getTypeAllocSize(st);
        llvm::Value* raw = b_->CreateCall(
            declareExternalSig("suki_alloc", { i64 }, i8p),
            { llvm::ConstantInt::get(i64, bytes) });
        llvm::Value* obj = b_->CreateBitCast(raw, ptrTy);

        // Zero the whole object (header included), then set the header fields.
        b_->CreateMemSet(raw,
                         llvm::ConstantInt::get(llvm::Type::getInt8Ty(*ctx_), 0),
                         bytes, llvm::MaybeAlign(8));
        // Store this class's dispatch table, so virtual calls resolve to the
        // override belonging to the object's dynamic type.
        if (llvm::GlobalVariable* vt = vtableFor(rec))
            b_->CreateStore(vt, b_->CreateStructGEP(st, obj, 0, "vtable"));
        else
            b_->CreateStore(llvm::ConstantPointerNull::get(i8p),
                            b_->CreateStructGEP(st, obj, 0, "vtable"));
        // Retain count: one for the reference we are about to return.
        b_->CreateStore(llvm::ConstantInt::get(i64, 1),
                        b_->CreateStructGEP(st, obj, 1, "rc"));
        // Destructor slot. A class without its own `deinit` inherits the nearest
        // ancestor's, so cleanup still runs for the whole chain.
        {
            llvm::Function* dtor = nullptr;
            for (const TypeRecord* r = rec; r && !dtor; r = r->superclass) {
                auto dit = methodFns_.find(r->name + ".deinit");
                if (dit != methodFns_.end()) dtor = dit->second;
            }
            llvm::Constant* dtorVal = dtor
                ? llvm::ConstantExpr::getBitCast(
                      llvm::cast<llvm::Function>(dtor), i8p)
                : llvm::ConstantPointerNull::get(i8p);
            b_->CreateStore(dtorVal,
                            b_->CreateStructGEP(st, obj, 2, "deinit.slot"));
        }

        // Run `init(...)` when the class declares one. Arguments are matched by
        // label first (`Point(x: 1)`) and then positionally, mirroring struct
        // construction so both forms read the same.
        auto initIt = methodFns_.find(rec->name + ".init");
        if (initIt != methodFns_.end() && initIt->second) {
            llvm::Function* initFn = initIt->second;
            llvm::FunctionType* fty = initFn->getFunctionType();
            std::vector<llvm::Value*> args;
            args.push_back(obj); // `self` is the freshly allocated object
            for (unsigned i = 1;
                 i < fty->getNumParams() && i - 1 < e->arguments.size(); ++i) {
                llvm::Value* av = genExpr(e->arguments[i - 1].get());
                if (av) args.push_back(coerce(av, fty->getParamType(i)));
            }
            b_->CreateCall(initFn, args);
        }
        return obj;
    }

    // Build a value of struct type `st`, initialising fields from the call's
    // arguments (matched by argument label, else positionally). Unmentioned
    // fields are zero-initialised.
    llvm::Value* genStructInit(const Type* st, CallExpr* e) {
        llvm::Type* sty = layout_->lower(st);
        if (!sty || !sty->isStructTy()) return nullptr;
        llvm::StructType* stl = llvm::cast<llvm::StructType>(sty);
        llvm::Value* slot = b_->CreateAlloca(sty, nullptr, "init.tmp");
        b_->CreateStore(llvm::Constant::getNullValue(sty), slot);
        for (size_t i = 0; i < e->arguments.size(); ++i) {
            llvm::Value* v = genExpr(e->arguments[i].get());
            if (!v) continue;
            long idx = -1;
            const std::string& label =
                (i < e->argumentLabels.size()) ? e->argumentLabels[i] : std::string();
            if (!label.empty() && st->record) {
                int fi = layout_->fieldIndex(st->record, label);
                if (fi >= 0) idx = fi;
            } else if (i < stl->getNumElements()) {
                idx = static_cast<long>(i);
            }
            if (idx < 0 || static_cast<unsigned>(idx) >= stl->getNumElements()) continue;
            unsigned u = static_cast<unsigned>(idx);
            llvm::Value* fp = b_->CreateStructGEP(stl, slot, u);
            b_->CreateStore(coerce(v, stl->getElementType(u)), fp);
        }
        return b_->CreateLoad(sty, slot);
    }

    // 隐式类型强制已迁移至 TypeRegistry，此处为转发桩。
    llvm::Value* coerce(llvm::Value* v, llvm::Type* to) {
        return typeReg_.coerce(v, to);
    }
    llvm::Value* genExpr(Node* e) { return exprGen_.genExpr(e); }

    llvm::Value* genBinary(BinaryExpr* e) {
        if (e->op == PunctuatorID::AmpAmp || e->op == PunctuatorID::PipePipe) {
            llvm::Value* l = coerce(genExpr(e->lhs.get()), llvm::Type::getInt1Ty(*ctx_));
            llvm::Value* r = coerce(genExpr(e->rhs.get()), llvm::Type::getInt1Ty(*ctx_));
            if (!l || !r) return nullptr;
            return (e->op == PunctuatorID::AmpAmp) ? b_->CreateAnd(l, r) : b_->CreateOr(l, r);
        }
        llvm::Value* l = genExpr(e->lhs.get());
        llvm::Value* r = genExpr(e->rhs.get());
        if (!l || !r) return nullptr;
        // Two Strings compare by content, which the runtime implements (and
        // which handles embedded NULs a byte-wise aggregate compare would not).
        // A String is never an ICmp operand, so this has to come first.
        if (l->getType() == layout_->stringTy() && r->getType() == layout_->stringTy()) {
            llvm::Value* cmp = b_->CreateCall(
                declareExternalSig("suki_str_compare",
                                   { layout_->stringTy(), layout_->stringTy() },
                                   llvm::Type::getInt32Ty(*ctx_)),
                { l, r });
            llvm::Value* zero =
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), 0);
            switch (e->op) {
                case PunctuatorID::EqualEqual: return b_->CreateICmpEQ(cmp, zero);
                case PunctuatorID::BangEqual: return b_->CreateICmpNE(cmp, zero);
                default: break;
            }
        }
        bool lf = l->getType()->isFloatingPointTy();
        bool rf = r->getType()->isFloatingPointTy();
        if (lf != rf) { if (lf) r = coerce(r, l->getType()); else l = coerce(l, r->getType()); }
        // Integer operands with different bit-widths (e.g. an `Int32` value
        // compared against an `Int` literal `0`) must be widened to a common
        // width before the ALU / ICmp, otherwise LLVM rejects the mismatched
        // operands (`icmp ne i32 %x, i64 0`). Extend the narrower to the wider
        // with sign extension, matching `coerce`'s integer rule.
        bool li = l->getType()->isIntegerTy() && !l->getType()->isIntegerTy(1);
        bool ri = r->getType()->isIntegerTy() && !r->getType()->isIntegerTy(1);
        if (li && ri && l->getType()->getIntegerBitWidth() != r->getType()->getIntegerBitWidth()) {
            if (l->getType()->getIntegerBitWidth() < r->getType()->getIntegerBitWidth())
                l = coerce(l, r->getType());
            else
                r = coerce(r, l->getType());
        }
        switch (e->op) {
            case PunctuatorID::Plus: return lf ? b_->CreateFAdd(l, r) : b_->CreateAdd(l, r);
            case PunctuatorID::Minus: return lf ? b_->CreateFSub(l, r) : b_->CreateSub(l, r);
            case PunctuatorID::Star: return lf ? b_->CreateFMul(l, r) : b_->CreateMul(l, r);
            case PunctuatorID::Slash: return lf ? b_->CreateFDiv(l, r) : b_->CreateSDiv(l, r);
            case PunctuatorID::Percent: return lf ? b_->CreateFRem(l, r) : b_->CreateSRem(l, r);
            case PunctuatorID::Amp: return b_->CreateAnd(l, r);
            case PunctuatorID::Pipe: return b_->CreateOr(l, r);
            case PunctuatorID::Caret: return b_->CreateXor(l, r);
            case PunctuatorID::LessLess: return b_->CreateShl(l, r);
            case PunctuatorID::GreaterGreater: return b_->CreateAShr(l, r);
            case PunctuatorID::EqualEqual: return cmp(l, r, llvm::CmpInst::ICMP_EQ, lf);
            case PunctuatorID::BangEqual: return cmp(l, r, llvm::CmpInst::ICMP_NE, lf);
            case PunctuatorID::Less: return cmp(l, r, llvm::CmpInst::ICMP_SLT, lf);
            case PunctuatorID::LessEqual: return cmp(l, r, llvm::CmpInst::ICMP_SLE, lf);
            case PunctuatorID::Greater: return cmp(l, r, llvm::CmpInst::ICMP_SGT, lf);
            case PunctuatorID::GreaterEqual: return cmp(l, r, llvm::CmpInst::ICMP_SGE, lf);
            default: return l;
        }
    }

    llvm::Value* cmp(llvm::Value* l, llvm::Value* r, llvm::CmpInst::Predicate p, bool isFloat) {
        if (!isFloat) return b_->CreateICmp(p, l, r);
        switch (p) {
            case llvm::CmpInst::ICMP_SLT: p = llvm::CmpInst::FCMP_OLT; break;
            case llvm::CmpInst::ICMP_SLE: p = llvm::CmpInst::FCMP_OLE; break;
            case llvm::CmpInst::ICMP_SGT: p = llvm::CmpInst::FCMP_OGT; break;
            case llvm::CmpInst::ICMP_SGE: p = llvm::CmpInst::FCMP_OGE; break;
            case llvm::CmpInst::ICMP_EQ: p = llvm::CmpInst::FCMP_OEQ; break;
            case llvm::CmpInst::ICMP_NE: p = llvm::CmpInst::FCMP_ONE; break;
            default: break;
        }
        return b_->CreateFCmp(p, l, r);
    }

    // Map a String method call onto its runtime entry point. Returns nullptr
    // when the member is not a String builtin.
    llvm::Value* genStringMethod(CallExpr* e, const std::string& name,
                                 llvm::Value* recv) {
        llvm::Type* sty = layout_->stringTy();
        recv = coerce(recv, sty);
        auto argAt = [&](size_t i) -> llvm::Value* {
            return i < e->arguments.size() ? genExpr(e->arguments[i].get())
                                           : nullptr;
        };
        if (name == "count" || name == "length") {
            const char* fn = name == "count" ? "suki_str_utf8_count"
                                             : "suki_str_length";
            return b_->CreateCall(
                declareExternalSig(fn, {sty}, llvm::Type::getInt32Ty(*ctx_)),
                {recv});
        }
        if (name == "isEmpty") {
            llvm::Value* len = b_->CreateCall(
                declareExternalSig("suki_str_length", {sty},
                                  llvm::Type::getInt64Ty(*ctx_)), {recv});
            return b_->CreateICmpEQ(len,
                llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0));
        }
        if (name == "hasPrefix" || name == "hasSuffix" || name == "contains") {
            llvm::Value* other = argAt(0);
            if (!other) return nullptr;
            llvm::Value* cmp = b_->CreateCall(
                declareExternalSig("suki_str_compare", {sty, sty},
                                   llvm::Type::getInt32Ty(*ctx_)),
                {recv, coerce(other, sty)});
            if (name == "contains")
                return b_->CreateICmpEQ(cmp,
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), 0));
            // Exact prefix/suffix relation is decided by the runtime helpers.
            const char* fn = name == "hasPrefix" ? "suki_str_has_prefix"
                                                 : "suki_str_has_suffix";
            return b_->CreateCall(
                declareExternalSig(fn, {sty, sty}, llvm::Type::getInt32Ty(*ctx_)),
                {recv, coerce(other, sty)});
        }
        return nullptr;
    }

    // Convert a value used inside string interpolation to a String, going
    // through the runtime's integer/float formatting helpers.
    llvm::Value* interpolateToString(llvm::Value* v, const Type* semaTy = nullptr) {
        llvm::Type* sty = layout_->stringTy();
        if (!v) return llvm::UndefValue::get(sty);
        if (v->getType() == sty) return v;
        llvm::Type* i64 = llvm::Type::getInt64Ty(*ctx_);
        if (v->getType()->isIntegerTy()) {
            llvm::Value* ext = v->getType()->isIntegerTy(64)
                ? v : b_->CreateSExt(v, i64);
            return b_->CreateCall(
                declareExternalSig("suki_int_to_string", {i64}, sty), {ext});
        }
        if (v->getType()->isFloatingPointTy()) {
            llvm::Value* d = b_->CreateSIToFP(v, llvm::Type::getDoubleTy(*ctx_));
            return b_->CreateCall(
                declareExternalSig("suki_double_to_string",
                                   {llvm::Type::getDoubleTy(*ctx_)}, sty), {d});
        }
        // Enums, structs, classes, tuples and other aggregates have no built-in
        // textual form here, so fall back to a "<TypeName>" debug representation.
        // This avoids handing `suki_str_concat` a raw aggregate value (which would
        // fail IR verification). A richer form would come from a
        // CustomStringConvertible conformance, which is not wired up yet.
        if (v->getType()->isStructTy() || v->getType()->isPointerTy()) {
            std::string tn = (semaTy && !semaTy->name.empty()) ? semaTy->name : "?";
            llvm::Value* p = b_->CreateGlobalStringPtr(tn);
            return b_->CreateCall(
                declareExternalSig("suki_debug_repr",
                                   {llvm::PointerType::get(*ctx_, 0)}, sty), {p});
        }
        return v; // last-resort: caller must coerce; should not normally be reached
    }

    // Map Array/Dictionary methods onto runtime entry points.
    // super.init(...) / super.method(...).
    //
    // The receiver stays the current object; only the callee changes, so the
    // call is statically bound to the superclass. An override can therefore
    // extend rather than replace: run the parent implementation, then add
    // behaviour of its own.
    // ── virtual dispatch ───────────────────────────────────────────────────
    // A class's vtable is a global constant of function pointers, one slot per
    // virtual method. The slot order is fixed per hierarchy (a subclass starts
    // from its parent's ordering), so a slot index means the same thing in every
    // table and an override simply replaces one entry.
    struct VirtualSlot {
        std::string name;              // method name
        const TypeRecord* owner;       // class that first declares it
    };
    std::map<std::string, std::vector<VirtualSlot>> vtableLayout_;  // class -> slots
    std::map<std::string, llvm::GlobalVariable*> vtables_;          // class -> table
    // Methods marked `final` are not virtual: they bind statically so a subclass
    // cannot change what a call to them means.
    std::set<std::string> nonVirtual_;                               // "Type.method"
    // Computed properties: "Type.prop" -> the accessor to call. A stored
    // property is absent here, so member access loads a field instead.
    std::map<std::string, VarDecl*> computedProps_;
    // Owners that declare at least one `mutating` member: their accessors take
    // `self` by pointer, matching those methods.
    std::set<std::string> ownerMutating_;
    // "Type.prop" for each property that declared observers, so an assignment
    // knows to fire them.
    std::map<std::string, VarDecl*> observedProps_;
    // True while generating an initialiser or a constructor body: assignments
    // there establish the initial value, so observers must not fire.
    bool inInitializer_ = false;
    // Name of the property whose accessor body is currently being generated, so
    // `didSet` can find the storage it is replacing.
    std::string currentPropertyName_;

    // GEP of a stored property's field, or null when the property is computed
    // (and therefore has no storage to read).
    llvm::GEPOperator* storageFieldPtr(const TypeRecord* rec,
                                       const std::string& name) {
        if (!rec) return nullptr;
        llvm::StructType* st = layout_->objectType(rec);
        int idx = layout_->fieldIndex(rec, name);
        if (!st || idx < 0) return nullptr;
        auto selfIt = locals_.find("self");
        if (selfIt == locals_.end()) return nullptr;
        llvm::Value* selfPtr = genRefBasePtr(
            currentOwner_, selfIt->second);
        if (!selfPtr) return nullptr;
        return llvm::cast<llvm::GEPOperator>(b_->CreateStructGEP(
            st, selfPtr, static_cast<unsigned>(idx), name));
    }

    llvm::Value* genSuperCall(CallExpr* e, const std::string& member) {
        if (!currentOwner_ || !currentOwner_->record) return nullptr;
        const TypeRecord* parent = currentOwner_->record->superclass;
        if (!parent) return nullptr;
        auto it = methodFns_.find(parent->name + "." + member);
        if (it == methodFns_.end() || !it->second) return nullptr;
        llvm::Function* f = it->second;
        llvm::FunctionType* fty = f->getFunctionType();
        auto selfIt = locals_.find("self");
        if (selfIt == locals_.end()) return nullptr;
        // The object pointer, not the slot that holds it.
        llvm::Value* me = genRefBasePtr(currentOwner_, selfIt->second);
        if (!me) return nullptr;

        std::vector<llvm::Value*> args;
        args.push_back(fty->getNumParams() ? coerce(me, fty->getParamType(0)) : me);
        // The source call has no receiver argument, so its arguments map to the
        // callee parameters from index 1 onwards.
        for (size_t i = 0; i < e->arguments.size(); ++i) {
            if (i + 1 >= fty->getNumParams()) break;
            llvm::Value* v = genExpr(e->arguments[i].get());
            if (v) args.push_back(coerce(v, fty->getParamType(i + 1)));
        }
        args.resize(fty->getNumParams());
        return b_->CreateCall(f, args);
    }

    llvm::Value* genCollectionMethod(CallExpr* e, const std::string& name,
                                     llvm::Value* recv, const Type* bt,
                                     llvm::Value* recvSlot = nullptr) {
        llvm::Type* cty = layout_->lower(bt);
        recv = coerce(recv, cty);
        llvm::Type* i64 = llvm::Type::getInt64Ty(*ctx_);
        if (bt->kind == TypeKind::Array) {
            if (name == "count" || name == "isEmpty") {
                llvm::Value* box = b_->CreateAlloca(cty, nullptr, "recv.tmp");
                b_->CreateStore(recv, box);
                llvm::Value* n = b_->CreateCall(
                    declareExternalSig("suki_array_len",
                        { llvm::PointerType::getUnqual(cty) }, i64), { box });
                return name == "count" ? n
                    : b_->CreateICmpEQ(n, llvm::ConstantInt::get(i64, 0));
            }
            if (name == "append") {
                const Type* elemT = bt->element;
                llvm::Type* elemTy = elemT ? layout_->lower(elemT) : i64;
                llvm::Value* v = e->arguments.empty()
                    ? nullptr : genExpr(e->arguments[0].get());
                if (!v) return nullptr;
                int64_t esz = sizeOf(elemTy);
                llvm::Value* slot = b_->CreateAlloca(elemTy);
                b_->CreateStore(coerce(v, elemTy), slot);
                llvm::Value* acc = b_->CreateAlloca(cty, nullptr, "arr.tmp");
                b_->CreateStore(recv, acc);
                b_->CreateCall(
                    declareExternalSig("suki_array_push",
                        { llvm::PointerType::getUnqual(cty), i64,
                          llvm::PointerType::getUnqual(elemTy) }),
                    { acc, llvm::ConstantInt::get(i64, esz), slot });
                return b_->CreateLoad(cty, acc);
            }
            if (name == "removeAt" || name == "removeAll") {
                llvm::Value* idx = e->arguments.empty()
                    ? llvm::ConstantInt::get(i64, 0)
                    : genExpr(e->arguments[0].get());
                if (!idx) return nullptr;
                llvm::Value* acc = b_->CreateAlloca(cty, nullptr, "arr.tmp");
                b_->CreateStore(recv, acc);
                b_->CreateCall(
                    declareExternalSig("suki_array_remove_at",
                        { llvm::PointerType::getUnqual(cty), i64 }),
                    { acc, coerce(idx, i64) });
                return b_->CreateLoad(cty, acc);
            }
        }
        // Dictionary / Set share the runtime's hash table. A Set is simply a
        // dictionary with val_size == 0, so every value copy becomes a no-op
        // and the two need no separate code paths here.
        if (bt->kind == TypeKind::Dict || bt->kind == TypeKind::Set) {
            llvm::Type* kTy;
            llvm::Type* vTy;
            if (bt->kind == TypeKind::Dict) {
                kTy = bt->key ? layout_->lower(bt->key) : i64;
                vTy = bt->value ? layout_->lower(bt->value) : i64;
            } else {
                kTy = bt->element ? layout_->lower(bt->element) : i64;
                vTy = nullptr; // Set carries no value payload
            }
            int64_t ksz = sizeOf(kTy);
            int64_t vsz = vTy ? sizeOf(vTy) : 0;

            // The dictionary aggregate is passed by pointer (see runtime.h on
            // why aggregates avoid by-value passing across the ABI boundary).
            // When the receiver is a named local we mutate its own slot so the
            // change stays observable; otherwise we operate on a temporary.
            llvm::Value* box;
            if (recvSlot && llvm::isa<llvm::AllocaInst>(recvSlot)) {
                box = recvSlot;
            } else {
                box = b_->CreateAlloca(cty, nullptr, "dict.tmp");
                b_->CreateStore(recv, box);
            }
            auto argAt = [&](size_t i) -> llvm::Value* {
                return i < e->arguments.size() ? genExpr(e->arguments[i].get())
                                               : nullptr;
            };
            // Spill an incoming key/value to memory so the runtime, which is
            // type-agnostic, can read it through a `const void*`.
            auto boxOf = [&](llvm::Value* v, llvm::Type* t) -> llvm::Value* {
                if (!v) return nullptr;
                llvm::Value* a = b_->CreateAlloca(t);
                b_->CreateStore(coerce(v, t), a);
                return a;
            };
            auto kPtrTy = [&] { return llvm::PointerType::getUnqual(kTy); };
            auto vPtrTy = [&] { return kPtrTy(); }; // values are read, never keyed
            llvm::Type* i32 = llvm::Type::getInt32Ty(*ctx_);
            // Probe helper: `get` returns presence; a NULL out pointer means
            // the caller only wants the answer (membership tests).
            auto probe = [&](llvm::Value* k, bool wantValue) -> llvm::Value* {
                llvm::Value* outSlot = wantValue
                    ? b_->CreateAlloca(vTy, nullptr, "dict.out") : nullptr;
                if (outSlot) // a miss must yield the absent value, not garbage
                    b_->CreateStore(llvm::Constant::getNullValue(vTy), outSlot);
                llvm::Value* hit = b_->CreateCall(
                    declareExternalSig("suki_dict_get",
                        { llvm::PointerType::getUnqual(cty), i64, i64,
                          kPtrTy(), vPtrTy() }, i32),
                    { box, llvm::ConstantInt::get(i64, ksz),
                      llvm::ConstantInt::get(i64, vsz), k,
                      wantValue ? outSlot
                                : llvm::ConstantPointerNull::get(vPtrTy()) });
                if (wantValue) return b_->CreateLoad(vTy, outSlot);
                return hit;
            };

            if (name == "set" || name == "update" || name == "insert") {
                llvm::Value* k = boxOf(argAt(0), kTy);
                if (!k) return nullptr;
                llvm::Value* v = (vsz > 0) ? boxOf(argAt(1), vTy) : nullptr;
                if (vsz > 0 && !v) return nullptr;
                if (name == "insert") {
                    // `insert` answers whether the element was new, so probe
                    // before writing.
                    llvm::Value* existed = probe(k, false);
                    b_->CreateCall(
                        declareExternalSig("suki_dict_set",
                            { llvm::PointerType::getUnqual(cty), i64, i64,
                              kPtrTy(), vPtrTy() }),
                        { box, llvm::ConstantInt::get(i64, ksz),
                          llvm::ConstantInt::get(i64, vsz), k,
                          v ? v : llvm::ConstantPointerNull::get(vPtrTy()) });
                    return b_->CreateICmpEQ(
                        existed, llvm::ConstantInt::get(i32, 0));
                }
                b_->CreateCall(
                    declareExternalSig("suki_dict_set",
                        { llvm::PointerType::getUnqual(cty), i64, i64,
                          kPtrTy(), vPtrTy() }),
                    { box, llvm::ConstantInt::get(i64, ksz),
                      llvm::ConstantInt::get(i64, vsz), k,
                      v ? v : llvm::ConstantPointerNull::get(vPtrTy()) });
                return nullptr; // `set`/`update` are Void
            }
            if (name == "remove" || name == "removeValue") {
                llvm::Value* k = boxOf(argAt(0), kTy);
                if (!k) return nullptr;
                return b_->CreateCall(
                    declareExternalSig("suki_dict_remove",
                        { llvm::PointerType::getUnqual(cty), i64, i64, kPtrTy() },
                        i32),
                    { box, llvm::ConstantInt::get(i64, ksz),
                      llvm::ConstantInt::get(i64, vsz), k });
            }
            if (name == "get") {
                llvm::Value* k = boxOf(argAt(0), kTy);
                if (!k) return nullptr;
                if (vsz <= 0) return probe(k, false); // Set membership
                return probe(k, true);
            }
            if (name == "containsKey" || name == "contains") {
                llvm::Value* k = boxOf(argAt(0), kTy);
                if (!k) return nullptr;
                return b_->CreateICmpNE(probe(k, false),
                                         llvm::ConstantInt::get(i32, 0));
            }
        }
        return nullptr;
    }

    // The type a generic argument is inferred as. A container parameter is
    // matched by its element: `T` in `[T]` binds to the element, not the array.
    // Mirrors Sema::inferTypeArgument so both sides name an instance the same.
    const Type* inferTypeArgument(const Type* t) {
        if (!t) return t;
        switch (t->kind) {
            case TypeKind::Array: case TypeKind::Optional:
            case TypeKind::Set:
                return t->element ? t->element : t;
            case TypeKind::Dict:
                return t->key ? t->key : t;
            default: return t;
        }
    }

    // Distinguishes the basic blocks of nested conditional expressions.
    unsigned condCounter_ = 0;
    // Merge block and PHI of the conditional currently being generated, so a
    // nested one contributes to the same PHI instead of making its own.
    llvm::BasicBlock* condMerge_ = nullptr;
    llvm::PHINode* condPhi_ = nullptr;

    // Byte size of a value of `ty`, as the runtime sees it. Aggregates have no
    // primitive bit width, so their size comes from the data layout; anything
    // still unknown falls back to one byte so the runtime stays safe.
    int64_t sizeOf(llvm::Type* ty) {
        if (!ty) return 1;
        if (unsigned bits = ty->getPrimitiveSizeInBits()) return (bits + 7) / 8;
        const llvm::DataLayout& dl = module_->getDataLayout();
        llvm::TypeSize size = dl.getTypeAllocSize(ty);
        return size.isScalable() || size.getFixedValue() == 0
            ? 1 : (int64_t)size.getFixedValue();
    }

    // Fit already-generated arguments to a callee's signature. `nil` carries no
    // type of its own, so it becomes a zeroed Optional of the parameter type;
    // conversely a plain value passed where a `T?` is expected is wrapped as
    // `.some(value)` (spec 2.7). Placeholders that could not be resolved are
    // dropped so the call keeps the arity the callee declares.
    void adaptArgs(std::vector<llvm::Value*>& args,
                   const std::vector<bool>& isNilArg,
                   llvm::FunctionType* fty) {
        if (!fty || fty->isVarArg()) {
            std::vector<llvm::Value*> kept;
            for (llvm::Value* a : args) if (a) kept.push_back(a);
            args = kept;
            return;
        }
        std::vector<llvm::Value*> out;
        out.reserve(args.size());
        for (size_t i = 0; i < args.size(); ++i) {
            if (i >= fty->getNumParams()) { if (args[i]) out.push_back(args[i]); continue; }
            llvm::Type* want = fty->getParamType(i);
            if (!args[i]) {
                if (i < isNilArg.size() && isNilArg[i])
                    out.push_back(llvm::Constant::getNullValue(want));
                continue;
            }
            out.push_back(wrapIntoOptional(args[i], want));
        }
        args = std::move(out);
    }

    // A constant String value `{ ptr, length }` for `text`.
    llvm::Value* stringConstant(const std::string& text) {
        llvm::Constant* data = b_->CreateGlobalString(text);
        llvm::Value* s = llvm::UndefValue::get(layout_->stringTy());
        s = b_->CreateInsertValue(s, data, {0});
        s = b_->CreateInsertValue(
            s, llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_),
                                      static_cast<uint64_t>(text.size())), {1});
        return s;
    }

    static bool isFloatKind(TypeKind k) {
        return k == TypeKind::Float16 || k == TypeKind::Float32 ||
               k == TypeKind::Float64 || k == TypeKind::Float128;
    }
    static bool isIntegerKind(TypeKind k) {
        switch (k) {
            case TypeKind::Int: case TypeKind::UInt:
            case TypeKind::Int8: case TypeKind::Int16:
            case TypeKind::Int32: case TypeKind::Int64:
            case TypeKind::UInt8: case TypeKind::UInt16:
            case TypeKind::UInt32: case TypeKind::UInt64:
            case TypeKind::ISize: case TypeKind::USize:
                return true;
            default: return false;
        }
    }

    // `print` / `println` write to standard output and are thread-safe
    // (spec 12.1). A String is written as-is; every other printable value is
    // formatted through the runtime first, which is what makes `print(i)` work
    // for an Int rather than demanding manual conversion at each call site.
    llvm::Value* genPrint(CallExpr* e, bool newline) {
        if (e->arguments.empty()) return nullptr;
        Node* arg = e->arguments[0].get();
        if (!arg) return nullptr;
        llvm::Value* v = genExpr(arg);
        if (!v) return nullptr;
        llvm::Type* sty = layout_->stringTy();
        const Type* t = arg->semaType;
        TypeKind k = t ? t->kind : TypeKind::Unknown;
        llvm::Type* vt = v->getType();
        llvm::Value* str = nullptr;
        if (k == TypeKind::String) {
            str = coerce(v, sty);
        } else if (k == TypeKind::Bool || (!t && vt->isIntegerTy(1))) {
            str = b_->CreateSelect(coerce(v, llvm::Type::getInt1Ty(*ctx_)),
                                   stringConstant("true"),
                                   stringConstant("false"));
        } else if (k == TypeKind::Char) {
            str = coerceForCast(v, sty, t); // suki_char_to_string
        } else if (isFloatKind(k) || vt->isFloatingPointTy()) {
            llvm::Function* f = declareExternalSig(
                "suki_double_to_string", { llvm::Type::getDoubleTy(*ctx_) }, sty);
            str = b_->CreateCall(f, { coerce(v, llvm::Type::getDoubleTy(*ctx_)) });
        } else if (isIntegerKind(k) || vt->isIntegerTy()) {
            // Without an annotation the width decides: any integer is formatted
            // as a signed decimal, which keeps `print(i)` working for a loop
            // variable whose semantic type is still unknown.
            llvm::Function* f = declareExternalSig(
                "suki_int_to_string", { llvm::Type::getInt64Ty(*ctx_) }, sty);
            str = b_->CreateCall(f, { coerce(v, llvm::Type::getInt64Ty(*ctx_)) });
        } else {
            return nullptr; // not printable: fall back to the generic path
        }
        llvm::Function* pf = declareExternalSig(
            newline ? "suki_println_str" : "suki_print_str", { sty });
        return b_->CreateCall(pf, { str });
    }

    // Arithmetic helpers on a floating-point receiver (规范 12.1).
    llvm::Value* genFloatMethod(const std::string& name, llvm::Value* v) {
        llvm::Type* ty = v->getType();
        if (!ty->isFloatingPointTy()) return nullptr;
        if (name == "squared") return b_->CreateFMul(v, v, "squared");
        if (name == "abs") {
            llvm::Function* f = llvm::Intrinsic::getDeclaration(
                module_.get(), llvm::Intrinsic::fabs, { ty });
            return b_->CreateCall(f, { v });
        }
        if (name == "squareRoot") {
            // `llvm.sqrt.*` lowers to the target's square-root instruction (or
            // the libm call where the hardware has none), so no external symbol
            // has to be linked by hand.
            llvm::Function* f = llvm::Intrinsic::getDeclaration(
                module_.get(), llvm::Intrinsic::sqrt, { ty });
            return b_->CreateCall(f, { v });
        }
        return nullptr;
    }

    // ── error handling ─────────────────────────────────────────────────────
    // Error propagation ABI (规范 9):
    //
    //   * a `throws` function takes one extra trailing parameter, the *error
    //     slot* (`i8**`). It stores null on entry and the error object on
    //     `throw`, so the caller can tell the two apart just by loading it;
    //   * the error itself is a heap cell `{ typeName, tag, payload }`. The tag
    //     and payload mirror the enum's tagged union, and `typeName` is what
    //     `catch … as E` compares — no runtime type information is needed;
    //   * at a call site the slot is loaded straight away: an error branches to
    //     the enclosing `catch` dispatcher, or returns to propagate.
    //
    // This keeps the return value untouched, so a throwing function's result is
    // used exactly like a non-throwing one.
    llvm::StructType* errorTy() {
        if (llvm::StructType* st = llvm::StructType::getTypeByName(*ctx_, "SukiError"))
            return st;
        llvm::StructType* st = llvm::StructType::create(*ctx_, "SukiError");
        st->setBody({ llvm::PointerType::get(*ctx_, 0),   // type name (C string)
                      llvm::Type::getInt64Ty(*ctx_),      // enum case tag
                      llvm::PointerType::get(*ctx_, 0) });// payload
        return st;
    }

    // True for the `{ i64 tag, i8* payload }` shape a payload-carrying enum
    // lowers to.
    static bool isTaggedUnion(llvm::Type* ty) {
        auto* st = llvm::dyn_cast_or_null<llvm::StructType>(ty);
        return st && st->getNumElements() == 2 &&
               st->getElementType(0)->isIntegerTy(64) &&
               st->getElementType(1)->isPointerTy();
    }

    // 错误槽分配已迁移至 ExceptionLowerer，此处为转发桩。
    llvm::Value* errorSlotForCall() { return exc_.errorSlotForCall(); }

    // 抛出调用后处理已迁移至 ExceptionLowerer，此处为转发桩。
    void finishThrowingCall(llvm::Value* slot) { exc_.finishThrowingCall(slot); }

    // Emit a call, appending the error slot when the callee throws and
    // branching on the outcome. `isNilArg` lets `nil` placeholders and plain
    // values for `T?` parameters be resolved against the callee's signature
    // (see adaptArgs / wrapIntoOptional).
    llvm::Value* emitCall(llvm::Function* callee, std::vector<llvm::Value*>& args,
                          bool throws, const std::vector<bool>& isNilArg = {}) {
        llvm::Value* slot = nullptr;
        if (throws) {
            slot = errorSlotForCall();
            args.push_back(slot);
        }
        llvm::FunctionType* fty = callee->getFunctionType();
        adaptArgs(args, isNilArg, fty);
        if (!fty->isVarArg()) {
            for (size_t i = 0; i < args.size() && i < fty->getNumParams(); ++i)
                args[i] = coerce(args[i], fty->getParamType(i));
        }
        llvm::Value* r = b_->CreateCall(callee, args);
        if (throws) finishThrowingCall(slot);
        return r;
    }

    // throw lowering 已迁移至 ExceptionLowerer，此处为转发桩。
    llvm::Value* genThrow(ThrowStmt* t) { return exc_.genThrow(t); }

    // do/catch lowering 已迁移至 ExceptionLowerer，此处为转发桩。
    void genDoStmt(DoStmt* d) { exc_.genDoStmt(d); }

    // ── closures ───────────────────────────────────────────────────────────
    // A closure value is the `{ captures, fnptr }` pair produced by
    // `emitClosure`, recognised by its identified struct name.
    static bool isClosureValue(llvm::Value* v) {
        auto* st = llvm::dyn_cast_or_null<llvm::StructType>(v ? v->getType() : nullptr);
        return st && st->hasName() && st->getName() == "SukiClosure";
    }

    // Lower a closure literal (spec 3.3).
    //
    // The body is emitted as an ordinary function whose *first* parameter is
    // the capture context, so a call needs no special ABI: it is an indirect
    // call through the stored pointer with the context passed along. Every
    // captured binding is stored as a pointer to its slot, which makes the
    // closure and the enclosing scope observe the same variable (a capture sees
    // later writes, and writes inside the closure are visible outside).
    llvm::Value* emitClosure(ClosureExpr* cl) {
        llvm::PointerType* i8p = llvm::PointerType::get(*ctx_, 0);
        llvm::Type* i64 = llvm::Type::getInt64Ty(*ctx_);
        std::vector<llvm::Type*> paramTys;
        for (auto& p : cl->params)
            paramTys.push_back(p.semaType ? layout_->lower(p.semaType) : i64);
        llvm::Type* retTy = llvm::Type::getVoidTy(*ctx_);
        if (cl->returnType)
            retTy = lowerDeclType(cl->returnType->semaType, cl->returnType.get());
        else if (cl->semaType && cl->semaType->kind == TypeKind::Closure &&
                 cl->semaType->ret && cl->semaType->ret->kind != TypeKind::Unknown)
            retTy = layout_->lower(cl->semaType->ret);

        // ── capture context ──
        // A captured entry is the *slot* of the variable, plus whether it is
        // captured weakly: a weak capture registers a side-table slot instead of
        // keeping the object alive (规范 6.1).
        struct Cap {
            std::string name;
            llvm::Value* slot;
            bool weak;
        };
        std::vector<Cap> caps;
        if (!cl->captures.empty()) {
            // An explicit capture list names exactly what is captured.
            for (const auto& c : cl->captures) {
                auto it = locals_.find(c.name);
                if (it != locals_.end())
                    caps.push_back({ c.name, it->second,
                                     c.mode == ClosureCapture::Mode::Weak });
            }
        } else {
            // Implicit capture: everything the closure body can see. Being
            // generous here is what keeps a captured variable correct; the
            // analyser has already rejected references to unknown names.
            for (auto& kv : locals_)
                caps.push_back({ kv.first, kv.second, false });
        }

        // `@escaping` (规范 3.3) means the closure may outlive the frame that
        // created it, so its context has to live on the heap.
        const bool escaping = hasAttr(cl, "escaping");
        llvm::Value* box = nullptr;
        if (caps.empty()) {
            box = llvm::ConstantPointerNull::get(i8p);
        } else if (escaping || inGlobalInit_) {
            llvm::Function* alloc = declareExternalSig("suki_alloc", { i64 }, i8p);
            box = b_->CreateCall(alloc, {
                llvm::ConstantInt::get(i64, static_cast<uint64_t>(caps.size()) * 8) });
        } else {
            box = b_->CreateAlloca(llvm::ArrayType::get(i8p, caps.size()),
                                   nullptr, "caps");
        }
        for (size_t i = 0; i < caps.size(); ++i) {
            llvm::Value* dst = b_->CreateGEP(i8p, box,
                                             { llvm::ConstantInt::get(i64, (uint64_t)i) });
            if (!caps[i].weak) {
                b_->CreateStore(b_->CreatePointerCast(caps[i].slot, i8p), dst);
                continue;
            }
            // A weak capture stores the address of a side-table cell: the
            // runtime nils that cell when the object dies, so the closure sees
            // nil instead of a dangling reference.
            llvm::Function* mk = declareExternalSig("suki_alloc", { i64 }, i8p);
            llvm::Value* cell = b_->CreateCall(
                mk, { llvm::ConstantInt::get(i64, 8) }, "weak.cell");
            llvm::Value* obj = b_->CreateLoad(i8p, caps[i].slot, "weak.obj");
            llvm::Function* reg = declareExternalSig(
                "suki_weak_register", { i8p, i8p }, llvm::Type::getVoidTy(*ctx_));
            b_->CreateCall(reg, { obj, cell });
            b_->CreateStore(cell, dst);
        }

        // ── the body function ──
        std::vector<llvm::Type*> sig{ i8p };
        for (llvm::Type* t : paramTys) sig.push_back(t);
        llvm::FunctionType* fty = llvm::FunctionType::get(retTy, sig, false);
        llvm::Function* thunk = llvm::Function::Create(
            fty, llvm::GlobalValue::InternalLinkage,
            "closure." + std::to_string(closureCounter_++), module_.get());

        // The closure body is emitted in its own function, so every piece of
        // per-function state has to be saved and restored around it.
        llvm::BasicBlock* savedBB = b_->GetInsertBlock();
        std::map<std::string, llvm::Value*> savedLocals = locals_;
        std::vector<ScopedRef> savedRefs = scopeRefs_;
        std::vector<size_t> savedMarks = scopeMarks_;
        size_t savedMark = currentScopeMark_;
        llvm::Type* savedRet = currentRet_;
        bool savedIsMain = isMain_;

        b_->SetInsertPoint(llvm::BasicBlock::Create(*ctx_, "entry", thunk));
        locals_.clear();
        inoutLocals_.clear();
        scopeRefs_.clear();
        scopeMarks_.clear();
        currentScopeMark_ = 0;
        isMain_ = false;
        currentRet_ = retTy;

        llvm::Value* ctxArg = thunk->getArg(0);
        for (size_t i = 0; i < caps.size(); ++i) {
            llvm::Value* slot = b_->CreateLoad(
                i8p, b_->CreateGEP(i8p, ctxArg, { llvm::ConstantInt::get(i64, (uint64_t)i) }),
                "cap.slot");
            if (caps[i].weak) {
                // The context holds the side-table cell itself; reading through
                // it yields nil once the object is gone.
                locals_[caps[i].name] = slot;
                continue;
            }
            locals_[caps[i].name] =
                b_->CreatePointerCast(slot, caps[i].slot->getType());
        }
        for (size_t i = 0; i < cl->params.size(); ++i) {
            const std::string& nm = cl->params[i].internalName.empty()
                                        ? cl->params[i].externalName
                                        : cl->params[i].internalName;
            llvm::Value* slot = b_->CreateAlloca(paramTys[i], nullptr, nm);
            if (i + 1 < thunk->arg_size()) b_->CreateStore(thunk->getArg(i + 1), slot);
            locals_[nm] = slot;
        }
        // A closure whose body ends in a bare expression returns that
        // expression (规范 3.3), so the last statement becomes the `ret`.
        const bool implicitReturn = !cl->body.empty() && !retTy->isVoidTy() &&
                                    cl->body.back()->kind == NodeKind::ExprStmt;
        for (size_t i = 0; i < cl->body.size(); ++i) {
            if (implicitReturn && i + 1 == cl->body.size()) {
                llvm::Value* v = genExpr(
                    static_cast<ExprStmt*>(cl->body[i].get())->expr.get());
                b_->CreateRet(v ? coerce(v, retTy)
                                : llvm::Constant::getNullValue(retTy));
                break;
            }
            genStmt(cl->body[i].get());
        }
        if (!b_->GetInsertBlock()->getTerminator()) {
            if (retTy->isVoidTy()) b_->CreateRetVoid();
            else b_->CreateRet(llvm::Constant::getNullValue(retTy));
        }

        locals_ = std::move(savedLocals);
        scopeRefs_ = std::move(savedRefs);
        scopeMarks_ = std::move(savedMarks);
        currentScopeMark_ = savedMark;
        currentRet_ = savedRet;
        isMain_ = savedIsMain;
        b_->SetInsertPoint(savedBB);

        llvm::Value* cv = llvm::UndefValue::get(layout_->closureTy(fty));
        cv = b_->CreateInsertValue(cv, box, {0});
        cv = b_->CreateInsertValue(cv, b_->CreatePointerCast(thunk, i8p), {1});
        return cv;
    }

    // Call a closure value: load the function pointer out of the pair and pass
    // the capture context as the first argument.
    llvm::Value* genClosureCall(llvm::Value* cv, CallExpr* e, const Type* ct) {
        llvm::PointerType* i8p = llvm::PointerType::get(*ctx_, 0);
        std::vector<llvm::Value*> args;
        std::vector<bool> isNilArg;
        for (auto& a : e->arguments) {
            isNilArg.push_back(a && a->kind == NodeKind::NilLitExpr);
            if (a && a->kind == NodeKind::NilLitExpr) args.push_back(nullptr);
            else if (llvm::Value* v = genExpr(a.get())) args.push_back(v);
        }
        llvm::Type* retTy = llvm::Type::getVoidTy(*ctx_);
        std::vector<llvm::Type*> pTys{ i8p };
        if (ct && (ct->kind == TypeKind::Closure || ct->kind == TypeKind::Function)) {
            for (const Type* p : ct->elements) pTys.push_back(layout_->lower(p));
            if (ct->ret && ct->ret->kind != TypeKind::Unknown) retTy = layout_->lower(ct->ret);
        } else {
            for (llvm::Value* a : args) pTys.push_back(a ? a->getType() : i8p);
        }
        llvm::FunctionType* fty = llvm::FunctionType::get(retTy, pTys, false);
        adaptArgs(args, isNilArg, fty);
        // adaptArgs drops the context parameter from its view, so shift by one.
        for (size_t i = 0; i + 1 < pTys.size() && i < args.size(); ++i)
            args[i] = coerce(args[i], pTys[i + 1]);

        llvm::Value* fn = b_->CreateExtractValue(cv, {1}, "closure.fn");
        llvm::Value* ctxv = b_->CreateExtractValue(cv, {0}, "closure.ctx");
        llvm::Value* fp = b_->CreatePointerCast(fn, fty->getPointerTo());
        std::vector<llvm::Value*> callArgs{ ctxv };
        for (llvm::Value* a : args) callArgs.push_back(a);
        llvm::CallInst* call = b_->CreateCall(fty, fp, callArgs);
        // @convention(c|stdcall)：被调用方为带显式调用约定的函数指针时，间接
        // 调用指令必须携带匹配的 LLVM calling convention（规范 §6.3）。
        if (ct && ct->kind == TypeKind::Function) {
            if (ct->callConv == CallConv::C)
                call->setCallingConv(llvm::CallingConv::C);
            else if (ct->callConv == CallConv::StdCall)
                call->setCallingConv(llvm::CallingConv::X86_StdCall);
        }
        return call;
    }

    // ─── async / Future lowering (real threads) ────────────────────────────────
    // An async call spawns a genuine OS thread: the arguments together with the
    // Future handle are copied into one heap context block, the worker runs the
    // function body on that thread, stores its result into the Future and signals
    // completion. `await` blocks on the Future's condition variable, copies the
    // value out and releases the handle. Threading lives entirely in the C runtime
    // (pthreads / Win32), so this lowering is platform-neutral. If a thread cannot
    // be started the work runs inline, so a program always makes progress.

    // Synchronous runtime builtins with fixed C prototypes, intercepted by name so
    // the exact signature (pointer first argument, etc.) is emitted.
    // 同步运行时内建 lowering 已迁移至 ConcurrencyLowerer，此处为转发桩。
    llvm::Value* genSyncBuiltin(const std::string& name, CallExpr* e) {
        return conc_.genSyncBuiltin(name, e);
    }

    // 并发运行时入口 lowering 已迁移至 ConcurrencyLowerer，此处为转发桩。
    llvm::Function* asyncAllocFn() { return conc_.asyncAllocFn(); }
    llvm::Function* asyncFreeFn() { return conc_.asyncFreeFn(); }
    llvm::Function* asyncFutureCreateFn() { return conc_.asyncFutureCreateFn(); }

    // Emit `<sym>_spawn` for an async function. The worker `sym_tramp` runs the
    // synchronous body on a fresh thread and publishes its result through the
    // Future; the spawn builds the context block and starts the worker.
    // async spawn lowering 已迁移至 ConcurrencyLowerer，此处为转发桩。
    void genAsyncSpawn(FunctionDecl* fn, const std::string& sym) {
        conc_.genAsyncSpawn(fn, sym);
    }

    // ─── Channel<T> ─────────────────────────────────────────────────────────
    // `Channel<Int>(capacity: n)` allocates the runtime FIFO with the element size
    // of `Int`, so one runtime implementation serves every element type.
    // Channel lowering 已迁移至嵌套组件 ConcurrencyLowerer，此处为转发桩。
    llvm::Value* genChannelInit(const Type* instTy, CallExpr* e) {
        return conc_.genChannelInit(instTy, e);
    }

    // Channel methods are runtime-backed, so their bodies call the runtime
    // directly instead of lowering a SukiCode body (which does not exist).
    void genChannelMethodBody(const std::string& method, const Type* instTy,
                              llvm::Function* f) {
        if (!f || !f->empty()) return;
        llvm::Type* i64 = llvm::Type::getInt64Ty(*ctx_);
        llvm::PointerType* i8p = llvm::PointerType::get(*ctx_, 0);
        llvm::Type* voidTy = llvm::Type::getVoidTy(*ctx_);
        // A Named type keeps its generic arguments in `elements`.
        const Type* elemTy = (!instTy->elements.empty())
                                 ? instTy->elements[0] : nullptr;
        llvm::Type* elemLLVM = elemTy ? layout_->lower(elemTy) : i64;

        llvm::Type* selfTy = f->getFunctionType()->getParamType(0);
        b_->SetInsertPoint(llvm::BasicBlock::Create(*ctx_, "entry", f));
        llvm::Value* selfSlot = b_->CreateAlloca(selfTy, nullptr, "self");
        b_->CreateStore(f->getArg(0), selfSlot);
        llvm::Value* hp = b_->CreateStructGEP(selfTy, selfSlot, 0, "handle.addr");
        llvm::Value* handle = b_->CreateLoad(i8p, hp, "handle");

        if (method == "send") {
            llvm::Value* vSlot = b_->CreateAlloca(elemLLVM, nullptr, "value");
            b_->CreateStore(coerce(f->getArg(1), elemLLVM), vSlot);
            llvm::Function* sf = declareExternalSig("suki_channel_send",
                                                    { i8p, i8p }, voidTy);
            b_->CreateCall(sf, { handle, b_->CreateBitCast(vSlot, i8p) });
            b_->CreateRetVoid();
        } else if (method == "receive") {
            llvm::Value* out = b_->CreateAlloca(elemLLVM, nullptr, "out");
            llvm::Function* rf = declareExternalSig("suki_channel_receive",
                                                    { i8p, i8p },
                                                    llvm::Type::getInt32Ty(*ctx_));
            b_->CreateCall(rf, { handle, b_->CreateBitCast(out, i8p) });
            b_->CreateRet(b_->CreateLoad(elemLLVM, out));
        } else if (method == "close") {
            llvm::Function* cf = declareExternalSig("suki_channel_close",
                                                    { i8p }, voidTy);
            b_->CreateCall(cf, { handle });
            b_->CreateRetVoid();
        } else {
            b_->CreateRetVoid();
        }
    }

    // `select { case ... <- chan: ... default: ... }` (规范 7.5): try every case's
    // channel operation; the first that can proceed runs its body. If none can,
    // a `default` arm runs; without one the select yields and tries again, which
    // is what makes it wait for the first ready case.
    // select lowering 已迁移至 ConcurrencyLowerer，此处为转发桩。
    void genSelect(SwitchStmt* s) { conc_.genSelect(s); }

    // `for await x in seq` — iterate an async sequence to completion.
    //
    // Channel<T>: block-receive each element; the runtime returns -1 once the
    // channel is closed and drained, which ends the loop.
    //
    // TaskGroup<T>: drain the children's results in submission order; each child
    // future is awaited and its value copied out by the runtime.
    //
    // `break` targets the exit block and `continue` the step block (which re-tests
    // the head), exactly like a `while` loop.
    // for await lowering 已迁移至 ConcurrencyLowerer，此处为转发桩。
    void genForAwait(ForInStmt* fr) { conc_.genForAwait(fr); }

    // async/Channel/TaskGroup 运行时辅助已迁移至 ConcurrencyLowerer，此处为转发桩。
    llvm::Value* asyncCompletedFuture(llvm::Type* valueTy, llvm::Value* valuePtr) {
        return conc_.asyncCompletedFuture(valueTy, valuePtr);
    }

    // Channel 异步成员 lowering 已迁移至 ConcurrencyLowerer，此处为转发桩。
    void genChannelMethod(FunctionDecl* mf, const Type* instTy) {
        conc_.genChannelMethod(mf, instTy);
    }

    // Runtime-backed members of an instantiated `TaskGroup<T>`: `addTask` spawns a
    // child whose result is a T (stored in its Future), `waitForAll` joins them.
    void genTaskGroupMethod(FunctionDecl* mf, const Type* instTy) {
        const std::string mkey = std::string(instTy->name) + "." + mf->name;
        if (methodFns_.count(mkey)) return;
        llvm::PointerType* i8p = llvm::PointerType::get(*ctx_, 0);
        llvm::Type* voidTy = llvm::Type::getVoidTy(*ctx_);
        const Type* elemTy = (!instTy->elements.empty())
                                 ? instTy->elements[0] : nullptr;
        llvm::Type* elemLLVM = elemTy ? layout_->lower(elemTy) : nullptr;
        llvm::Type* selfTy = layout_->lower(instTy);
        if (!selfTy) return;

        if (mf->name == "waitForAll") {
            llvm::FunctionType* fty = llvm::FunctionType::get(voidTy, { selfTy },
                                                              false);
            llvm::Function* f = llvm::Function::Create(fty,
                llvm::GlobalValue::InternalLinkage, mkey, module_.get());
            b_->SetInsertPoint(llvm::BasicBlock::Create(*ctx_, "entry", f));
            llvm::Value* g = loadHandle(f->getArg(0), selfTy);
            b_->CreateCall(declareExternalSig("suki_taskgroup_wait_all", { i8p },
                                              voidTy), { g });
            b_->CreateRetVoid();
            methodFns_[mkey] = f;
            return;
        }
        if (mf->name != "addTask") return;

        llvm::StructType* clTy = llvm::cast<llvm::StructType>(
            layout_->closureTy(llvm::FunctionType::get(voidTy, { i8p }, false)));
        llvm::FunctionType* fty = llvm::FunctionType::get(voidTy,
                                                          { selfTy, clTy }, false);
        llvm::Function* f = llvm::Function::Create(fty,
            llvm::GlobalValue::InternalLinkage, mkey, module_.get());
        b_->SetInsertPoint(llvm::BasicBlock::Create(*ctx_, "entry", f));
        llvm::Value* g = loadHandle(f->getArg(0), selfTy);
        llvm::Value* fut = startClosureTask(f->getArg(1), f, elemLLVM);
        b_->CreateCall(declareExternalSig("suki_taskgroup_add", { i8p, i8p },
                                          voidTy), { g, fut });
        b_->CreateRetVoid();
        methodFns_[mkey] = f;
    }

    // ─── Task / TaskGroup ───────────────────────────────────────────────────
    // Both are `{ i8* handle }` wrappers around a runtime object. They are
    // registered by Sema rather than declared in source, so the struct type is
    // built directly here — keeping the constructors and the runtime-backed method
    // bodies in agreement.
    // handle / Task / TaskGroup 辅助已迁移至 ConcurrencyLowerer，此处为转发桩。
    llvm::StructType* handleStructTy(const std::string& name) { return conc_.handleStructTy(name); }
    llvm::Value* makeHandleValue(llvm::StructType* sty, llvm::Value* handle) {
        return conc_.makeHandleValue(sty, handle);
    }
    llvm::Value* loadHandle(llvm::Value* selfVal, llvm::Type* selfTy) {
        return conc_.loadHandle(selfVal, selfTy);
    }

    // closure 线程启动已迁移至 ConcurrencyLowerer，此处为转发桩。
    llvm::Value* startClosureTask(llvm::Value* cv, llvm::Function* cur,
                                  llvm::Type* resultTy) {
        return conc_.startClosureTask(cv, cur, resultTy);
    }

    // Task/TaskGroup 运行时辅助已迁移至 ConcurrencyLowerer，此处为转发桩。
    llvm::Value* genTaskInit(CallExpr* e) { return conc_.genTaskInit(e); }
    llvm::Value* genTaskGroupInit(const Type* elemTy) { return conc_.genTaskGroupInit(elemTy); }
    void genConcurrencyBuiltinMethod(const std::string& base, const std::string& mem,
                                     const Type* elemTy = nullptr) {
        conc_.genConcurrencyBuiltinMethod(base, mem, elemTy);
    }
    void genSleepSpawn() { conc_.genSleepSpawn(); }

    // withTaskGroup 运行时辅助已迁移至 ConcurrencyLowerer，此处为转发桩。
    void genWithTaskGroupSpawn() { conc_.genWithTaskGroupSpawn(); }

    llvm::Value* genCall(CallExpr* e) {
        if (!e->callee) return nullptr;
        // A local (or member) holding a closure is called indirectly rather
        // than through the function table.
        if (e->callee->kind == NodeKind::IdentExpr || e->callee->kind == NodeKind::MemberExpr) {
            if (llvm::Value* cv = genExpr(e->callee.get())) {
                if (isClosureValue(cv))
                    return genClosureCall(cv, e, e->callee->semaType);
            }
        }
        // Builtin methods on values that are not user-defined types.
        if (e->callee->kind == NodeKind::MemberExpr) {
            auto* bm = static_cast<MemberExpr*>(e->callee.get());
            const Type* bt = bm->base ? bm->base->semaType : nullptr;

            if (bt && isFloatKind(bt->kind)) {
                if (llvm::Value* recv = genExpr(bm->base.get()))
                    if (llvm::Value* r = genFloatMethod(bm->member, recv))
                        return r;
            }
            if (bt && isIntegerKind(bt->kind)) {
                if (llvm::Value* recv = genExpr(bm->base.get())) {
                    if (bm->member == "squared")
                        return b_->CreateMul(recv, recv, "squared");
                }
            }

            // super.init(...) / super.method(...): dispatch statically to the
            // superclass implementation while passing the current object.
            if (bm->base && bm->base->kind == NodeKind::IdentExpr &&
                static_cast<IdentExpr*>(bm->base.get())->name == "super") {
                if (llvm::Value* r = genSuperCall(e, bm->member)) return r;
                return nullptr;
            }

            if (bt && bt->kind == TypeKind::String) {
                if (llvm::Value* recv = genExpr(bm->base.get()))
                    if (llvm::Value* r = genStringMethod(e, bm->member, recv))
                        return r;
            }
            if (bt && (bt->kind == TypeKind::Array || bt->kind == TypeKind::Set ||
                       bt->kind == TypeKind::Dict)) {
                if (llvm::Value* recv = genExpr(bm->base.get())) {
                    // A named receiver keeps a stable alloca; handing it to the
                    // callee lets `set`/`insert`/`remove` mutate in place.
                    llvm::Value* slot = nullptr;
                    if (bm->base->kind == NodeKind::IdentExpr) {
                        auto lit = locals_.find(
                            static_cast<IdentExpr*>(bm->base.get())->name);
                        if (lit != locals_.end()) slot = lit->second;
                    }
                    if (llvm::Value* r = genCollectionMethod(
                            e, bm->member, recv, bt, slot))
                        return r;
                }
            }
        }
        // Method call: `receiver.method(args)` on a value type.
        if (e->callee->kind == NodeKind::MemberExpr) {
            auto* m = static_cast<MemberExpr*>(e->callee.get());
            const Type* bt = m->base ? m->base->semaType : nullptr;
            // Resolve the owning type of the member call. The base may be a value
            // of the type (instance call, resolved normally) or the type name
            // itself — a `Metatype` — which is how `Type.staticMethod()` is written.
            const Type* ownerRecord = nullptr;
            if (bt && bt->kind == TypeKind::Named && bt->record) {
                ownerRecord = bt;
            } else if (bt && bt->kind == TypeKind::Metatype && bt->element &&
                       bt->element->kind == TypeKind::Named && bt->element->record) {
                ownerRecord = bt->element;
            }
            if (ownerRecord) {
                // addTask 的子结果类型 T 由闭包实参返回类型推断（裸 TaskGroup 无
                // elements）。该 T 必须并入查找/存储 key，否则生成后查不到而无法调用。
                const Type* egElemTy = nullptr;
                if (ownerRecord->name == "TaskGroup" && m->member == "addTask" &&
                    !e->arguments.empty() && e->arguments[0] &&
                    e->arguments[0]->semaType) {
                    const TypeKind k = e->arguments[0]->semaType->kind;
                    if (k == TypeKind::Function || k == TypeKind::Closure)
                        egElemTy = e->arguments[0]->semaType->ret;
                }
                std::string mkey = ownerRecord->name + "." + m->member;
                if (egElemTy) mkey += "/" + (egElemTy->name.empty() ? std::string("?") : egElemTy->name);
                auto it = methodFns_.find(mkey);
                // Runtime-backed concurrency members of the base Task / TaskGroup
                // are emitted on first use, so programmes that never touch them
                // ship no such machinery. The generator moves the IR builder into
                // the new function, so restore the caller's insertion point.
                if (it == methodFns_.end() &&
                    (ownerRecord->name == "Task" || ownerRecord->name == "TaskGroup")) {
                    llvm::BasicBlock* savedIP = b_->GetInsertBlock();
                    genConcurrencyBuiltinMethod(ownerRecord->name, m->member, egElemTy);
                    if (savedIP) b_->SetInsertPoint(savedIP);
                    it = methodFns_.find(mkey);
                }
                if (it != methodFns_.end()) {
                    llvm::Function* mf = it->second;
                    llvm::FunctionType* fty = mf->getFunctionType();
                    // Static method call `Type.method(args)`: no receiver is passed.
                    if (staticMethods_.count(mkey)) {
                        std::vector<llvm::Value*> args;
                        std::vector<bool> isNilArg;
                        for (auto& a : e->arguments) {
                            isNilArg.push_back(a && a->kind == NodeKind::NilLitExpr);
                            args.push_back(genExpr(a.get()));
                        }
                        adaptArgs(args, isNilArg, fty);
                        for (size_t i = 0; i < args.size() && i < fty->getNumParams(); ++i)
                            args[i] = coerce(args[i], fty->getParamType(i));
                        const bool methodThrows =
                            methodThrows_.count(mkey) ? methodThrows_[mkey] : false;
                        llvm::Value* errSlot = nullptr;
                        if (methodThrows) { errSlot = errorSlotForCall(); args.push_back(errSlot); }
                        llvm::Value* result = nullptr;
                        if (methodThrows) {
                            for (size_t i = 0; i < args.size() && i < fty->getNumParams(); ++i)
                                args[i] = coerce(args[i], fty->getParamType(i));
                            result = b_->CreateCall(mf, args);
                            finishThrowingCall(errSlot);
                        } else {
                            result = b_->CreateCall(mf, args);
                        }
                        return result;
                    }
                    // How `self` is passed depends on the receiver:
                    //  - a `mutating` struct method takes the address of the
                    //    caller's copy, so writes are visible to the caller;
                    //  - a class/actor method receives the object pointer
                    //    itself (reference semantics: one instance, shared);
                    //  - any other value method receives a copy.
                    llvm::Value* recv = nullptr;
                    if (!fty->getNumParams()) return nullptr;
                    bool selfByPointer = fty->getParamType(0)->isPointerTy();
                    if (selfByPointer && !layout_->isReferenceType(bt))
                        recv = genAddr(m->base.get());
                    else
                        recv = genExpr(m->base.get());
                    if (!recv) return nullptr;
                    std::vector<llvm::Value*> args{recv};
                    std::vector<bool> isNilArg{false}; // `self` is never nil
                    for (auto& a : e->arguments) {
                        isNilArg.push_back(a && a->kind == NodeKind::NilLitExpr);
                        llvm::Value* v = genExpr(a.get());
                        args.push_back(v); // a nil placeholder is filled in below
                    }
                    adaptArgs(args, isNilArg, fty);
                    for (size_t i = 0; i < args.size() && i < fty->getNumParams(); ++i)
                        args[i] = coerce(args[i], fty->getParamType(i));

                    // A call on a class receiver is virtual: dispatch through the
                    // receiver's vtable so the override that runs is the one
                    // belonging to the object's dynamic type. `final` methods
                    // (and non-class receivers) keep the static call above, which
                    // is what lets an override extend rather than replace.
                    // Actor isolation (规范 7.4): a call into an actor crosses an
                    // isolation boundary, so it is asynchronous. 执行权由 actor 的
                    // **串行执行器队列**授予：suki_actor_enter 取票并等待轮到自己
                    // （FIFO，不会饥饿/乱序），suki_actor_leave 把执行权交给队列中
                    // 的下一个任务。这既保证了可变状态无数据竞争，也满足规范要求的
                    // 串行队列语义。
                    const bool isActorCall = bt->record &&
                        bt->record->kind == TypeDeclKind::Actor;
                    llvm::Value* actorLockPtr = nullptr;
                    if (isActorCall) {
                        llvm::StructType* ost = layout_->objectType(bt->record);
                        if (ost) {
                            actorLockPtr = b_->CreateStructGEP(ost, recv, 3,
                                                               "actor.lock");
                            llvm::PointerType* i64p = llvm::PointerType::getUnqual(
                                llvm::Type::getInt64Ty(*ctx_));
                            b_->CreateCall(declareExternalSig("suki_actor_enter",
                                { i64p }, llvm::Type::getVoidTy(*ctx_)),
                                { b_->CreateBitCast(actorLockPtr, i64p) });
                        }
                    }

                    const bool methodThrows =
                        methodThrows_.count(bt->name + "." + m->member)
                            ? methodThrows_[bt->name + "." + m->member] : false;
                    llvm::Value* errSlot = nullptr;
                    if (methodThrows) {
                        errSlot = errorSlotForCall();
                        args.push_back(errSlot);
                    }
                    llvm::Value* result = nullptr;
                    if (bt->record && layout_->isReferenceType(bt) &&
                        !nonVirtual_.count(bt->name + "." + m->member)) {
                        result = genVirtualCall(bt->record, m->member, recv, args);
                        if (result && methodThrows) finishThrowingCall(errSlot);
                    }
                    if (!result) {
                        if (methodThrows) {
                            llvm::FunctionType* fty = mf->getFunctionType();
                            for (size_t i = 0; i < args.size() &&
                                 i < fty->getNumParams(); ++i)
                                args[i] = coerce(args[i], fty->getParamType(i));
                            result = b_->CreateCall(mf, args);
                            finishThrowingCall(errSlot);
                        } else {
                            result = b_->CreateCall(mf, args);
                        }
                    }
                    if (isActorCall && actorLockPtr) {
                        llvm::PointerType* i64p = llvm::PointerType::getUnqual(
                            llvm::Type::getInt64Ty(*ctx_));
                        b_->CreateCall(declareExternalSig("suki_actor_leave",
                            { i64p }, llvm::Type::getVoidTy(*ctx_)),
                            { b_->CreateBitCast(actorLockPtr, i64p) });
                        // The call itself completed synchronously under the lock, so
                        // an already-complete Future carries its result to `await`.
                        if (!result || result->getType()->isVoidTy())
                            return asyncCompletedFuture(nullptr, nullptr);
                        llvm::Value* slot = b_->CreateAlloca(result->getType(),
                                                             nullptr,
                                                             "actor.result");
                        b_->CreateStore(result, slot);
                        return asyncCompletedFuture(result->getType(), slot);
                    }
                    return result;
                }
            }
            return nullptr;
        }
        // A constructor for a generic type (`Box<Int>(value:)`) names the type in a
        // NamedType node rather than an identifier. Resolve it to the monomorphised
        // instance so the initialiser builds that instance's concrete layout.
        if (e->callee && (e->callee->kind == NodeKind::NamedType ||
                          e->callee->kind == NodeKind::GenericExpr)) {
            const Type* ct = e->callee->semaType;
            std::string cname;
            if (ct && ct->kind == TypeKind::Named && ct->record)
                cname = ct->record->name;
            else if (e->callee->kind == NodeKind::NamedType)
                cname = static_cast<NamedType*>(e->callee.get())->name;
            if (!cname.empty()) {
                // A Channel is runtime-backed: allocating it creates the FIFO with
                // the element size of its type argument.
                if (cname.rfind("Channel<", 0) == 0) {
                    auto chit = structTypes_.find(cname);
                    if (chit != structTypes_.end())
                        return genChannelInit(chit->second, e);
                    if (ct) return genChannelInit(ct, e);
                    return nullptr;
                }
                // `TaskGroup<T>()` allocates a group that stores children results of
                // type T; the element size drives the runtime bookkeeping.
                if (ct && ct->kind == TypeKind::Named &&
                    ct->name.rfind("TaskGroup<", 0) == 0) {
                    auto tgit = structTypes_.find(ct->name);
                    if (tgit != structTypes_.end() && !tgit->second->elements.empty())
                        return genTaskGroupInit(tgit->second->elements[0]);
                    return genTaskGroupInit(nullptr);
                }
                auto gsit = structTypes_.find(cname);
                if (gsit != structTypes_.end()) return genStructInit(gsit->second, e);
                auto gcit = classTypes_.find(cname);
                if (gcit != classTypes_.end()) return genClassInit(gcit->second, e);
            }
            return nullptr;
        }
        if (e->callee->kind != NodeKind::IdentExpr) return nullptr;
        const std::string& n = static_cast<IdentExpr*>(e->callee.get())->name;
        // Synchronous runtime builtins with fixed C prototypes (atomicAdd / mutex*)
        // — intercepted by name so the exact signature is emitted rather than the
        // default variadic foreign declaration.
        if (n == "atomicAdd" || n == "mutexLock" || n == "mutexUnlock")
            return genSyncBuiltin(n, e);
        // `Task { ... }` runs its body on a new thread; `TaskGroup()` allocates the
        // group that will own its children.
        if (n == "Task" && !e->arguments.empty()) return genTaskInit(e);
        if (n == "TaskGroup" && e->arguments.empty()) return genTaskGroupInit(nullptr);
        if (n.rfind("TaskGroup<", 0) == 0) {
            auto tgit = structTypes_.find(n);
            if (tgit != structTypes_.end() && !tgit->second->elements.empty())
                return genTaskGroupInit(tgit->second->elements[0]);
            return genTaskGroupInit(nullptr);
        }
        // A call naming a struct type is a constructor invocation.
        auto sit = structTypes_.find(n);
        if (sit != structTypes_.end()) return genStructInit(sit->second, e);
        // A call naming a class type allocates an instance and runs `init`.
        auto cit = classTypes_.find(n);
        if (cit != classTypes_.end()) return genClassInit(cit->second, e);
        // `print` / `println` accept any printable value; a String's runtime
        // entry point receives the `{ i8*, i64 }` aggregate, and other scalars
        // are formatted first.
        if ((n == "print" || n == "println") && !e->arguments.empty() &&
            e->arguments[0] && e->arguments[0]->semaType) {
            if (llvm::Value* r = genPrint(e, n == "println")) return r;
        }
        std::vector<llvm::Value*> args;
        std::vector<bool> isNilArg;
        std::vector<std::string> argTypeNames;
        bool calleeIsAsync = false;
        FunctionDecl* fdecl = nullptr;
        auto fdit = fnDecls_.find(n);
        if (fdit != fnDecls_.end()) {
            // 默认参数 / 变长参数展开（规范 3.1）。
            fdecl = fdit->second;
            calleeIsAsync = fdecl->isAsync;
            std::vector<bool> provided(fdecl->params.size(), false);
            for (size_t i = 0; i < e->arguments.size(); ++i) {
                const std::string& lab = (i < e->argumentLabels.size())
                    ? e->argumentLabels[i] : std::string();
                int target = -1;
                if (!lab.empty()) {
                    for (size_t p = 0; p < fdecl->params.size(); ++p)
                        if (fdecl->params[p].externalName == lab) { target = (int)p; break; }
                    // 标签未匹配任何形参的外部名时，回退到位置匹配
                    //（规范 3.1：标签调用若缺失对应标签则按位置传参）。
                    if (target < 0 && i < fdecl->params.size()) target = (int)i;
                } else if (i < fdecl->params.size()) {
                    target = (int)i;
                }
                if (target < 0 || target >= (int)fdecl->params.size()) continue;
                if (fdecl->params[target].isVariadic) {
                    // 从该位置起的全部实参打包为数组（变长参数）。
                    std::vector<llvm::Value*> elems;
                    for (size_t j = i; j < e->arguments.size(); ++j)
                        if (llvm::Value* ev = genExpr(e->arguments[j].get()))
                            elems.push_back(ev);
                    args.push_back(genArrayFromElems(
                        fdecl->params[target].semaType, elems));
                    isNilArg.push_back(false);
                    argTypeNames.push_back("Array");
                    provided[target] = true;
                    break;
                }
                if (e->arguments[i] && e->arguments[i]->kind == NodeKind::NilLitExpr) {
                    isNilArg.push_back(true);
                    args.push_back(nullptr);
                } else {
                    isNilArg.push_back(false);
                    if (llvm::Value* v = genExpr(e->arguments[i].get())) args.push_back(v);
                }
                argTypeNames.push_back(
                    e->arguments[i]->semaType
                        ? typeToString(inferTypeArgument(e->arguments[i]->semaType))
                        : std::string());
                provided[target] = true;
            }
            // 补齐缺失形参：默认参数值，或空变长数组。
            for (size_t p = 0; p < fdecl->params.size(); ++p) {
                if (provided[p]) continue;
                if (fdecl->params[p].isVariadic) {
                    args.push_back(genArrayFromElems(
                        fdecl->params[p].semaType, {}));
                    isNilArg.push_back(false);
                    argTypeNames.push_back("Array");
                } else if (fdecl->params[p].defaultValue) {
                    isNilArg.push_back(false);
                    if (llvm::Value* v = genExpr(fdecl->params[p].defaultValue.get()))
                        args.push_back(v);
                    argTypeNames.push_back(
                        fdecl->params[p].defaultValue->semaType
                            ? typeToString(inferTypeArgument(
                                  fdecl->params[p].defaultValue->semaType))
                            : std::string());
                }
            }
        } else {
            for (auto& a : e->arguments) {
                isNilArg.push_back(a && a->kind == NodeKind::NilLitExpr);
                // `nil` has no type of its own: the value is built once the callee's
                // parameter type is known, so a placeholder stands in for now.
                if (a && a->kind == NodeKind::NilLitExpr) args.push_back(nullptr);
                else if (llvm::Value* v = genExpr(a.get())) args.push_back(v);
                argTypeNames.push_back(
                    a->semaType ? typeToString(inferTypeArgument(a->semaType))
                                : std::string());
            }
        }
        // A call to a generic function names its instantiation: the argument
        // types decide which `f<T1,T2>` to call. The symbol encodes the type
        // arguments, so an exact match on the printed types is what selects it.
        auto mit = monoNames_.find(n);
        if (mit != monoNames_.end() && !mit->second.empty()) {
            std::string want;
            if (!e->genericTypeArgs.empty()) {
                // Use the exact type arguments Sema resolved for this call. They are
                // identical to the strings the monomorphiser used to name the
                // instance, so the lookup is exact even for 2+ type parameters
                // (which the argument-type reconstruction below got wrong).
                want = monoSymbol(n, e->genericTypeArgs);
            } else {
                want = n + "<";
                for (size_t i = 0; i < argTypeNames.size(); ++i) {
                    if (i) want += ",";
                    want += argTypeNames[i];
                }
                want += ">";
            }
            auto fit = std::find(mit->second.begin(), mit->second.end(), want);
            if (fit == mit->second.end()) fit = mit->second.begin();
            llvm::Function* inst = fns_.count(*fit) ? fns_[*fit] : nullptr;
            if (inst) {
                return emitCall(inst, args,
                                fnThrows_.count(*fit) ? fnThrows_[*fit] : false,
                                isNilArg);
            }
        }
        // An async call returns a Future handle (i8*), not the value directly. Route
        // it through the generated `<n>_spawn` (or `sleep_spawn`) wrapper that runs
        // the body and boxes the result; `await` unboxes it.
        // Async builtins (`sleep`, `withTaskGroup`) are registered by Sema rather
        // than declared in source, so they are not in fnDecls_; name them here so
        // the call still resolves to their spawn.
        if (calleeIsAsync || n == "sleep" || n == "withTaskGroup") {
            std::string spawnSym;
            if (n == "sleep") spawnSym = "sleep_spawn";
            else if (n == "withTaskGroup") spawnSym = "withTaskGroup_spawn";
            else spawnSym = n + "_spawn";
            // Generate the worker spawn lazily — it only exists when the async
            // call is actually made, so an unused concurrency feature costs
            // nothing in .text and pulls in no runtime symbols. The generators
            // move the IR builder into the new function, so restore the caller's
            // insertion point afterwards.
            if (fns_.find(spawnSym) == fns_.end()) {
                llvm::BasicBlock* savedIP = b_->GetInsertBlock();
                if (n == "sleep") genSleepSpawn();
                else if (n == "withTaskGroup") genWithTaskGroupSpawn();
                else if (calleeIsAsync && fdecl) genAsyncSpawn(fdecl, n);
                if (savedIP) b_->SetInsertPoint(savedIP);
            }
            auto sit = fns_.find(spawnSym);
            if (sit == fns_.end() || !sit->second) return nullptr;
            std::vector<llvm::Value*> callArgs;
            for (size_t i = 0; i < args.size() && i < sit->second->arg_size(); ++i)
                callArgs.push_back(coerce(args[i],
                    sit->second->getFunctionType()->getParamType(i)));
            return b_->CreateCall(sit->second, callArgs);
        }
        auto it = fns_.find(n);
        llvm::Function* callee = (it != fns_.end()) ? it->second : declareExternal(n);
        return emitCall(callee, args, fnThrows_.count(n) ? fnThrows_[n] : false,
                        isNilArg);
    }

public:
        bool generate(const NodeList& decls, std::string& errOut) {
        registerTypes(decls);
        generateBodies(decls);
        std::string verifyErr;
        llvm::raw_string_ostream os(verifyErr);
        if (llvm::verifyModule(*module_, &os)) {
            // Include the module so the offending function is visible even
            // though the module is rejected.
            errOut = "IR verification failed: " + os.str() + "\n" + irText();
            return false;
        }
        return true;
    }

    private:
    // Phase 1: register every type, function, method, generic instance and
    // global *before* any body is generated. Generic type/function instances
    // are emitted eagerly here because their bodies need bound type params.
    void registerTypes(const NodeList& decls) {
        // The data layout must be established first: TypeLayout relies on it to
        // compute ABI padding for struct/class field offsets.
        std::string err;
        if (auto* T = llvm::TargetRegistry::lookupTarget(target_.triple, err)) {
            std::unique_ptr<llvm::TargetMachine> tm(T->createTargetMachine(
                target_.triple, "generic", "", llvm::TargetOptions(), std::nullopt,
                std::nullopt, llvm::CodeGenOptLevel::None));
            if (tm) module_->setDataLayout(tm->createDataLayout());
        }
        layout_ = std::make_unique<TypeLayout>(*ctx_, target_, &module_->getDataLayout());

        // Pre-register struct declarations so `%T` bodies exist before any
        // function that mentions them is generated (and so constructors work).
        // Pre-register enum declarations so `Enum.case` tags can be resolved
        // by name (a bare type name has no useful semantic type of its own).
        for (auto& d : decls) {
            if (!d || d->kind != NodeKind::EnumDecl) continue;
            auto* td = static_cast<TypeDecl*>(d.get());
            if (td->semaType) {
                enumTypes_[td->name] = td->semaType;
                layout_->lower(td->semaType);
            }
        }

        // Actors are reference types like classes (ARC-managed, vtable, init), so
        // they take the same path — the only difference is the isolation lock in
        // the object header and the Future returned by their methods.
        for (auto& d : decls) {
            if (!d || (d->kind != NodeKind::ClassDecl &&
                       d->kind != NodeKind::ActorDecl)) continue;
            auto* td = static_cast<TypeDecl*>(d.get());
            if (td->semaType) {
                // The calling convention is needed before declaring accessors.
                if (td->semaType->record)
                    for (const auto& mem : td->semaType->record->members)
                        if (mem.isFunction && mem.type && mem.decl &&
                            static_cast<FunctionDecl*>(mem.decl)->isMutating)
                            ownerMutating_.insert(td->name);
                // A computed property lowers to accessor functions rather than
                // storage, so it must be recorded before member access.
                if (td->semaType->record) {
                    for (const auto& mem : td->semaType->record->members) {
                        if (!mem.decl || mem.decl->kind != NodeKind::VarDecl) continue;
                        auto* pv = static_cast<VarDecl*>(mem.decl);
                        if (pv->accessors.empty()) continue;
                        bool computed = false;
                        bool observed = false;
                        for (auto& a : pv->accessors) {
                            if (!a || a->kind != NodeKind::AccessorDecl) continue;
                            auto* ad = static_cast<AccessorDecl*>(a.get());
                            if (ad->kind == AccessorDecl::Kind::Getter ||
                                ad->kind == AccessorDecl::Kind::Setter)
                                computed = true;
                            if (ad->kind == AccessorDecl::Kind::WillSet ||
                                ad->kind == AccessorDecl::Kind::DidSet)
                                observed = true;
                        }
                        if (computed) computedProps_[td->name + "." + pv->name] = pv;
                        if (observed) observedProps_[td->name + "." + pv->name] = pv;
                        declareAccessors(td->semaType, pv, mem.type);
                    }
                }
                classTypes_[td->name] = td->semaType;
                layout_->lower(td->semaType);
                // Instance methods are mangled "Type.method" exactly like a
                // struct's, so dispatch and `self` handling are shared. Members
                // are taken from the Sema record, which already folds in
                // extension methods and inherited protocol defaults (规范 2.6/4.5).
                for (const auto& mem : td->semaType->record->members) {
                    if (!mem.isFunction || !mem.decl ||
                        mem.decl->kind != NodeKind::FunctionDecl) continue;
                    auto* mf = static_cast<FunctionDecl*>(mem.decl);
                    // 规范 §8.7：@no_mangle 保留给定符号名，不改写成 "Type.method"。
                    std::string nmSym = mf->name;
                    declareMethod(td->name + "." + mf->name, mf, td->semaType,
                                  hasAttr(mf, "no_mangle") ? &nmSym : nullptr,
                                  mem.isStatic);
                    methodOrder_.emplace_back(mf, td->semaType);
                }
                for (const auto& mem : td->semaType->record->members) {
                    if (!mem.decl || mem.decl->kind != NodeKind::InitDecl) continue;
                    auto* id = static_cast<InitDecl*>(mem.decl);
                    declareInit(td->name + ".init", id, td->semaType);
                    initOrder_.emplace_back(id, td->semaType);
                }
                for (const auto& mem : td->semaType->record->members) {
                    if (!mem.decl || mem.decl->kind != NodeKind::DeinitDecl) continue;
                    auto* dd = static_cast<DeinitDecl*>(mem.decl);
                    declareDeinit(td->name + ".deinit", dd, td->semaType);
                    deinitOrder_.emplace_back(dd, td->semaType);
                }
            }
        }

        for (auto& d : decls) {
            if (!d || d->kind != NodeKind::StructDecl) continue;
            auto* td = static_cast<TypeDecl*>(d.get());
            if (!td->semaType) continue;
            structTypes_[td->name] = td->semaType;
            // The calling convention is needed before declaring accessors.
            if (td->semaType->record)
                for (const auto& mem : td->semaType->record->members)
                    if (mem.isFunction && mem.type && mem.decl &&
                        static_cast<FunctionDecl*>(mem.decl)->isMutating)
                        ownerMutating_.insert(td->name);
            // A computed property lowers to accessor functions rather than
            // storage, so it must be recorded before member access.
            if (td->semaType->record) {
                for (const auto& mem : td->semaType->record->members) {
                    if (!mem.decl || mem.decl->kind != NodeKind::VarDecl) continue;
                    auto* pv = static_cast<VarDecl*>(mem.decl);
                    if (pv->accessors.empty()) continue;
                    bool computed = false;
                    bool observed = false;
                    for (auto& a : pv->accessors) {
                        if (!a || a->kind != NodeKind::AccessorDecl) continue;
                        auto* ad = static_cast<AccessorDecl*>(a.get());
                        if (ad->kind == AccessorDecl::Kind::Getter ||
                            ad->kind == AccessorDecl::Kind::Setter)
                            computed = true;
                        if (ad->kind == AccessorDecl::Kind::WillSet ||
                            ad->kind == AccessorDecl::Kind::DidSet)
                            observed = true;
                    }
                    if (computed) computedProps_[td->name + "." + pv->name] = pv;
                    if (observed) observedProps_[td->name + "." + pv->name] = pv;
                    declareAccessors(td->semaType, pv, mem.type);
                }
            }
            layout_->lower(td->semaType);
            // Collect instance methods (mangled "Type.method") after the
            // owner's layout exists so signatures can reference %T. Members come
            // from the Sema record so extension methods are included too.
            for (const auto& mem : td->semaType->record->members) {
                if (!mem.isFunction || !mem.decl ||
                    mem.decl->kind != NodeKind::FunctionDecl) continue;
                auto* mf = static_cast<FunctionDecl*>(mem.decl);
                // 规范 §8.7：@no_mangle 保留给定符号名，不改写成 "Type.method"。
                std::string nmSym = mf->name;
                declareMethod(td->name + "." + mf->name, mf, td->semaType,
                              hasAttr(mf, "no_mangle") ? &nmSym : nullptr,
                              mem.isStatic);
                methodOrder_.emplace_back(mf, td->semaType);
            }
        }

        // All functions (foreign + ordinary) must be declared *before* any body is
        // generated. In particular, a generic struct's method bodies are emitted
        // eagerly below (the generic-type instantiation loop), and those bodies may
        // call foreign functions — so the foreign symbols must already exist in
        // `fns_` or `genCall` would fabricate a wrong variadic placeholder.
        std::vector<FunctionDecl*> genericFns;
        for (auto& d : decls) {
            if (!d || d->kind != NodeKind::FunctionDecl) continue;
            auto* fn = static_cast<FunctionDecl*>(d.get());
            if (!fn->genericParams.empty()) genericFns.push_back(fn);
            else declare(fn);
        }

        // Generic type instantiations (规范 5.2): each `Box<Int>` is a distinct
        // record with concrete members. Register it under its mangled key so
        // constructor calls and method dispatch resolve per instance, and emit its
        // methods with that instance's parameters bound — the shared method AST is
        // re-annotated for each instance, exactly as generic functions are.
        if (sema_) {
            for (const auto& gi : sema_->genericTypeInstances()) {
                const Type* instTy = gi.instanceType;
                if (!instTy || !instTy->record) continue;
                if (gi.isClass) classTypes_[gi.key] = instTy;
                else structTypes_[gi.key] = instTy;
                layout_->lower(instTy);
                sema_->bindTypeParams(gi.params, gi.args);
                for (const auto& mem : instTy->record->members) {
                    if (!mem.isFunction || !mem.decl ||
                        mem.decl->kind != NodeKind::FunctionDecl) continue;
                    auto* mf = static_cast<FunctionDecl*>(mem.decl);
                    sema_->resolveFunctionSignature(mf);
                    // Channel's members are runtime-backed and async, so they are
                    // emitted directly (returning a completed Future) instead of
                    // being declared from the signature and lowered from a body.
                    if (gi.typeName == "Channel") {
                        genChannelMethod(mf, instTy);
                        continue;
                    }
                    // TaskGroup<T>'s members are runtime-backed too; the element
                    // type T drives how the child result is stored / collected.
                    if (gi.typeName == "TaskGroup" && !instTy->elements.empty()) {
                        genTaskGroupMethod(mf, instTy);
                        continue;
                    }
                    const std::string mkey = gi.key + "." + mf->name;
                    declareMethod(mkey, mf, instTy, /*symbolOverride=*/nullptr,
                                  mem.isStatic);
                    auto mit = methodFns_.find(mkey);
                    if (mit != methodFns_.end()) genMethodBody(mf, instTy, mit->second);
                }
                sema_->unbindTypeParams();
            }
        }

        // Module-level variables become globals so every function can reach
        // them; a nested declaration would be invisible across functions.
        for (auto& d : decls) {
            if (!d || d->kind != NodeKind::VarDecl) continue;
            auto* gv = static_cast<VarDecl*>(d.get());
            if (gv->name.empty()) continue;
            // The declared type is what determines the global's LLVM type; an
            // aggregate (or class) global must be zero-initialised and mutable.
            const Type* gt = gv->semaType;
            if (!gt && gv->type) gt = gv->type->semaType;
            if (!gt || gt->kind == TypeKind::Unknown) continue;
            llvm::Type* ty = layout_->lower(gt);
            if (!ty) continue;
            auto* slot = new llvm::GlobalVariable(
                *module_, ty, /*isConstant=*/false,
                llvm::GlobalValue::InternalLinkage,
                llvm::Constant::getNullValue(ty), ("g." + gv->name).c_str());
            globalsMap_[gv->name] = slot;
            globalOrder_.emplace_back(gv, gt);
        }

        // Monomorphise: for each recorded instance, re-check the generic body
        // with its type parameters bound — which annotates the shared AST with
        // that instance's concrete types — and emit it immediately, so the next
        // instance overwrites the annotations before the next body is generated.
        if (sema_) {
            const auto& instances = sema_->genericInstances();
            for (size_t i = 0; i < instances.size(); ++i) {
                FunctionDecl* target = nullptr;
                for (FunctionDecl* g : genericFns)
                    if (g->name == instances[i].funcName) { target = g; break; }
                if (!target) continue;
                // The binding must span both the declaration and the body:
                // each resolves `T` through the analyser.
                sema_->bindInstance(i, target);
                const std::string sym = monoSymbol(target->name,
                                                   instances[i].typeArgs);
                declareAs(target, sym);
                generateBodyAs(target, sym);
                sema_->unbindInstance();
                monoNames_[target->name].push_back(sym);
            }
            // Bodies of the instances were emitted above; anything still pending
            // is a non-generic function.
        }
    }
    // Phase 2: generate the deferred function/initialiser/method/accessor/
    // deinit bodies now that every symbol exists in the module.
    void generateBodies(const NodeList& decls) {        // 自定义下标的 getter/setter 必须在生成普通函数体之前声明，否则
        // 函数体内对 `obj[idx]` 的调用在查表时还找不到对应方法（规范 3.1）。
        genSubscriptBodies(decls);
        // Async spawns must exist *before* bodies are generated: a body (e.g.
        // @main) that awaits an async call resolves `<fn>_spawn` at that moment, and
        // a missing entry silently emits no call at all, leaving the Future slot
        // uninitialised.
        // Concurrency machinery (Task.wait / TaskGroup.addTask / waitForAll, the
        // sleep / withTaskGroup spawns and their worker trampolines) is no longer
        // emitted up-front: each piece is generated lazily the first time it is
        // actually used, so a plain programme ships none of it.
        for (auto& pb : pendingBodies_) {
            // 外部函数（foreign）只声明符号、不发射函数体，否则会与
            // runtime.c 里的真实定义产生 multiple definition 冲突。
            if (pb.first->isForeign) continue;
            if (fns_.count(pb.second) && !fns_[pb.second]->empty()) continue;
            generateBodyAs(pb.first, pb.second);
        }
        // A global's initialiser needs real code, so it goes into a synthetic
        // entry function that `main` calls first. Generating it outside any
        // function would leave the IR builder without an insertion point.
        if (!globalOrder_.empty())
            genGlobalInitialisers();

        for (auto& io : initOrder_) {
            auto idIt = methodFns_.find(io.second->name + ".init");
            if (idIt != methodFns_.end())
                genInitBody(io.first, io.second, idIt->second);
        }

        for (auto& d : decls) {
            if (!d || (d->kind != NodeKind::ClassDecl &&
                       d->kind != NodeKind::StructDecl)) continue;
            auto* td = static_cast<TypeDecl*>(d.get());
            if (td->semaType) genAccessorBodies(td->semaType);
        }

        for (auto& dop : deinitOrder_) {
            auto ddIt = methodFns_.find(dop.second->name + ".deinit");
            if (ddIt != methodFns_.end())
                genDeinitBody(dop.first, dop.second, ddIt->second);
        }

        for (auto& mo : methodOrder_) {
            FunctionDecl* mf = mo.first;
            const Type* owner = mo.second;
            auto it = methodFns_.find(owner->name + "." + mf->name);
            if (it != methodFns_.end()) genMethodBody(mf, owner, it->second);
        }

    }
    public:
    std::string irText() {
        std::string out;
        llvm::raw_string_ostream os(out);
        module_->print(os, nullptr);
        return os.str();
    }

    // BLK-B: lower the generated module directly to a native object file using
    // LLVM's TargetMachine + MC/AsmPrinter. Replaces the old `clang -c`
    // shell-out so the driver is fully self-contained for code generation.
    bool emitObject(const NodeList& decls, const std::string& objPath,
                    std::string* irOut, std::string& errOut,
                    int optLevel, bool optSize, bool emitAsm) {
        if (!generate(decls, errOut)) return false;
        if (irOut) *irOut = irText();

        std::string err;
        llvm::TargetOptions opts;
        const llvm::Target* target =
            llvm::TargetRegistry::lookupTarget(target_.triple, err);
        if (!target) {
            errOut = "cannot select LLVM target for '" + target_.triple +
                     "': " + err;
            return false;
        }
        // 规范 §10.4 / §14.1.3：把 `-O*` / `-Os` 映射到 LLVM 优化级别。
        // 0=None(O0)，1=Less(O1)，2=Default(O2)，3=Aggressive(O3)；
        // optSize 对应 `-Os`（尺寸优先，通过 TargetOptions.OptimizeSize 启用）。
        llvm::CodeGenOptLevel ol = llvm::CodeGenOptLevel::None;
        if (optLevel >= 3) ol = llvm::CodeGenOptLevel::Aggressive;
        else if (optLevel == 2) ol = llvm::CodeGenOptLevel::Default;
        else if (optLevel == 1) ol = llvm::CodeGenOptLevel::Less;
        if (optSize) {
            // 规范 §10.4 / §14.1.3：`-Os` 尺寸优先。LLVM 没有独立的 size 优化级别，
            // 改为给所有函数附加 OptimizeForSize 属性，促使后端按尺寸取舍。
            for (auto& F : *module_)
                F.addFnAttr(llvm::Attribute::OptimizeForSize);
        }

        // Emit position-independent code: the system linker builds a PIE
        // executable by default, so absolute (R_X86_64_32) relocations would be
        // rejected. PIC relocations (R_X86_64_PC32, RIP-relative) match what the
        // precompiled runtime object uses and link cleanly into a PIE.
        std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
            target_.triple, "generic", "", opts, llvm::Reloc::PIC_,
            std::nullopt, ol));
        if (!tm) {
            errOut = "cannot create target machine for '" + target_.triple + "'";
            return false;
        }
        // Keep the module's data layout in lock-step with the machine that will
        // emit code for it (the layout drives ABI padding decisions).
        module_->setDataLayout(tm->createDataLayout());

        std::error_code ec;
        llvm::raw_fd_ostream dest(objPath, ec, llvm::sys::fs::OF_None);
        if (ec) {
            errOut = "cannot open object file '" + objPath + "': " + ec.message();
            return false;
        }

        llvm::legacy::PassManager pm;
        // 规范 §14.1.3：`-S` 输出汇编文本，否则输出对象文件。
        llvm::CodeGenFileType ft = emitAsm ? llvm::CodeGenFileType::AssemblyFile
                                           : llvm::CodeGenFileType::ObjectFile;
        if (tm->addPassesToEmitFile(pm, dest, /*DwoOut=*/nullptr, ft)) {
            errOut = "target '" + target_.triple +
                     "' does not support " +
                     (emitAsm ? std::string("assembly") : std::string("object-file")) +
                     " emission";
            return false;
        }
        pm.run(*module_);
        dest.flush();
        return true;
    }

    TargetInfo target_;
    std::unique_ptr<llvm::LLVMContext> ctx_;
    std::unique_ptr<llvm::Module> module_;
    std::unique_ptr<llvm::IRBuilder<>> b_;
    std::map<std::string, llvm::Function*> fns_;
    // Declarations still waiting for their body, as (decl, symbol name).
    std::vector<std::pair<FunctionDecl*, std::string>> pendingBodies_;
    std::map<std::string, bool> isMainFns_;
    std::map<std::string, llvm::Value*> locals_;
    // inout 形参：其 local 槽存放的是调用方地址（T*）。读取需多解引用一次，
    // 赋值直接写回该地址。键为形参内部名，按函数体清理（同 locals_）。
    std::unordered_set<std::string> inoutLocals_;
    llvm::Type* currentRet_ = nullptr;
    bool isMain_ = false;
    // 规范 §7.4 重入：当前方法体所属 actor 的类型（空 = 不在 actor 隔离域内）。
    const Type* actorIsolationOwner_ = nullptr;
    // 惰性生成 actor 隔离锁指针（对象头 index 3）。必须惰性：方法体入口处
    // 插入点尚未建立，提前发射 GEP 会把指令落到上一个基本块末尾，
    // IR 验证会报 "Basic Block does not have terminator"。
    llvm::Value* currentActorIsolationLock() {
        if (!actorIsolationOwner_ || !actorIsolationOwner_->record ||
            actorIsolationOwner_->record->kind != TypeDeclKind::Actor)
            return nullptr;
        auto it = locals_.find("self");
        if (it == locals_.end()) return nullptr;
        llvm::StructType* ost = layout_->objectType(actorIsolationOwner_->record);
        if (!ost) return nullptr;
        llvm::Value* obj = b_->CreateLoad(llvm::PointerType::getUnqual(*ctx_),
                                          it->second, "self.obj");
        return b_->CreateStructGEP(ost, obj, 3, "actor.iso");
    }
    // Declare a value-type instance method. The receiver is an implicit first
    // parameter: a `mutating` method receives it as a pointer (so writes hit the
    // caller's copy in place, preserving value semantics), otherwise by value.
    // `symbolOverride` 非空时以该名字作为 LLVM 符号（规范 §8.7 `@no_mangle`），
    // 而 `key` 仍为分派用的改写名，二者解耦：调用方按 "Type.method" 查找，
    // 链接器看到的是未改写的给定符号名。
    void declareMethod(const std::string& key, FunctionDecl* fn, const Type* ownerTy,
                       const std::string* symbolOverride = nullptr,
                       bool isStatic = false) {
        // A generic type's methods are emitted once per monomorphised instance
        // (`Box<Int>.get`), never for the uninstantiated generic: its members still
        // mention the type parameter, so its signature cannot be lowered.
        if (ownerTy && ownerTy->record && !ownerTy->record->genericParams.empty())
            return;
        if (methodFns_.count(key)) return;
        std::vector<llvm::Type*> params;
        if (!isStatic) {
            // A value-type instance method receives the receiver as an implicit
            // first parameter: a `mutating` method gets its address (so writes
            // hit the caller's copy), otherwise a copy by value.
            llvm::Type* selfTy = layout_->lower(ownerTy);
            llvm::Type* selfParam = fn->isMutating
                ? static_cast<llvm::Type*>(llvm::PointerType::getUnqual(selfTy))
                : selfTy;
            params.push_back(selfParam);
        }
        for (auto& p : fn->params)
            params.push_back(lowerDeclType(p.semaType, p.type.get()));
        // A throwing method carries the hidden error slot just like a function.
        if (fn->isThrows) params.push_back(llvm::PointerType::get(*ctx_, 0));
        const Type* mrt =
            (fn->returnType && fn->returnType->kind == NodeKind::OptionalType &&
             static_cast<OptionalType*>(fn->returnType.get())->isOpaque &&
             fn->semaType && fn->semaType->kind == TypeKind::Function && fn->semaType->ret)
                ? fn->semaType->ret
                : (fn->returnType ? fn->returnType->semaType : nullptr); // 不透明返回类型（规范 5.5）
        llvm::Type* ret = mrt ? lowerDeclType(mrt, fn->returnType.get())
                               : llvm::Type::getVoidTy(*ctx_);
        methodThrows_[key] = fn->isThrows;
        llvm::Function* f = llvm::Function::Create(
            llvm::FunctionType::get(ret, params, /*isVarArg=*/false),
            llvm::GlobalValue::ExternalLinkage,
            symbolOverride ? *symbolOverride : key, module_.get());
        methodFns_[key] = f;
        if (isStatic) staticMethods_.insert(key);
    }

    void declareInit(const std::string& key, InitDecl* id, const Type* ownerTy) {
        if (methodFns_.count(key)) return;
        llvm::Type* selfParam = layout_->lower(ownerTy);
        std::vector<llvm::Type*> params{ selfParam };
        for (auto& p : id->params)
            params.push_back(lowerDeclType(p.semaType, p.type.get()));
        llvm::Function* f = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx_), params, false),
            llvm::GlobalValue::ExternalLinkage, key, module_.get());
        methodFns_[key] = f;
    }

    void genInitBody(InitDecl* id, const Type* ownerTy, llvm::Function* f) {
        if (!f || !f->empty()) return;
        isMain_ = false;
        inInitializer_ = true;   // assignments here set the initial value
        currentOwner_ = ownerTy;
        currentRet_ = llvm::Type::getVoidTy(*ctx_);
        locals_.clear();
        inoutLocals_.clear();
        scopeRefs_.clear();
        scopeMarks_.clear();
        currentScopeMark_ = 0;
        b_->SetInsertPoint(llvm::BasicBlock::Create(*ctx_, "entry", f));

        // `self` is the object under construction; keeping it in a slot lets
        // field assignment and nested calls share the ordinary code paths.
        llvm::Type* selfTy = layout_->lower(ownerTy);
        llvm::Value* selfSlot = b_->CreateAlloca(selfTy, nullptr, "self");
        b_->CreateStore(f->getArg(0), selfSlot);
        locals_["self"] = selfSlot;

        unsigned arg = 1;
        for (auto& p : id->params) {
            llvm::Type* pt = lowerDeclType(p.semaType, p.type.get());
            llvm::Value* slot = b_->CreateAlloca(pt, nullptr, p.internalName);
            if (arg < f->arg_size()) b_->CreateStore(f->getArg(arg), slot);
            locals_[p.internalName] = slot;
            ++arg;
        }
        currentScopeMark_ = scopeRefs_.size();
        for (auto& st : id->body) genStmt(st.get());
        releaseScopeTo(currentScopeMark_);
        if (!b_->GetInsertBlock()->getTerminator()) b_->CreateRetVoid();
    }

    // `deinit` is a DeinitDecl: no parameters beyond `self`, no return value.
    // It is stored in the object header so the runtime can run it when the last
    // strong reference goes away.
    void declareDeinit(const std::string& key, DeinitDecl* dd,
                       const Type* ownerTy) {
        if (methodFns_.count(key)) return;
        llvm::Function* f = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx_),
                                    { layout_->lower(ownerTy) }, false),
            llvm::GlobalValue::ExternalLinkage, key, module_.get());
        methodFns_[key] = f;
    }

    void genDeinitBody(DeinitDecl* dd, const Type* ownerTy, llvm::Function* f) {
        if (!f || !f->empty()) return;
        isMain_ = false;
        currentOwner_ = ownerTy;
        currentRet_ = llvm::Type::getVoidTy(*ctx_);
        locals_.clear();
        inoutLocals_.clear();
        scopeRefs_.clear();
        scopeMarks_.clear();
        currentScopeMark_ = 0;
        b_->SetInsertPoint(llvm::BasicBlock::Create(*ctx_, "entry", f));
        llvm::Type* selfTy = layout_->lower(ownerTy);
        llvm::Value* selfSlot = b_->CreateAlloca(selfTy, nullptr, "self");
        b_->CreateStore(f->getArg(0), selfSlot);
        locals_["self"] = selfSlot;
        currentScopeMark_ = scopeRefs_.size();
        for (auto& st : dd->body) genStmt(st.get());
        releaseScopeTo(currentScopeMark_);
        // A subclass destructor must let its parent's run as well, otherwise
        // the inherited half of the object would never be cleaned up.
        if (ownerTy && ownerTy->record && ownerTy->record->superclass) {
            auto pit = methodFns_.find(ownerTy->record->superclass->name + ".deinit");
            if (pit != methodFns_.end() && pit->second && pit->second != f)
                b_->CreateCall(pit->second, { f->getArg(0) });
        }
        if (!b_->GetInsertBlock()->getTerminator()) b_->CreateRetVoid();
    }

    // ── computed properties ────────────────────────────────────────────────
    // A computed property has no storage, so its accessors become ordinary
    // functions mangled as accessors. `get` returns the property type; `set`
    // takes the new value; the observers take the value (and, for `didSet`, the
    // previous one). They are *not* virtual: an accessor belongs to the property
    // it was declared with, and virtual dispatch on it would be surprising.
    llvm::Function* accessorFn(const TypeRecord* rec, const std::string& prop,
                               AccessorDecl::Kind kind, llvm::Type** valueTyOut) {
        std::string suffix = kind == AccessorDecl::Kind::Getter ? ".get"
                           : kind == AccessorDecl::Kind::Setter ? ".set"
                           : kind == AccessorDecl::Kind::WillSet ? ".willSet"
                                                                  : ".didSet";
        std::string key = rec->name + "." + prop + suffix;
        auto it = methodFns_.find(key);
        if (it != methodFns_.end() && it->second) {
            if (valueTyOut) {
                llvm::FunctionType* ft = it->second->getFunctionType();
                *valueTyOut = ft->getNumParams() > 1
                    ? ft->getParamType(1) : ft->getReturnType();
            }
            return it->second;
        }
        return nullptr;
    }

    // Declare every accessor of `vd` in the owner's context. The property's own
    // type comes from the analyser, so a computed property can return an
    // aggregate or a class reference just like a method.
    void declareAccessors(const Type* ownerTy, VarDecl* vd,
                          const Type* propType) {
        if (!vd || vd->accessors.empty()) return;
        const std::string prop = vd->name;
        llvm::Type* selfTy = layout_->lower(ownerTy);
        // Match the owner's calling convention: a `mutating` member passes
        // `self` by pointer, everything else by value.
        if (ownerMutating_.count(ownerTy->name))
            selfTy = llvm::PointerType::getUnqual(selfTy);
        // The property type was resolved when the record's members were
        // collected, which happens before bodies are generated.
        const Type* vt = propType ? propType : vd->semaType;
        if (!vt) return;
        llvm::Type* valTy = layout_->lower(vt);
        // Record it so the body generator agrees with the signature.
        vd->semaType = vt;
        for (auto& a : vd->accessors) {
            if (!a || a->kind != NodeKind::AccessorDecl) continue;
            auto* ad = static_cast<AccessorDecl*>(a.get());
            std::string suffix = ad->kind == AccessorDecl::Kind::Getter ? ".get"
                               : ad->kind == AccessorDecl::Kind::Setter ? ".set"
                               : ad->kind == AccessorDecl::Kind::WillSet ? ".willSet"
                                                                          : ".didSet";
            std::string key = ownerTy->name + "." + prop + suffix;
            if (methodFns_.count(key)) continue;
            // A setter and the observers receive the incoming value; a getter
            // receives only the receiver. `selfTy` is a pointer for classes and
            // the aggregate itself for structs, matching how methods on the same
            // owner are called.
            std::vector<llvm::Type*> params{selfTy};
            if (ad->kind != AccessorDecl::Kind::Getter) params.push_back(valTy);
            llvm::Type* ret = ad->kind == AccessorDecl::Kind::Getter
                ? valTy : llvm::Type::getVoidTy(*ctx_);
            methodFns_[key] = llvm::Function::Create(
                llvm::FunctionType::get(ret, params, false),
                llvm::GlobalValue::ExternalLinkage, key, module_.get());
        }
    }

    // Generate the bodies of every accessor registered for `rec`.
    void genAccessorBodies(const Type* ownerTy) {
        const TypeRecord* rec = ownerTy ? ownerTy->record : nullptr;
        if (!rec) return;
        for (const auto& m : rec->members) {
            if (!m.decl || m.decl->kind != NodeKind::VarDecl) continue;
            auto* vd = static_cast<VarDecl*>(m.decl);
            for (auto& a : vd->accessors) {
                if (!a || a->kind != NodeKind::AccessorDecl) continue;
                auto* ad = static_cast<AccessorDecl*>(a.get());
                std::string suffix = ad->kind == AccessorDecl::Kind::Getter ? ".get"
                               : ad->kind == AccessorDecl::Kind::Setter ? ".set"
                               : ad->kind == AccessorDecl::Kind::WillSet ? ".willSet"
                                                                          : ".didSet";
                std::string key = rec->name + "." + vd->name + suffix;
                auto fit = methodFns_.find(key);
                if (fit == methodFns_.end() || !fit->second) continue;
                currentPropertyName_ = vd->name;
                // m.type was resolved when the record was collected; vd->semaType
                // is not filled in until checking, which runs after this point.
                genAccessorBody(ad, ownerTy,
                                m.type ? m.type : vd->semaType, fit->second);
            }
        }
    }

    void genAccessorBody(AccessorDecl* ad, const Type* ownerTy,
                         const Type* propTy, llvm::Function* f) {
        if (!f || !f->empty()) return;
        isMain_ = false;
        currentOwner_ = ownerTy;
        currentRet_ = ad->kind == AccessorDecl::Kind::Getter
            ? layout_->lower(propTy) : llvm::Type::getVoidTy(*ctx_);
        // Without a property type there is nothing the getter can return.
        if (!currentRet_) return;
        locals_.clear();
        inoutLocals_.clear();
        scopeRefs_.clear();
        scopeMarks_.clear();
        currentScopeMark_ = 0;
        b_->SetInsertPoint(llvm::BasicBlock::Create(*ctx_, "entry", f));

        llvm::Type* selfTy = layout_->lower(currentOwner_);
        if (ownerMutating_.count(currentOwner_->name)) {
            // A mutating owner (the type declares at least one `mutating`
            // member) hands `self` to its accessors by pointer — exactly like a
            // `mutating` method. Bind `self` directly to the incoming pointer so
            // field GEPs dereference it once. Wrapping the pointer in an extra
            // alloca would make `genRefBasePtr` (which only strips a level of
            // indirection for reference types) emit a spurious second hop, and
            // every `self.<field>` access would then read the wrong address.
            locals_["self"] = f->getArg(0);
        } else {
            llvm::Value* selfSlot = b_->CreateAlloca(selfTy, nullptr, "self");
            b_->CreateStore(f->getArg(0), selfSlot);
            locals_["self"] = selfSlot;
        }

        // A setter/observer names its incoming value; default to `newValue` /
        // `oldValue` when the declaration omitted the parameter.
        unsigned arg = 1;
        if (ad->kind != AccessorDecl::Kind::Getter) {
            llvm::Type* vt = layout_->lower(propTy);
            llvm::Value* slot = b_->CreateAlloca(vt, nullptr, "value");
            if (arg < f->arg_size()) b_->CreateStore(f->getArg(arg), slot);
            locals_[ad->valueParam.empty()
                    ? (ad->kind == AccessorDecl::Kind::DidSet ? "oldValue"
                                                             : "newValue")
                    : ad->valueParam] = slot;
            ++arg;
            // `didSet` also receives the value being replaced.
            if (ad->kind == AccessorDecl::Kind::DidSet) {
                llvm::Value* oldSlot = b_->CreateAlloca(vt, nullptr, "old");
                // Read the current storage so the observer sees the old value.
                if (auto* fld =
                        storageFieldPtr(ownerTy->record, currentPropertyName_)) {
                    llvm::Value* cur = b_->CreateLoad(
                        fld->getResultElementType(), fld);
                    b_->CreateStore(cur, oldSlot);
                }
                locals_["oldValue"] = oldSlot;
            }
        }
        for (auto& st : ad->body) {
            genStmt(st.get());
        }
        if (!b_->GetInsertBlock()->getTerminator()) {
            if (currentRet_->isVoidTy()) b_->CreateRetVoid();
            else b_->CreateRet(llvm::Constant::getNullValue(currentRet_));
        }
    }

    // Run `willSet` / `didSet` around a store to `prop` on `record`.
    //
    // Order matters and mirrors the language contract: `willSet` sees the value
    // about to be stored, the store happens, then `didSet` sees the value that
    // was there before. Observers do not fire while an initialiser runs, because
    // an initial value is not a change.
    // Whether `prop` on `record` declared willSet / didSet.
    bool hasObservers(const TypeRecord* record, const std::string& prop) {
        return record && observedProps_.count(record->name + "." + prop) > 0;
    }

    void fireObservers(const TypeRecord* record, const std::string& prop,
                       llvm::Value* self, llvm::Value* newValue,
                       llvm::Type* propTy, llvm::Value* oldValue) {
        if (inInitializer_ || !record) return;
        std::string key = record->name + "." + prop;
        if (!observedProps_.count(key)) return;
        llvm::Type* dummy = nullptr;
        llvm::Function* will = accessorFn(record, prop,
            AccessorDecl::Kind::WillSet, &dummy);
        llvm::Function* did = accessorFn(record, prop,
            AccessorDecl::Kind::DidSet, &dummy);
        llvm::Function* probe = will ? will : did;
        llvm::Type* selfTy = probe->getFunctionType()->getParamType(0);
        // A struct accessor takes `self` by value, so the caller hands us the
        // address of its storage and we load from it.
        llvm::Value* recv = self;
        if (!selfTy->isPointerTy())
            if (auto* ai = llvm::dyn_cast<llvm::AllocaInst>(self))
                recv = b_->CreateLoad(ai->getAllocatedType(), ai);
        recv = coerce(recv, selfTy);
        if (will)
            b_->CreateCall(will, {recv, coerce(newValue, propTy)});
        if (did)
            b_->CreateCall(did, {recv, coerce(oldValue ? oldValue : newValue, propTy)});
    }

    // ── ARC scope tracking ─────────────────────────────────────────────────
    // A local holding a strong reference must release it when the variable goes
    // out of scope, otherwise the object outlives its lexical lifetime. Releases
    // are emitted in reverse declaration order, mirroring the way the
    // references were taken.
    struct ScopedRef {
        std::string name;
        const Type* type;
    };
    std::vector<ScopedRef> scopeRefs_;
    // Suffix of scopeRefs_ to release when the current function body ends.
    std::vector<size_t> scopeMarks_;
    // `break` / `continue` targets of the enclosing loops, innermost last. Each
    // loop generation pushes its exit / step block and pops it afterwards, so a
    // nested loop cannot capture its parent's target.
    std::vector<llvm::BasicBlock*> breakTargets_;
    std::vector<llvm::BasicBlock*> continueTargets_;
    // 带标签循环的断点映射（规范 §1.7 `outer: loop { … break outer }`）：
    // 标签名 -> (break 目标块, continue 目标块)。
    std::unordered_map<std::string, std::pair<llvm::BasicBlock*, llvm::BasicBlock*>> labelTargets_;
    // Mark of the enclosing function body; a `return` releases down to it.
    size_t currentScopeMark_ = 0;

    // Note that `name` holds a strong reference for the current scope.
    void trackScopedRef(const std::string& name, const Type* t) {
        if (!t || !layout_->isReferenceType(t)) return;
        scopeRefs_.push_back(ScopedRef{name, t});
    }

    // Release every reference recorded since the matching mark, innermost first.
    void releaseScopeTo(size_t mark) {
        for (size_t i = scopeRefs_.size(); i > mark; --i) {
            auto sit = locals_.find(scopeRefs_[i - 1].name);
            if (sit == locals_.end()) continue;
            // Locals are always allocas; anything else is not ours to release.
            auto* slot = llvm::dyn_cast<llvm::AllocaInst>(sit->second);
            if (!slot) continue;
            llvm::Value* v = b_->CreateLoad(slot->getAllocatedType(), slot);
            arcReleaseIfRef(v, scopeRefs_[i - 1].type);
        }
        scopeRefs_.resize(mark);
    }

    void genMethodBody(FunctionDecl* fn, const Type* ownerTy, llvm::Function* f) {
        if (!f || !f->empty()) return;
        isMain_ = false;
        currentOwner_ = ownerTy;
        // 规范 §7.4：标记本方法体处于 actor 隔离域，await 挂起点据此做重入。
        // 锁指针本身由 currentActorIsolationLock() 惰性生成。
        actorIsolationOwner_ = (ownerTy && ownerTy->record &&
                                ownerTy->record->kind == TypeDeclKind::Actor)
                                   ? ownerTy : nullptr;
        const Type* mrt =
            (fn->returnType && fn->returnType->kind == NodeKind::OptionalType &&
             static_cast<OptionalType*>(fn->returnType.get())->isOpaque &&
             fn->semaType && fn->semaType->kind == TypeKind::Function && fn->semaType->ret)
                ? fn->semaType->ret
                : (fn->returnType ? fn->returnType->semaType : nullptr); // 不透明返回类型（规范 5.5）
        currentRet_ = mrt ? lowerDeclType(mrt, fn->returnType.get())
                          : llvm::Type::getVoidTy(*ctx_);
        locals_.clear();
        inoutLocals_.clear();
        scopeRefs_.clear();
        scopeMarks_.clear();
        currentScopeMark_ = 0;
        b_->SetInsertPoint(llvm::BasicBlock::Create(*ctx_, "entry", f));

        llvm::Type* selfTy = layout_->lower(ownerTy);
        unsigned arg = 0;
        // A static method has no receiver: do not bind `self` and start binding
        // its real parameters at index 0 (the signature has no `self` slot).
        bool isStatic = staticMethods_.count(ownerTy->name + "." + fn->name) > 0;
        if (!isStatic) {
            if (fn->isMutating) {
                // Keep the receiver pointer: field writes go through it.
                locals_["self"] = f->getArg(0);
                arg = 1;
            } else {
                llvm::Value* slot = b_->CreateAlloca(selfTy, nullptr, "self");
                b_->CreateStore(f->getArg(0), slot);
                locals_["self"] = slot;
                arg = 1;
            }
        }
        for (auto& p : fn->params) {
            llvm::Type* pt = lowerDeclType(p.semaType, p.type.get());
            if (p.isInout) {
                // 按引用传参：形参槽持有调用方地址（规范 3.1）。
                locals_[p.internalName] = f->getArg(arg);
                inoutLocals_.insert(p.internalName);
            } else {
                if (p.isVariadic) pt = llvm::PointerType::getUnqual(
                    llvm::ArrayType::get(pt, 0));
                llvm::Value* slot = b_->CreateAlloca(pt, nullptr, p.internalName);
                if (arg < f->arg_size()) b_->CreateStore(f->getArg(arg), slot);
                locals_[p.internalName] = slot;
            }
            ++arg;
        }
        // A throwing method's hidden trailing argument is its error slot.
        llvm::Value* savedErrorSlot = currentErrorSlot_;
        currentErrorSlot_ = nullptr;
        if (fn->isThrows && f->arg_size() == fn->params.size() + 2) {
            currentErrorSlot_ = f->getArg(f->arg_size() - 1);
            b_->CreateStore(
                llvm::ConstantPointerNull::get(llvm::PointerType::get(*ctx_, 0)),
                currentErrorSlot_);
        }
        for (auto& s : fn->body) genStmt(s.get());
        if (!b_->GetInsertBlock()->getTerminator()) {
            if (currentRet_->isVoidTy()) b_->CreateRetVoid();
            else b_->CreateRet(llvm::Constant::getNullValue(currentRet_));
        }
        currentErrorSlot_ = savedErrorSlot;
    }

    // 自定义下标的单个存取器体（规范 3.1）。
    void genSubscriptBody(SubscriptDecl* sub, const Type* ownerTy,
                      const Type* idxT, const Type* elemT,
                      bool isSetter, llvm::Function* f) {
    if (!f || !f->empty()) return;
    // 保存被本函数改写的环境，结束后还原，避免污染后续函数体的生成。
    bool savedMain = isMain_;
    const Type* savedOwner = currentOwner_;
    llvm::Type* savedRet = currentRet_;
    isMain_ = false;
    currentOwner_ = ownerTy;
    currentRet_ = isSetter ? llvm::Type::getVoidTy(*ctx_)
                           : layout_->lower(elemT);
        locals_.clear();
        inoutLocals_.clear();
        scopeRefs_.clear();
        scopeMarks_.clear();
        currentScopeMark_ = 0;
        b_->SetInsertPoint(llvm::BasicBlock::Create(*ctx_, "entry", f));
        // getter 的 self 按值传递（struct 值），setter 的 self 按指针传递
        // （struct*，指向调用方的副本，写回对调用方可见）。两者最终都让
        // `self` 在体中等价于一个 struct*，成员访问 `self.x` 的处理一致。
        if (isSetter) {
            locals_["self"] = f->getArg(0);
        } else {
            llvm::Type* selfTy = layout_->lower(ownerTy);
            llvm::Value* selfSlot = b_->CreateAlloca(selfTy, nullptr, "self");
            b_->CreateStore(f->getArg(0), selfSlot);
            locals_["self"] = selfSlot;
        }
        // 下标形参命名为 `index`，与 getter 体内引用一致。
        if (!sub->params.empty()) {
            llvm::Type* iTy = layout_->lower(idxT);
            llvm::Value* islot = b_->CreateAlloca(iTy, nullptr, "index");
            b_->CreateStore(f->getArg(1), islot);
            locals_["index"] = islot;
        }
        if (isSetter) {
            llvm::Type* eTy = layout_->lower(elemT);
            llvm::Value* nslot = b_->CreateAlloca(eTy, nullptr, "newValue");
            b_->CreateStore(f->getArg(2), nslot);
            locals_["newValue"] = nslot;
        }
        currentScopeMark_ = scopeRefs_.size();
        for (auto& st : (isSetter ? sub->setter : sub->getter))
            genStmt(st.get());
        releaseScopeTo(currentScopeMark_);
        if (!b_->GetInsertBlock()->getTerminator()) {
            if (currentRet_->isVoidTy()) b_->CreateRetVoid();
            else b_->CreateRet(llvm::Constant::getNullValue(currentRet_));
        }
        // 还原被保存的环境（与函数入口的保存配对）。
        isMain_ = savedMain;
        currentOwner_ = savedOwner;
        currentRet_ = savedRet;
    }

    // 为所有含下标的类型登记并生成 getter/setter 函数（规范 3.1）。
    void genSubscriptBodies(const NodeList& decls) {
        for (auto& d : decls) {
            if (!d) continue;
            if (d->kind != NodeKind::StructDecl && d->kind != NodeKind::ClassDecl &&
                d->kind != NodeKind::EnumDecl)
                continue;
            auto* td = static_cast<TypeDecl*>(d.get());
            const TypeRecord* rec = sema_ ? sema_->findType(td->name) : nullptr;
            if (!rec) continue;
            const Type* ownerTy = td->semaType;
            if (!ownerTy) continue;
            for (auto& m : td->members) {
                if (!m || m->kind != NodeKind::SubscriptDecl) continue;
                auto* sub = static_cast<SubscriptDecl*>(m.get());
                const TypeRecord::Member* sm = nullptr;
                for (const auto& mm : rec->members)
                    if (mm.name == "subscript") { sm = &mm; break; }
                if (!sm || !sm->type) continue;
                const Type* idxT = !sm->type->elements.empty() ? sm->type->elements[0]
                                                              : nullptr;
                const Type* elemT = sm->type->ret ? sm->type->ret : nullptr;
                if (!idxT || !elemT) continue; // 下标需有下标参数与元素类型
                // getter：非 mutating，self 按值传递。
                {
                    std::string key = td->name + ".subscript.get";
                    if (methodFns_.count(key)) continue;
                    llvm::Type* selfTy = layout_->lower(ownerTy);
                    llvm::Type* iTy = layout_->lower(idxT);
                    llvm::Type* eTy = layout_->lower(elemT);
                    llvm::Function* f = llvm::Function::Create(
                        llvm::FunctionType::get(eTy, {selfTy, iTy}, false),
                        llvm::GlobalValue::ExternalLinkage, key, module_.get());
                    methodFns_[key] = f;
                    genSubscriptBody(sub, ownerTy, idxT, elemT, false, f);
                }
                // setter：始终可变，self 按指针传递以便写回。
                {
                    std::string key = td->name + ".subscript.set";
                    if (methodFns_.count(key)) continue;
                    llvm::Type* selfTy =
                        llvm::PointerType::getUnqual(layout_->lower(ownerTy));
                    llvm::Type* iTy = layout_->lower(idxT);
                    llvm::Type* eTy = layout_->lower(elemT);
                    llvm::Function* f = llvm::Function::Create(
                        llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx_),
                                                {selfTy, iTy, eTy}, false),
                        llvm::GlobalValue::ExternalLinkage, key, module_.get());
                    methodFns_[key] = f;
                    genSubscriptBody(sub, ownerTy, idxT, elemT, true, f);
                }
            }
        }
    }

    std::unique_ptr<TypeLayout> layout_;
    // Struct types reachable by name, for constructor lowering.
    std::map<std::string, const Type*> structTypes_;
    // Build (once) the vtable layout for `rec`: its parent's slots followed by
    // the virtual methods it introduces. A method that overrides an inherited
    // one keeps the parent's slot index, which is what makes the call site able
    // to dispatch without knowing the dynamic type.
    const std::vector<VirtualSlot>& vtableLayoutFor(const TypeRecord* rec) {
        auto it = vtableLayout_.find(rec->name);
        if (it != vtableLayout_.end()) return it->second;
        std::vector<VirtualSlot> slots;
        if (rec->superclass)
            slots = vtableLayoutFor(rec->superclass);
        for (const auto& m : rec->members) {
            if (!m.decl || m.decl->kind != NodeKind::FunctionDecl) continue;
            auto* fn = static_cast<FunctionDecl*>(m.decl);
            const std::string key = rec->name + "." + fn->name;
            if (nonVirtual_.count(key)) continue;   // `final`: bound statically
            bool seen = false;
            for (const VirtualSlot& v : slots)
                if (v.name == fn->name) { seen = true; break; }
            if (!seen) slots.push_back(VirtualSlot{fn->name, rec});
        }
        return vtableLayout_.emplace(rec->name, std::move(slots)).first->second;
    }

    // The vtable global for `rec`, filled with the address each slot resolves to
    // for this class: an override if the class (or a subclass) declares it,
    // otherwise the nearest inherited implementation.
    llvm::GlobalVariable* vtableFor(const TypeRecord* rec) {
        if (!rec) return nullptr;
        auto it = vtables_.find(rec->name);
        if (it != vtables_.end()) return it->second;

        const std::vector<VirtualSlot>& slots = vtableLayoutFor(rec);
        std::vector<llvm::Constant*> entries;
        // A class with no virtual methods still needs a distinct, non-null
        // table so that "is virtual" and "has a table" stay separate concerns.
        llvm::Type* fnPtrTy = llvm::PointerType::getUnqual(*ctx_);
        if (slots.empty()) {
            entries.push_back(llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(fnPtrTy)));
        } else {
            for (const VirtualSlot& slot : slots) {
                // Walk up to find the implementation this class should use.
                llvm::Function* impl = nullptr;
                for (const TypeRecord* r = rec; r && !impl; r = r->superclass) {
                    auto mit = methodFns_.find(r->name + "." + slot.name);
                    if (mit != methodFns_.end()) impl = mit->second;
                }
                if (!impl) {
                    // The declaring class is still being generated; leave the
                    // slot null rather than emitting a call to nothing.
                    entries.push_back(llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(fnPtrTy)));
                    continue;
                }
                // Every entry is stored in a uniform table of opaque function
                // pointers; the call site supplies the real signature.
                // Uniform table element type: bitcast the concrete function
                // pointer to the opaque `ptr` the table stores.
                entries.push_back(llvm::ConstantExpr::getBitCast(
                    llvm::cast<llvm::Function>(impl), fnPtrTy));
            }
        }
        // Bitcast every entry to a uniform table element type.
        llvm::ArrayType* arrTy = llvm::ArrayType::get(fnPtrTy,
            static_cast<uint64_t>(entries.size()));
        llvm::GlobalVariable* table = new llvm::GlobalVariable(
            *module_, arrTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(arrTy, entries), ("vtable." + rec->name).c_str());
        vtables_[rec->name] = table;
        return table;
    }

    // Call a virtual method: load the function pointer from the receiver's
    // vtable and call it indirectly, so the override that actually runs is the
    // one belonging to the object's dynamic type.
    llvm::Value* genVirtualCall(const TypeRecord* rec, const std::string& method,
                                llvm::Value* obj,
                                const std::vector<llvm::Value*>& args) {
        const std::vector<VirtualSlot>& slots = vtableLayoutFor(rec);
        int slotIndex = -1;
        for (size_t i = 0; i < slots.size(); ++i)
            if (slots[i].name == method) { slotIndex = static_cast<int>(i); break; }
        if (slotIndex < 0) return nullptr;   // not virtual: use a static call
        llvm::GlobalVariable* table = vtableFor(rec);
        if (!table) return nullptr;

        llvm::StructType* objSt = layout_->objectType(rec);
        if (!objSt) return nullptr;
        // Slot 0 of the object is the vtable pointer; slot 1 is the ARC count.
        llvm::Type* elemTy = table->getValueType()->getArrayElementType();
        llvm::Type* elemPtrTy = llvm::PointerType::getUnqual(elemTy);
        // Slot 0 of the object header holds the table pointer.
        llvm::Value* vtPtr = b_->CreateLoad(
            llvm::PointerType::getUnqual(table->getValueType()),
            b_->CreateStructGEP(objSt, obj, 0, "vtable.slot"));
        // Step to the requested slot inside that table.
        llvm::Value* slotPtr = b_->CreateInBoundsGEP(
            table->getValueType(), vtPtr,
            { llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0),
              llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), slotIndex) },
            "vt.slot");
        llvm::Value* entry = b_->CreateLoad(elemPtrTy, slotPtr, "vt.fn");
        // The table stores opaque pointers, so the call needs the callee's type
        // spelled out. Overrides of one slot share a signature, so the type of
        // the declaration this call site resolved is correct for every override.
        llvm::FunctionType* ft = nullptr;
        for (const TypeRecord* r = rec; r && !ft; r = r->superclass) {
            auto mit = methodFns_.find(r->name + "." + method);
            if (mit != methodFns_.end() && mit->second) ft = mit->second->getFunctionType();
        }
        if (!ft) return nullptr;
        return b_->CreateCall(ft, entry, args);
    }

    // Module-level variables live in globals rather than stack slots, so they
    // are visible from every function — including accessors and methods, whose
    // locals_ map only ever holds their own parameters and locals.
    // The analyser, consulted to re-check a generic function for each of its
    // instantiations. Null when the caller did not supply one.
    Sema* sema_ = nullptr;
    // "baseName<T1,T2>" per recorded instance, so a call site can find the
    // function that matches its argument types.
    std::map<std::string, std::vector<std::string>> monoNames_;
    std::map<std::string, llvm::GlobalVariable*> globalsMap_;
    llvm::Function* globalInitFn_ = nullptr;
    std::vector<std::pair<VarDecl*, const Type*>> globalOrder_;

    // Class types reachable by name. These are reference types: their "value"
    // in IR is a pointer to a heap object, not a by-value aggregate.
    std::map<std::string, const Type*> classTypes_;
    // Initialisers are emitted after plain methods so an `init` may call any
    // method of its own class.
    std::vector<std::pair<InitDecl*, const Type*>> initOrder_;
    // Destructors are emitted after methods and initialisers, because a
    // destructor may call any of them.
    std::vector<std::pair<DeinitDecl*, const Type*>> deinitOrder_;
    // Enum types reachable by name, for `Enum.case` tag resolution.
    std::map<std::string, const Type*> enumTypes_;
    // Instance methods of value types, keyed by mangled "Type.method".
    std::map<std::string, llvm::Function*> methodFns_;
    // Keys of methodFns_ that are *static* (no receiver); used to emit a direct
    // call without a `self` argument at the call site, and to skip the implicit
    // `self` binding while generating the method body.
    std::set<std::string> staticMethods_;
    // Free functions keyed by name, for call-site default/variadic expansion.
    std::map<std::string, FunctionDecl*> fnDecls_;
    std::vector<std::pair<FunctionDecl*, const Type*>> methodOrder_; // decl, owner
    // Enclosing value type while generating a method body (implicit self).
    const Type* currentOwner_ = nullptr;
};

// ─── IRGenerator (PIMPL façade) ─────────────────────────────────────────────
IRGenerator::IRGenerator(const TargetInfo& target) : impl_(std::make_unique<Impl>(target)) {}
IRGenerator::~IRGenerator() = default;
const TargetInfo& IRGenerator::target() const { return impl_->target(); }

// Without an analyser there is nothing to monomorphise, so a translation unit
// that still contains generic functions can only be emitted as written.
bool IRGenerator::emitIR(const NodeList& decls, std::string& irOut,
                         std::string& errOut) {
    if (!impl_->generate(decls, errOut)) return false;
    irOut = impl_->irText();
    return true;
}

bool IRGenerator::emitIR(const NodeList& decls, Sema& sema, std::string& irOut,
                         std::string& errOut) {
    impl_->sema_ = &sema;
    if (!impl_->generate(decls, errOut)) return false;
    irOut = impl_->irText();
    return true;
}

bool IRGenerator::emitObject(const NodeList& decls, Sema& sema,
                            const std::string& objPath, std::string* irOut,
                            std::string& errOut) {
    impl_->sema_ = &sema;
    return impl_->emitObject(decls, objPath, irOut, errOut,
                             optLevel_, optSize_, emitAsm_);
}

} // namespace suki
