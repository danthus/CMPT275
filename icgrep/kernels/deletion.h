/*
 *  Copyright (c) 2016 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 */
#ifndef DELETION_H
#define DELETION_H

#include "kernel.h"
#include <llvm/IR/Value.h>
namespace IDISA { class IDISA_Builder; }

//
// Parallel Prefix Deletion 
// see Parallel Prefix Compress in Henry S. Warren, Hacker's Delight, Chapter 7
// 
// Given that we want to delete bits within fields of width fw, moving
// nondeleted bits to the right, the parallel prefix compress method can
// be applied.   This requires a preprocessing step to compute log2(fw) 
// masks that can be used to select bits to be moved in each step of the
// algorithm.
//

namespace kernel {

class DeletionKernel final : public BlockOrientedKernel {
public:
    DeletionKernel(const std::unique_ptr<kernel::KernelBuilder> & iBuilder, unsigned fw, unsigned streamCount);
    bool isCachable() const override { return true; }
    bool moduleIDisSignature() const override { return true; }
protected:
    void generateDoBlockMethod(const std::unique_ptr<KernelBuilder> & iBuilder) override;
    void generateFinalBlockMethod(const std::unique_ptr<KernelBuilder> & iBuilder, llvm::Value * remainingBytes) override;
private:
    const unsigned mDeletionFieldWidth;
    const unsigned mStreamCount;
};

class DeleteByPEXTkernel final : public BlockOrientedKernel {
public:
    DeleteByPEXTkernel(const std::unique_ptr<kernel::KernelBuilder> & iBuilder, unsigned fw, unsigned streamCount, bool shouldSwizzle);
    bool isCachable() const override { return true; }
    bool moduleIDisSignature() const override { return true; }
protected:
    void generateDoBlockMethod(const std::unique_ptr<KernelBuilder> & iBuilder) override;
    void generateFinalBlockMethod(const std::unique_ptr<KernelBuilder> & iBuilder, llvm::Value * remainingBytes) override;
    void generatePEXTAndSwizzleLoop(const std::unique_ptr<KernelBuilder> & iBuilder, const std::vector<llvm::Value *> & masks);
    void generatePEXTLoop(const std::unique_ptr<KernelBuilder> & iBuilder, const std::vector<llvm::Value *> & masks);
    void generateProcessingLoop(const std::unique_ptr<KernelBuilder> & iBuilder, const std::vector<llvm::Value *> & masks, llvm::Value * delMask);
private:
    const unsigned mDelCountFieldWidth;
    const unsigned mStreamCount;
    const unsigned mSwizzleFactor;
    const bool mShouldSwizzle;
    static constexpr const char* mOutputSwizzleNameBase = "outputStreamSet";
};
    
class SwizzledBitstreamCompressByCount final : public BlockOrientedKernel {
public:
    SwizzledBitstreamCompressByCount(const std::unique_ptr<kernel::KernelBuilder> & iBuilder, unsigned bitStreamCount, unsigned fieldWidth = 64);
    bool isCachable() const override { return true; }
    bool moduleIDisSignature() const override { return true; }
protected:
    void generateDoBlockMethod(const std::unique_ptr<KernelBuilder> & iBuilder) override;
    void generateFinalBlockMethod(const std::unique_ptr<KernelBuilder> & iBuilder, llvm::Value * remainingBytes) override;
private:
    const unsigned mBitStreamCount;
    const unsigned mFieldWidth;
    const unsigned mSwizzleFactor;
    const unsigned mSwizzleSetCount;
};

    
}
    
#endif

