/*
 *  Copyright (c) 2016 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 */

#ifndef KERNEL_BUILDER_H
#define KERNEL_BUILDER_H

#include <string>           // for string
#include <memory>           // for unique_ptr
#include "interface.h"      // for KernelInterface
#include <boost/container/flat_map.hpp>
#include <IR_Gen/idisa_builder.h>

//namespace llvm { class ConstantInt; }
#include <llvm/IR/Constants.h>
namespace llvm { class Function; }
namespace llvm { class IntegerType; }
namespace llvm { class LoadInst; }
namespace llvm { class Type; }
namespace llvm { class Value; }
namespace parabix { class StreamSetBuffer; }

namespace kernel {
    
class KernelBuilder : public KernelInterface {
protected:
    using KernelMap = boost::container::flat_map<std::string, unsigned>;
    enum class Port { Input, Output };
    using StreamPort = std::pair<Port, unsigned>;
    using StreamMap = boost::container::flat_map<std::string, StreamPort>;
    using StreamSetBuffers = std::vector<parabix::StreamSetBuffer *>;
public:
    
    // Kernel Signatures and Module IDs
    //
    // A kernel signature uniquely identifies a kernel and its full functionality.
    // In the event that a particular kernel instance is to be generated and compiled
    // to produce object code, and we have a cached kernel object code instance with 
    // the same signature and targetting the same IDISA architecture, then the cached 
    // object code may safely be used to avoid recompilation.
    //
    // A kernel signature is a byte string of arbitrary length.
    //
    // Kernel developers should take responsibility for designing appropriate signature
    // mechanisms that are short, inexpensive to compute and guarantee uniqueness
    // based on the semantics of the kernel.  
    //
    // If no other mechanism is available, the default generateKernelSignature() method
    // uses the full LLVM IR (before optimization) of the kernel instance.
    //
    // A kernel Module ID is short string that is used as a name for a particular kernel
    // instance.  Kernel Module IDs are used to look up and retrieve cached kernel instances
    // and so should be highly likely to uniquely identify a kernel instance.
    //
    // The ideal case is that a kernel Module ID serves as a full kernel signature thus
    // guaranteeing uniqueness.  In this case, the moduleIDisUnique() method 
    // should return true.
    //
    
    // Can the module ID itself serve as the unique signature?
    virtual bool moduleIDisSignature() { return false; }
    
    virtual std::string generateKernelSignature(std::string moduleId);
    
    // Create a module stub for the kernel, populated only with its Module ID.     
    //
    llvm::Module * createKernelStub(const StreamSetBuffers & inputs, const StreamSetBuffers & outputs);
      
    void setCallParameters(const StreamSetBuffers & inputs, const StreamSetBuffers & outputs);

    // Generate the Kernel to the current module (iBuilder->getModule()).
    void generateKernel();
    
    void createInstance() override;

    llvm::Value * getProducedItemCount(llvm::Value * instance, const std::string & name, llvm::Value * doFinal = nullptr) const final;

    void setProducedItemCount(llvm::Value * instance, const std::string & name, llvm::Value * value) const final;

    llvm::Value * getProcessedItemCount(llvm::Value * instance, const std::string & name) const final;

    void setProcessedItemCount(llvm::Value * instance, const std::string & name, llvm::Value * value) const final;

    bool hasNoTerminateAttribute() { return mNoTerminateAttribute;}
    
    llvm::Value * getTerminationSignal(llvm::Value * instance) const final;

    void setTerminationSignal(llvm::Value * instance) const final;

    // Get the value of a scalar field for a given instance.
    llvm::Value * getScalarField(llvm::Value * instance, const std::string & fieldName) const;

    llvm::Value * getScalarField(llvm::Value * instance, llvm::Value * index) const;

    // Set the value of a scalar field for a given instance.
    void setScalarField(llvm::Value *instance, const std::string & fieldName, llvm::Value * value) const;

    void setScalarField(llvm::Value * instance, llvm::Value * index, llvm::Value * value) const;

    // Synchronization actions for executing a kernel for a particular logical segment.
    //
    // Before the segment is processed, acquireLogicalSegmentNo must be used to load
    // the segment number of the kernel state to ensure that the previous segment is
    // complete (by checking that the acquired segment number is equal to the desired segment
    // number).
    // After all segment processing actions for the kernel are complete, and any necessary
    // data has been extracted from the kernel for further pipeline processing, the
    // segment number must be incremented and stored using releaseLogicalSegmentNo.
    llvm::LoadInst * acquireLogicalSegmentNo(llvm::Value * instance) const;

    void releaseLogicalSegmentNo(llvm::Value * instance, llvm::Value * newFieldVal) const;

    // Get a parameter by name.
    llvm::Argument * getParameter(llvm::Function * f, const std::string & name) const;

    inline llvm::IntegerType * getSizeTy() const {
        return getBuilder()->getSizeTy();
    }

    inline llvm::Type * getStreamTy(const unsigned FieldWidth = 1) {
        return getBuilder()->getStreamTy(FieldWidth);
    }
    
