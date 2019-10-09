/*
 *  Copyright (c) 2019 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */
#ifndef ZTF_SCAN_H
#define ZTF_SCAN_H

#include <pablo/pablo_kernel.h>
#include <kernel/core/kernel_builder.h>
#include "ztf-logic.h"

namespace kernel {

class LengthGroupCompressionMask : public MultiBlockKernel {
public:
    LengthGroupCompressionMask(const std::unique_ptr<kernel::KernelBuilder> & b,
                               EncodingInfo encodingScheme,
                               unsigned groupNo,
                               StreamSet * symbolMarks,
                               StreamSet * hashValues,
                               StreamSet * const byteData,
                               StreamSet * compressionMask, unsigned strideBlocks = 8);
    bool isCachable() const override { return true; }
    bool hasSignature() const override { return false; }
private:
    void generateMultiBlockLogic(const std::unique_ptr<kernel::KernelBuilder> & iBuilder, llvm::Value * const numOfStrides) override;
    EncodingInfo mEncodingScheme;
    unsigned mGroupNo;
};

class LengthGroupDecompression : public MultiBlockKernel {
public:
    LengthGroupDecompression(const std::unique_ptr<kernel::KernelBuilder> & b,
                             EncodingInfo encodingScheme,
                             unsigned groupNo,
                             StreamSet * keyMarks,
                             StreamSet * hashValues,
                             StreamSet * const hashMarks, StreamSet * const byteData,
                             StreamSet * const result, unsigned strideBlocks = 8);
    bool isCachable() const override { return true; }
    bool hasSignature() const override { return false; }
private:
    void generateMultiBlockLogic(const std::unique_ptr<kernel::KernelBuilder> & iBuilder, llvm::Value * const numOfStrides) override;
    EncodingInfo mEncodingScheme;
    unsigned mGroupNo;
};
    
    
class FixedLengthCompressionMask : public MultiBlockKernel {
public:
    FixedLengthCompressionMask(const std::unique_ptr<kernel::KernelBuilder> & b,
                               EncodingInfo encodingScheme,
                                                       unsigned length,
                                                       StreamSet * symbolMarks,
                                                       StreamSet * hashValues,
                                                       StreamSet * const byteData,
                               StreamSet * compressionMask, unsigned strideBlocks = 8);
    bool isCachable() const override { return true; }
    bool hasSignature() const override { return false; }
private:
    void generateMultiBlockLogic(const std::unique_ptr<kernel::KernelBuilder> & iBuilder, llvm::Value * const numOfStrides) override;
    EncodingInfo mEncodingScheme;
    unsigned mLength;
};

class FixedLengthDecompression : public MultiBlockKernel {
public:
    FixedLengthDecompression(const std::unique_ptr<kernel::KernelBuilder> & b,
                             EncodingInfo encodingScheme,
                             unsigned length,
                             StreamSet * keyMarks,
                             StreamSet * const hashValues,
                             StreamSet * const hashMarks, StreamSet * const byteData,
                             StreamSet * const result, unsigned strideBlocks = 8);
    bool isCachable() const override { return true; }
    bool hasSignature() const override { return false; }
private:
    void generateMultiBlockLogic(const std::unique_ptr<kernel::KernelBuilder> & iBuilder, llvm::Value * const numOfStrides) override;
    EncodingInfo mEncodingScheme;
    unsigned mLength;
};

}
#endif
