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
    MMapSourceKernel(const std::unique_ptr<kernel::KernelBuilder> & b, Scalar * const fd, StreamSet * const outputStream);
    bool isCachable() const override { return true; }
    bool hasSignature() const override { return false; }
    void linkExternalMethods(const std::unique_ptr<kernel::KernelBuilder> & iBuilder) override {
        mFileSizeFunction = linkFileSizeMethod(iBuilder);
    }
    void generateInitializeMethod(const std::unique_ptr<kernel::KernelBuilder> & iBuilder) override {
        generateInitializeMethod(mFileSizeFunction, mCodeUnitWidth, mStride, iBuilder);
    }
    void generateDoSegmentMethod(const std::unique_ptr<kernel::KernelBuilder> & iBuilder) override {
        generateDoSegmentMethod(mCodeUnitWidth, mStride, iBuilder);
    }
    void generateFinalizeMethod(const std::unique_ptr<kernel::KernelBuilder> & iBuilder) override {
        freeBuffer(iBuilder, mCodeUnitWidth);
    }
protected:
    static llvm::Function * linkFileSizeMethod(const std::unique_ptr<kernel::KernelBuilder> & b);
    static void generateInitializeMethod(llvm::Function * fileSize, const unsigned codeUnitWidth, const unsigned stride, const std::unique_ptr<kernel::KernelBuilder> & b);
    static void generateDoSegmentMethod(const unsigned codeUnitWidth, const unsigned stride, const std::unique_ptr<kernel::KernelBuilder> & b);
    static void freeBuffer(const std::unique_ptr<kernel::KernelBuilder> & iBuilder, const unsigned codeUnitWidth);
protected:
    const unsigned mCodeUnitWidth;
    llvm::Function * mFileSizeFunction;
};

class ReadSourceKernel final : public SegmentOrientedKernel {
    friend class FDSourceKernel;
public:
    ReadSourceKernel(const std::unique_ptr<kernel::KernelBuilder> & b, Scalar * const fd, StreamSet * const outputStream);
    bool isCachable() const override { return true; }
    bool hasSignature() const override { return false; }
    void generateInitializeMethod(const std::unique_ptr<kernel::KernelBuilder> & iBuilder) override {
        generateInitializeMethod(mCodeUnitWidth, mStride, iBuilder);
    }
    void generateDoSegmentMethod(const std::unique_ptr<kernel::KernelBuilder> & iBuilder) override {
        generateDoSegmentMethod(mCodeUnitWidth, mStride, iBuilder);
    }
    void generateFinalizeMethod(const std::unique_ptr<kernel::KernelBuilder> & iBuilder) override {
        freeBuffer(iBuilder);
    }
protected:
    static void generateInitializeMethod(const unsigned codeUnitWidth, const unsigned stride, const std::unique_ptr<kernel::KernelBuilder> & iBuilder);
    static void generateDoSegmentMethod(const unsigned codeUnitWidth, const unsigned stride, const std::unique_ptr<kernel::KernelBuilder> & iBuilder);
    static void freeBuffer(const std::unique_ptr<kernel::KernelBuilder> & iBuilder);
private:
    const unsigned mCodeUnitWidth;
};

class FDSourceKernel final : public SegmentOrientedKernel {
public:
    FDSourceKernel(const std::unique_ptr<kernel::KernelBuilder> & b, Scalar * const useMMap, Scalar * const fd, StreamSet * const outputStream);
    bool isCachable() const override { return true; }
    bool hasSignature() const override { return false; }
    void linkExternalMethods(const std::unique_ptr<kernel::KernelBuilder> & iBuilder) override;
    void generateInitializeMethod(const std::unique_ptr<kernel::KernelBuilder> & iBuilder) override;
    void generateDoSegmentMethod(const std::unique_ptr<kernel::KernelBuilder> & iBuilder) override;
    void generateFinalizeMethod(const std::unique_ptr<kernel::KernelBuilder> & iBuilder) override;
protected:
    const unsigned mCodeUnitWidth;
    llvm::Function * mFileSizeFunction;
};

class MemorySourceKernel final : public SegmentOrientedKernel {
public:
    MemorySourceKernel(const std::unique_ptr<kernel::KernelBuilder> & b, Scalar * fileSource, Scalar * fileItems, StreamSet * const outputStream);
    bool hasSignature() const override { return false; }
protected:
    void generateInitializeMethod(const std::unique_ptr<kernel::KernelBuilder> & iBuilder) override;
    void generateDoSegmentMethod(const std::unique_ptr<kernel::KernelBuilder> & iBuilder) override;
    void generateFinalizeMethod(const std::unique_ptr<kernel::KernelBuilder> & iBuilder) override;
private:
    const unsigned mStreamSetCount;
    const unsigned mCodeUnitWidth;
};

}

#endif // SOURCE_KERNEL_H