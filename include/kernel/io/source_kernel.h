#ifndef SOURCE_KERNEL_H
#define SOURCE_KERNEL_H

#include <kernel/core/kernel.h>
namespace kernel { class KernelBuilder; }

namespace kernel {

/* The MMapSourceKernel is a simple wrapper for an external MMap file buffer.
   The doSegment method of this kernel feeds one segment at a time to a
   pipeline. */

class MMapSourceKernel final : public SegmentOrientedKernel {
    friend class FDSourceKernel;
public:
    MMapSourceKernel(BuilderRef b, Scalar * const fd, StreamSet * const outputStream);
    bool isCachable() const override { return true; }
    bool hasSignature() const override { return false; }
    void linkExternalMethods(BuilderRef b) override {
        mFileSizeFunction = linkFileSizeMethod(b);
    }
    void generateInitializeMethod(BuilderRef b) override {
        generateInitializeMethod(mFileSizeFunction, mCodeUnitWidth, mStride, b);
    }
    void generateDoSegmentMethod(BuilderRef b) override {
        generateDoSegmentMethod(mCodeUnitWidth, mStride, b);
    }
    void generateFinalizeMethod(BuilderRef b) override {
        freeBuffer(b, mCodeUnitWidth);
    }
protected:
    static llvm::Function * linkFileSizeMethod(BuilderRef b);
    static void generateInitializeMethod(llvm::Function * fileSize, const unsigned codeUnitWidth, const unsigned stride, BuilderRef b);
    static void generateDoSegmentMethod(const unsigned codeUnitWidth, const unsigned stride, BuilderRef b);
    static void freeBuffer(BuilderRef b, const unsigned codeUnitWidth);
protected:
    const unsigned mCodeUnitWidth;
    llvm::Function * mFileSizeFunction;
};

class ReadSourceKernel final : public SegmentOrientedKernel {
    friend class FDSourceKernel;
public:
    ReadSourceKernel(BuilderRef b, Scalar * const fd, StreamSet * const outputStream);
    bool isCachable() const override { return true; }
    bool hasSignature() const override { return false; }
    void generateInitializeMethod(BuilderRef b) override {
        generateInitializeMethod(mCodeUnitWidth, mStride, b);
    }
    void generateDoSegmentMethod(BuilderRef b) override {
        generateDoSegmentMethod(mCodeUnitWidth, mStride, b);
    }
    void generateFinalizeMethod(BuilderRef b) override {
        freeBuffer(b);
    }
protected:
    static void generateInitializeMethod(const unsigned codeUnitWidth, const unsigned stride, BuilderRef b);
    static void generateDoSegmentMethod(const unsigned codeUnitWidth, const unsigned stride, BuilderRef b);
    static void freeBuffer(BuilderRef b);
    static void createInternalBuffer(BuilderRef b);
private:
    const unsigned mCodeUnitWidth;
};

class FDSourceKernel final : public SegmentOrientedKernel {
public:
    FDSourceKernel(BuilderRef b, Scalar * const useMMap, Scalar * const fd, StreamSet * const outputStream);
    bool isCachable() const override { return true; }
    bool hasSignature() const override { return false; }
    void linkExternalMethods(BuilderRef b) override;
    void generateInitializeMethod(BuilderRef b) override;
    void generateDoSegmentMethod(BuilderRef b) override;
    void generateFinalizeMethod(BuilderRef b) override;
protected:
    const unsigned mCodeUnitWidth;
    llvm::Function * mFileSizeFunction;
};

class MemorySourceKernel final : public SegmentOrientedKernel {
public:
    MemorySourceKernel(BuilderRef b, Scalar * fileSource, Scalar * fileItems, StreamSet * const outputStream);
    bool hasSignature() const override { return false; }
protected:
    void generateInitializeMethod(BuilderRef b) override;
    void generateDoSegmentMethod(BuilderRef b) override;
    void generateFinalizeMethod(BuilderRef b) override;
};

}

#endif // SOURCE_KERNEL_H
