/*
 *  Copyright (c) 2016 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 */
#ifndef EDITD_CPU_KERNEL_H
#define EDITD_CPU_KERNEL_H

#include <kernel/core/kernel.h>

namespace llvm { class Module; }

namespace IDISA { class IDISA_Builder; }

namespace kernel {

class editdCPUKernel : public BlockOrientedKernel {
public:

    editdCPUKernel(const std::unique_ptr<kernel::KernelBuilder> & b,
                   const unsigned patternLen, const unsigned groupSize,
                   Scalar * const pattStream,
                   StreamSet * const CCStream, StreamSet * const ResultStream);


private:
    void generateDoBlockMethod(const std::unique_ptr<kernel::KernelBuilder> & idb) override;
    void generateFinalBlockMethod(const std::unique_ptr<kernel::KernelBuilder> & idb, llvm::Value * remainingBytes) override;
    void bitblock_advance_ci_co(const std::unique_ptr<KernelBuilder> & idb, llvm::Value * val, unsigned shift, llvm::Value * stideCarryArr, unsigned carryIdx, std::vector<std::vector<llvm::Value *>> & adv, std::vector<std::vector<int>> & calculated, int i, int j) const;
    void reset_to_zero(std::vector<std::vector<int>> & calculated);
    unsigned mEditDistance;
    unsigned mPatternLen;
    unsigned mGroupSize;

};



}
#endif