    inline llvm::Type * getStreamSetTy(const unsigned NumElements = 1, const unsigned FieldWidth = 1) {
        return getBuilder()->getStreamSetTy(NumElements, FieldWidth);
    }
    
    virtual ~KernelBuilder() = 0;
    
    const std::vector<const parabix::StreamSetBuffer *> & getStreamSetInputBuffers() const { return mStreamSetInputBuffers; }

    const parabix::StreamSetBuffer * getStreamSetInputBuffer(const unsigned i) const { return mStreamSetInputBuffers[i]; }

    const std::vector<const parabix::StreamSetBuffer *> & getStreamSetOutputBuffers() const { return mStreamSetOutputBuffers; }

    const parabix::StreamSetBuffer * getStreamSetOutputBuffer(const unsigned i) const { return mStreamSetOutputBuffers[i]; }

    llvm::Value * createDoSegmentCall(const std::vector<llvm::Value *> & args) const;

    llvm::Value * createGetAccumulatorCall(llvm::Value * self, const std::string & accumName) const;

protected:

    // Constructor
    KernelBuilder(IDISA::IDISA_Builder * builder,
                    std::string && kernelName,
                    std::vector<Binding> && stream_inputs,
                    std::vector<Binding> && stream_outputs,
                    std::vector<Binding> && scalar_parameters,
                    std::vector<Binding> && scalar_outputs,
                    std::vector<Binding> && internal_scalars);

    //
    // Kernel builder subtypes define their logic of kernel construction
    // in terms of 3 virtual methods for
    // (a) preparing the Kernel state data structure
    // (b) defining the logic of the doBlock function, and
    // (c) defining the logic of the finalBlock function.
    //
    // Note: the kernel state data structure must only be finalized after
    // all scalar fields have been added.   If there are no fields to
    // be added, the default method for preparing kernel state may be used.
    
    void setNoTerminateAttribute(const bool noTerminate = true) {
        mNoTerminateAttribute = noTerminate;
    }

    void prepareStreamSetNameMap();

    virtual void prepareKernel();

    virtual void generateInitMethod() { }
    
    virtual void generateDoSegmentMethod(llvm::Value * doFinal, const std::vector<llvm::Value *> & producerPos) = 0;

    // Add an additional scalar field to the KernelState struct.
    // Must occur before any call to addKernelDeclarations or createKernelModule.
    unsigned addScalar(llvm::Type * type, const std::string & name);

    unsigned addUnnamedScalar(llvm::Type * type);

    unsigned getScalarCount() const;

    // Run-time access of Kernel State and parameters of methods for
    // use in implementing kernels.
    
    // Get the index of a named scalar field within the kernel state struct.
    llvm::ConstantInt * getScalarIndex(const std::string & name) const;

    // Get the value of a scalar field for a given instance.
    llvm::Value * getScalarField(const std::string & fieldName) const {
        return getScalarField(getSelf(), fieldName);
    }

    llvm::Value * getScalarField(llvm::Value * index) const {
        return getScalarField(getSelf(), index);
    }

    // Set the value of a scalar field for a given instance.
    void setScalarField(const std::string & fieldName, llvm::Value * value) const {
        return setScalarField(getSelf(), fieldName, value);
    }

    void setScalarField(llvm::Value * index, llvm::Value * value) const {
        return setScalarField(getSelf(), index, value);
    }

    llvm::Value * getInputStreamBlockPtr(const std::string & name, llvm::Value * streamIndex) const;

    llvm::Value * loadInputStreamBlock(const std::string & name, llvm::Value * streamIndex) const;
    
    llvm::Value * getInputStreamPackPtr(const std::string & name, llvm::Value * streamIndex, llvm::Value * packIndex) const;
    
    llvm::Value * loadInputStreamPack(const std::string & name, llvm::Value * streamIndex, llvm::Value * packIndex) const;
    
    llvm::Value * getInputStreamSetCount(const std::string & name) const;

    llvm::Value * getOutputStreamBlockPtr(const std::string & name, llvm::Value * streamIndex) const;
    
    void storeOutputStreamBlock(const std::string & name, llvm::Value * streamIndex, llvm::Value * toStore) const;
    
    llvm::Value * getOutputStreamPackPtr(const std::string & name, llvm::Value * streamIndex, llvm::Value * packIndex) const;
    
    void storeOutputStreamPack(const std::string & name, llvm::Value * streamIndex, llvm::Value * packIndex, llvm::Value * toStore) const;

    llvm::Value * getOutputStreamSetCount(const std::string & name) const;

    llvm::Value * getAdjustedInputStreamBlockPtr(llvm::Value * blockAdjustment, const std::string & name, llvm::Value * streamIndex) const;

    llvm::Value * getRawInputPointer(const std::string & name, llvm::Value * streamIndex, llvm::Value * absolutePosition) const;

    llvm::Value * getRawOutputPointer(const std::string & name, llvm::Value * streamIndex, llvm::Value * absolutePosition) const;

    void setBaseAddress(const std::string & name, llvm::Value * addr) const;

    llvm::Value * getBufferedSize(const std::string & name) const;

    void setBufferedSize(const std::string & name, llvm::Value * size) const;

