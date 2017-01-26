/*
 *  Copyright (c) 2016 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 */
#ifndef CBUILDER_H
#define CBUILDER_H

#include <string>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Constants.h>
namespace llvm { class Function; }
namespace llvm { class IntegerType; }
namespace llvm { class Module; }
namespace llvm { class PointerType; }
namespace llvm { class Type; }
namespace llvm { class Value; }

class CBuilder : public llvm::IRBuilder<> {
    
public:
    
    CBuilder(llvm::Module * m, unsigned GeneralRegisterWidthInBits, unsigned CacheLineAlignmentInBytes = 64);
    
    virtual ~CBuilder() {}

    llvm::Module * getModule() const {
        return mMod;
    }
    
    void setModule(llvm::Module * m)  {
        mMod = m;
    }
        
    llvm::Value * CreateMalloc(llvm::Type * type, llvm::Value * size);

    llvm::Value * CreateAlignedMalloc(llvm::Type * type, llvm::Value * size, const unsigned alignment);

    void CreateFree(llvm::Value * const ptr);

    void CreateAlignedFree(llvm::Value * const ptr, const bool ptrMayBeNull = false);

    llvm::Value * CreateRealloc(llvm::Value * ptr, llvm::Value * size);

    void CreateMemZero(llvm::Value * ptr, llvm::Value * size, const unsigned alignment = 1);

    inline llvm::AllocaInst * CreateCacheAlignedAlloca(llvm::Type * Ty, llvm::Value * ArraySize = nullptr) {
        llvm::AllocaInst * instr = CreateAlloca(Ty, ArraySize);
        instr->setAlignment(getCacheAlignment());
        return instr;
    }
    
    llvm::Function * GetPrintf();

    llvm::Value * CreateWriteCall(llvm::Value * fildes, llvm::Value * buf, llvm::Value * nbyte);
    
    llvm::Value * CreatePThreadCreateCall(llvm::Value * thread, llvm::Value * attr, llvm::Function * start_routine, llvm::Value * arg);

    llvm::Value * CreatePThreadExitCall(llvm::Value * value_ptr);

    llvm::Value * CreatePThreadJoinCall(llvm::Value * thread, llvm::Value * value_ptr);
    
    void CallPrintInt(const std::string & name, llvm::Value * const value);
    
    inline llvm::IntegerType * getSizeTy() const {
        return mSizeType;
    }
    
    inline llvm::ConstantInt * getSize(const size_t value) {
        return llvm::ConstantInt::get(getSizeTy(), value);
    }
    
    llvm::PointerType * getVoidPtrTy() const;
    
    inline unsigned getCacheAlignment() const {
        return mCacheLineAlignment;
    }
    
    virtual llvm::LoadInst* CreateAtomicLoadAcquire(llvm::Value * ptr);

    virtual llvm::StoreInst *  CreateAtomicStoreRelease(llvm::Value * val, llvm::Value * ptr);
    
protected:
    llvm::Module *      mMod;
    unsigned            mCacheLineAlignment;
    llvm::IntegerType * mSizeType;
};

#endif