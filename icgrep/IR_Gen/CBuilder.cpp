/*
 *  Copyright (c) 2016 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#include "CBuilder.h"
#include <llvm/IR/Module.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/TypeBuilder.h>

using namespace llvm;

// ssize_t write(int fildes, const void *buf, size_t nbyte);
Value * CBuilder::CreateWriteCall(Value * fildes, Value * buf, Value * nbyte) {
    Function * write = mMod->getFunction("write");
    if (write == nullptr) {
        IntegerType * sizeTy = getSizeTy();
        IntegerType * int32Ty = getInt32Ty();
        PointerType * int8PtrTy = getInt8PtrTy();
        write = cast<Function>(mMod->getOrInsertFunction("write",
                                                        AttributeSet().addAttribute(mMod->getContext(), 2U, Attribute::NoAlias),
                                                        sizeTy, int32Ty, int8PtrTy, sizeTy, nullptr));
    }
    return CreateCall(write, {fildes, buf, nbyte});
}

Function * CBuilder::GetPrintf() {
    Function * printf = mMod->getFunction("printf");
    if (printf == nullptr) {
        printf = cast<Function>(mMod->getOrInsertFunction("printf"
                                , FunctionType::get(getInt32Ty(), {getInt8PtrTy()}, true)
                                , AttributeSet().addAttribute(getContext(), 1U, Attribute::NoAlias)));

    }
    return printf;
}

void CBuilder::CallPrintInt(const std::string & name, Value * const value) {
    Constant * printRegister = mMod->getFunction("PrintInt");
    if (LLVM_UNLIKELY(printRegister == nullptr)) {
        FunctionType *FT = FunctionType::get(getVoidTy(), { PointerType::get(getInt8Ty(), 0), getSizeTy() }, false);
        Function * function = Function::Create(FT, Function::InternalLinkage, "PrintInt", mMod);
        auto arg = function->arg_begin();
        std::string out = "%-40s = %" PRIx64 "\n";
        BasicBlock * entry = BasicBlock::Create(mMod->getContext(), "entry", function);
        IRBuilder<> builder(entry);
        std::vector<Value *> args;
        args.push_back(CreateGlobalStringPtr(out.c_str()));
        Value * const name = &*(arg++);
        name->setName("name");
        args.push_back(name);
        Value * value = &*arg;
        value->setName("value");
        args.push_back(value);
        builder.CreateCall(GetPrintf(), args);
        builder.CreateRetVoid();

        printRegister = function;
    }
    Value * num = nullptr;
    if (value->getType()->isPointerTy()) {
        num = CreatePtrToInt(value, getSizeTy());
    } else {
        num = CreateZExtOrBitCast(value, getSizeTy());
    }
    assert (num->getType()->isIntegerTy());
    CreateCall(printRegister, {CreateGlobalStringPtr(name.c_str()), num});
}

Value * CBuilder::CreateMalloc(Type * type, Value * size) {
    DataLayout DL(getModule());
    Type * const intTy = getIntPtrTy(DL);
    Constant * const width = ConstantExpr::getSizeOf(type);
    if (size->getType() != intTy) {
        if (isa<Constant>(size)) {
            size = ConstantExpr::getIntegerCast(cast<Constant>(size), intTy, false);
        } else {
            size = CreateTruncOrBitCast(size, intTy);
        }
    }
    if (!width->isOneValue()) {
        if (isa<Constant>(size)) {
            size = ConstantExpr::getMul(cast<Constant>(size), width);
        } else {
            size = CreateMul(size, width);
        }
    }
    Module * const m = getModule();
    Function * malloc = m->getFunction("malloc");
    if (malloc == nullptr) {
        PointerType * const voidPtrTy = getVoidPtrTy();
        malloc = cast<Function>(m->getOrInsertFunction("malloc", voidPtrTy, intTy, nullptr));
        malloc->setCallingConv(CallingConv::C);
        malloc->setDoesNotAlias(0);
    }
    CallInst * ci = CreateCall(malloc, size);
    ci->setTailCall();
    ci->setCallingConv(malloc->getCallingConv());
    return CreateBitOrPointerCast(ci, type->getPointerTo());
}

Value * CBuilder::CreateAlignedMalloc(Type * type, Value * size, const unsigned alignment) {
    assert ((alignment & (alignment - 1)) == 0); // is power of 2
    DataLayout DL(getModule());
    IntegerType * const intTy = getIntPtrTy(DL);
    const auto byteWidth = (intTy->getBitWidth() / 8);
    Constant * const offset = ConstantInt::get(intTy, alignment + byteWidth - 1);
    Constant * const width = ConstantExpr::getSizeOf(type);
    if (!width->isOneValue()) {
        if (isa<Constant>(size)) {
            size = ConstantExpr::getMul(cast<Constant>(size), width);
        } else {
            size = CreateMul(size, width);
        }
    }
    if (size->getType() != intTy) {
        if (isa<Constant>(size)) {
            size = ConstantExpr::getIntegerCast(cast<Constant>(size), intTy, false);
        } else {
            size = CreateTruncOrBitCast(size, intTy);
        }
    }
    if (isa<Constant>(size)) {
        size = ConstantExpr::getAdd(cast<Constant>(size), offset);
    } else {
        size = CreateAdd(size, offset);
    }
    Value * unaligned = CreatePtrToInt(CreateMalloc(getInt8Ty(), size), intTy);
    Value * aligned = CreateAnd(CreateAdd(unaligned, offset), ConstantExpr::getNot(ConstantInt::get(intTy, alignment - 1)));
    Value * prefix = CreateIntToPtr(CreateSub(aligned, ConstantInt::get(intTy, byteWidth)), intTy->getPointerTo());
    assert (unaligned->getType() == prefix->getType()->getPointerElementType());
    CreateAlignedStore(unaligned, prefix, byteWidth);
    return CreateIntToPtr(aligned, type->getPointerTo());
}

void CBuilder::CreateFree(Value * const ptr) {
    assert (ptr->getType()->isPointerTy());
    Module * const m = getModule();
    PointerType * const voidPtrTy = getVoidPtrTy();
    Function * free = m->getFunction("free");
    if (free == nullptr) {
        free = cast<Function>(getModule()->getOrInsertFunction("free", getVoidTy(), voidPtrTy, nullptr));
        free->setCallingConv(CallingConv::C);
    }
    CallInst * const ci = CreateCall(free, CreatePointerCast(ptr, voidPtrTy));
    ci->setTailCall();
    ci->setCallingConv(free->getCallingConv());
}

void CBuilder::CreateAlignedFree(Value * const ptr, const bool ptrMayBeNull) {
    // WARNING: this will cause a segfault if the value of the ptr at runtime is null but ptrMayBeNull was not set
    PointerType * type = cast<PointerType>(ptr->getType());
    BasicBlock * exit = nullptr;
    if (ptrMayBeNull) {
        LLVMContext & C = getContext();
        BasicBlock * bb = GetInsertBlock();
        Function * f = bb->getParent();
        exit = BasicBlock::Create(C, "", f, bb);
        BasicBlock * entry = BasicBlock::Create(C, "", f, exit);
        Value * cond = CreateICmpEQ(ptr, ConstantPointerNull::get(type));
        CreateCondBr(cond, exit, entry);
        SetInsertPoint(entry);
    }
    DataLayout DL(getModule());
    IntegerType * const intTy = getIntPtrTy(DL);
    const auto byteWidth = (intTy->getBitWidth() / 8);
    Value * prefix = CreatePtrToInt(ptr, intTy);
    prefix = CreateSub(prefix, ConstantInt::get(intTy, byteWidth));
    prefix = CreateIntToPtr(prefix, intTy->getPointerTo());
    prefix = CreateIntToPtr(CreateAlignedLoad(prefix, byteWidth), type);
    CreateFree(prefix);
    if (ptrMayBeNull) {
        CreateBr(exit);
        SetInsertPoint(exit);
    }
}

Value * CBuilder::CreateRealloc(Value * ptr, Value * size) {
    DataLayout DL(getModule());
    Type * const intTy = getIntPtrTy(DL);
    PointerType * type = cast<PointerType>(ptr->getType());
    Constant * const width = ConstantExpr::getSizeOf(type->getPointerElementType());
    if (size->getType() != intTy) {
        if (isa<Constant>(size)) {
            size = ConstantExpr::getIntegerCast(cast<Constant>(size), intTy, false);
        } else {
            size = CreateTruncOrBitCast(size, intTy);
        }
    }
    if (!width->isOneValue()) {
        if (isa<Constant>(size)) {
            size = ConstantExpr::getMul(cast<Constant>(size), width);
        } else {
            size = CreateMul(size, width);
        }
    }
    Module * const m = getModule();
    Function * realloc = m->getFunction("realloc");
    if (realloc == nullptr) {
        PointerType * const voidPtrTy = getVoidPtrTy();
        realloc = cast<Function>(m->getOrInsertFunction("realloc", voidPtrTy, voidPtrTy, intTy, nullptr));
        realloc->setCallingConv(CallingConv::C);
        realloc->setDoesNotAlias(1);
    }
    CallInst * ci = CreateCall(realloc, {ptr, size});
    ci->setTailCall();
    ci->setCallingConv(realloc->getCallingConv());
    return CreateBitOrPointerCast(ci, type);
}

void CBuilder::CreateMemZero(Value * ptr, Value * size, const unsigned alignment) {
    assert (ptr->getType()->isPointerTy() && size->getType()->isIntegerTy());
    Type * const type = ptr->getType();
    Constant * const width = ConstantExpr::getSizeOf(type->getPointerElementType());
    if (isa<Constant>(size)) {
        size = ConstantExpr::getMul(cast<Constant>(size), width);
    } else {
        size = CreateMul(size, width);
    }
    CreateMemSet(CreatePointerCast(ptr, getInt8PtrTy()), getInt8(0), size, alignment);
}

PointerType * CBuilder::getVoidPtrTy() const {
    return TypeBuilder<void *, false>::get(getContext());
}

LoadInst * CBuilder::CreateAtomicLoadAcquire(Value * ptr) {
    unsigned alignment = dyn_cast<PointerType>(ptr->getType())->getElementType()->getPrimitiveSizeInBits()/8;
    LoadInst * inst = CreateAlignedLoad(ptr, alignment);
    inst->setOrdering(AtomicOrdering::Acquire);
    return inst;
    
}
StoreInst * CBuilder::CreateAtomicStoreRelease(Value * val, Value * ptr) {
    unsigned alignment = dyn_cast<PointerType>(ptr->getType())->getElementType()->getPrimitiveSizeInBits()/8;
    StoreInst * inst = CreateAlignedStore(val, ptr, alignment);
    inst->setOrdering(AtomicOrdering::Release);
    return inst;
}

Value * CBuilder::CreatePThreadCreateCall(Value * thread, Value * attr, Function * start_routine, Value * arg) {
    Function * pthreadCreateFunc = mMod->getFunction("pthread_create");
    if (pthreadCreateFunc == nullptr) {
        Type * pthreadTy = getSizeTy();
        FunctionType * funVoidPtrVoidTy = FunctionType::get(getVoidTy(), getVoidPtrTy(), false);

        pthreadCreateFunc = cast<Function>(mMod->getOrInsertFunction("pthread_create",
                                                                     getInt32Ty(),
                                                                     pthreadTy->getPointerTo(),
                                                                     getVoidPtrTy(),
                                                                     static_cast<Type *>(funVoidPtrVoidTy)->getPointerTo(),
                                                                     getVoidPtrTy(), nullptr));
        pthreadCreateFunc->setCallingConv(llvm::CallingConv::C);
    }
    return CreateCall(pthreadCreateFunc, {thread, attr, start_routine, arg});
}

Value * CBuilder::CreatePThreadExitCall(Value * value_ptr) {
    Function * pthreadExitFunc = mMod->getFunction("pthread_exit");
    if (pthreadExitFunc == nullptr) {
        pthreadExitFunc = cast<Function>(mMod->getOrInsertFunction("pthread_exit", getVoidTy(), getVoidPtrTy(), nullptr));
        pthreadExitFunc->addFnAttr(llvm::Attribute::NoReturn);
        pthreadExitFunc->setCallingConv(llvm::CallingConv::C);
    }
    CallInst * exitThread = CreateCall(pthreadExitFunc, {value_ptr});
    exitThread->setDoesNotReturn();
    return exitThread;
}

Value * CBuilder::CreatePThreadJoinCall(Value * thread, Value * value_ptr){
    Type * pthreadTy = getSizeTy();
    Function * pthreadJoinFunc = cast<Function>(mMod->getOrInsertFunction("pthread_join",
                                                                       getInt32Ty(),
                                                                       pthreadTy,
                                                                       getVoidPtrTy()->getPointerTo(), nullptr));
    pthreadJoinFunc->setCallingConv(llvm::CallingConv::C);
    return CreateCall(pthreadJoinFunc, {thread, value_ptr});
}

CBuilder::CBuilder(llvm::Module * m, unsigned GeneralRegisterWidthInBits, unsigned CacheLineAlignmentInBytes)
: IRBuilder<>(m->getContext())
, mMod(m)
, mCacheLineAlignment(CacheLineAlignmentInBytes)
, mSizeType(getIntNTy(GeneralRegisterWidthInBits)) {
}