    void reserveBytes(const std::string & name, llvm::Value * requested) const;

    llvm::Value * getScalarFieldPtr(const std::string & name) const {
        return getScalarFieldPtr(getSelf(), name);
    }

    llvm::Value * getScalarFieldPtr(llvm::Value * index) const {
        return getScalarFieldPtr(getSelf(), index);
    }

    inline llvm::Value * getProducedItemCount(const std::string & name) const {
        return getProducedItemCount(getSelf(), name);
    }

    inline void setProducedItemCount(const std::string & name, llvm::Value * value) const {
        setProducedItemCount(getSelf(), name, value);
    }

    llvm::Value * getAvailableItemCount(const std::string & name) const;

    inline llvm::Value * getProcessedItemCount(const std::string & name) const {
        return getProcessedItemCount(getSelf(), name);
    }

    inline void setProcessedItemCount(const std::string & name, llvm::Value * value) const {
        setProcessedItemCount(getSelf(), name, value);
    }

    llvm::Value * getConsumedItemCount(const std::string & name) const;

    llvm::Value * getTerminationSignal() const {
        return getTerminationSignal(getSelf());
    }

    void setTerminationSignal() const {
        return setTerminationSignal(getSelf());
    }

    llvm::Value * getSelf() const {
        return mSelf;
    }

    llvm::BasicBlock * CreateBasicBlock(std::string && name) const;

    // Stream set helpers.

    llvm::Value * getStreamSetBufferPtr(const std::string & name) const;

    llvm::Value * getScalarFieldPtr(llvm::Value * instance, const std::string & name) const;

    llvm::Value * getScalarFieldPtr(llvm::Value * instance, llvm::Value * index) const;

    StreamPort getStreamPort(const std::string & name) const;

    const parabix::StreamSetBuffer * getInputStreamSetBuffer(const std::string & name) const {
        const auto port = getStreamPort(name);
        assert (port.first == Port::Input);
        assert (port.second < mStreamSetInputBuffers.size());
        return mStreamSetInputBuffers[port.second];
    }

    const parabix::StreamSetBuffer * getOutputStreamSetBuffer(const std::string & name) const {
        const auto port = getStreamPort(name);
        assert (port.first == Port::Output);
        assert (port.second < mStreamSetOutputBuffers.size());
        return mStreamSetOutputBuffers[port.second];
    }

    void callGenerateInitMethod();

    void callGenerateDoSegmentMethod();

private:

    llvm::Value * computeBlockIndex(const std::vector<Binding> & binding, const std::string & name, llvm::Value * itemCount) const;

protected:

    llvm::Value *                                   mSelf;
    llvm::Function *                                mCurrentMethod;

    std::vector<llvm::Type *>                       mKernelFields;
    KernelMap                                       mKernelMap;
    StreamMap                                       mStreamMap;
    std::vector<const parabix::StreamSetBuffer *>   mStreamSetInputBuffers;
    std::vector<const parabix::StreamSetBuffer *>   mStreamSetOutputBuffers;
    bool                                            mNoTerminateAttribute;
    bool                                            mIsGenerated;

};

class SegmentOrientedKernel : public KernelBuilder {
protected:

    SegmentOrientedKernel(IDISA::IDISA_Builder * builder,
                          std::string && kernelName,
                          std::vector<Binding> && stream_inputs,
                          std::vector<Binding> && stream_outputs,
                          std::vector<Binding> && scalar_parameters,
                          std::vector<Binding> && scalar_outputs,
                          std::vector<Binding> && internal_scalars);

};

class BlockOrientedKernel : public KernelBuilder {
protected:

    void CreateDoBlockMethodCall();

    // Each kernel builder subtype must provide its own logic for generating
    // doBlock calls.
    virtual void generateDoBlockMethod() = 0;

    // Each kernel builder subtypre must also specify the logic for processing the
    // final block of stream data, if there is any special processing required
    // beyond simply calling the doBlock function.   In the case that the final block
    // processing may be trivially implemented by dispatching to the doBlock method
    // without additional preparation, the default generateFinalBlockMethod need
    // not be overridden.

    virtual void generateFinalBlockMethod(llvm::Value * remainingItems);

    void generateDoSegmentMethod(llvm::Value * doFinal, const std::vector<llvm::Value *> & producerPos) override final;

    BlockOrientedKernel(IDISA::IDISA_Builder * builder,
                        std::string && kernelName,
                        std::vector<Binding> && stream_inputs,
                        std::vector<Binding> && stream_outputs,
                        std::vector<Binding> && scalar_parameters,
                        std::vector<Binding> && scalar_outputs,
                        std::vector<Binding> && internal_scalars);

private:

    bool useIndirectBr() const {
        return iBuilder->supportsIndirectBr();
    }

    void writeDoBlockMethod();

    void writeFinalBlockMethod(llvm::Value * remainingItems);

private:

    llvm::Function *        mDoBlockMethod;
    llvm::BasicBlock *      mStrideLoopBody;
    llvm::IndirectBrInst *  mStrideLoopBranch;
    llvm::PHINode *         mStrideLoopTarget;
};


}
#endif 
