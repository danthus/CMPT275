#include <kernel/core/kernel_compiler.h>
#include <kernel/core/kernel_builder.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>
#include <llvm/IR/Dominators.h>
#ifndef NDEBUG
#include <llvm/IR/Verifier.h>
#endif
#include <boost/intrusive/detail/math.hpp>
#include <boost/container/flat_set.hpp>

using namespace llvm;
using namespace boost;
using boost::intrusive::detail::floor_log2;
using boost::container::flat_set;

namespace kernel {

using AttrId = Attribute::KindId;
using Rational = ProcessingRate::Rational;
using RateId = ProcessingRate::KindId;
using StreamSetPort = Kernel::StreamSetPort;
using PortType = Kernel::PortType;

const static std::string BUFFER_HANDLE_SUFFIX = "_buffer";
const static std::string TERMINATION_SIGNAL = "__termination_signal";

#define BEGIN_SCOPED_REGION {
#define END_SCOPED_REGION }

// TODO: this check is a bit too strict in general; if the pipeline could request data/
// EOF padding from the MemorySource kernel, it would be possible to re-enable.

// #define CHECK_IO_ADDRESS_RANGE

// TODO: split the init/final into two methods each, one to do allocation/init, and the
// other final/deallocate? Would potentially allow us to reuse the kernel/stream set
// memory in the nested engine if each init method memzero'ed them. Would need to change
// the "main" method.

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief generateKernel
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::generateKernel(BuilderRef b) {
    // NOTE: make sure to keep and reset the original compiler here. A kernel could generate new kernels and
    // reuse the same KernelBuilder to do so; this could result in unexpected behaviour if the this function
    // exits without restoring the original compiler state.
    auto const oc = b->getCompiler();
    b->setCompiler(this);
    constructStreamSetBuffers(b);
    #ifndef NDEBUG
    for (const auto & buffer : mStreamSetInputBuffers) {
        assert ("input buffer not set by constructStreamSetBuffers" && buffer.get());
    }
    for (const auto & buffer : mStreamSetOutputBuffers) {
        assert ("output buffer not set by constructStreamSetBuffers" && buffer.get());
    }
    #endif
    addBaseInternalProperties(b);
    mTarget->addInternalProperties(b);
    mTarget->constructStateTypes(b);
    mTarget->addKernelDeclarations(b);
    callGenerateInitializeMethod(b);
    callGenerateAllocateSharedInternalStreamSets(b);
    callGenerateInitializeThreadLocalMethod(b);
    callGenerateAllocateThreadLocalInternalStreamSets(b);
    callGenerateDoSegmentMethod(b);
    callGenerateFinalizeThreadLocalMethod(b);
    callGenerateFinalizeMethod(b);
    mTarget->addAdditionalFunctions(b);

    // TODO: we could create a LLVM optimization pass manager here and execute it on this kernel;
    // it would allow the programmer to define a set of optimizations they want executed on the
    // kernel code. However, if compilers are intended to be short lived, we wouldn't be able to
    // easily share it amongst the same type of kernel compiler.

    // What is the cost of generating a pass manager instance for each compiled kernel vs.
    // the complexity of using a factory?

    runInternalOptimizationPasses(b->getModule());
    mTarget->runOptimizationPasses(b);
    b->setCompiler(oc);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief constructStreamSetBuffers
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::constructStreamSetBuffers(BuilderRef b) {
    mStreamSetInputBuffers.clear();
    const auto numOfInputStreams = mInputStreamSets.size();
    mStreamSetInputBuffers.resize(numOfInputStreams);
    for (unsigned i = 0; i < numOfInputStreams; ++i) {
        const Binding & input = mInputStreamSets[i];
        mStreamSetInputBuffers[i].reset(new ExternalBuffer(i, b, input.getType(), true, 0));
    }
    mStreamSetOutputBuffers.clear();
    const auto numOfOutputStreams = mOutputStreamSets.size();
    mStreamSetOutputBuffers.resize(numOfOutputStreams);
    for (unsigned i = 0; i < numOfOutputStreams; ++i) {
        const Binding & output = mOutputStreamSets[i];
        mStreamSetOutputBuffers[i].reset(new ExternalBuffer(i + numOfInputStreams, b, output.getType(), true, 0));
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addBaseInternalProperties
  ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::addBaseInternalProperties(BuilderRef b) {
     // If an output is a managed buffer, store its handle.
    const auto n = mOutputStreamSets.size();
    for (unsigned i = 0; i < n; ++i) {
        const Binding & output = mOutputStreamSets[i];
        Type * const handleTy = mStreamSetOutputBuffers[i]->getHandleType(b);
        if (LLVM_UNLIKELY(Kernel::isLocalBuffer(output, false))) {
            mTarget->addInternalScalar(handleTy, output.getName() + BUFFER_HANDLE_SUFFIX);
        } else {
            mTarget->addNonPersistentScalar(handleTy, output.getName() + BUFFER_HANDLE_SUFFIX);
        }
    }
    IntegerType * const sizeTy = b->getSizeTy();
    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableAsserts))) {

        // In multi-threaded mode, given a small file, the pipeline could finish before all of the
        // threads are constructed. Since we cannot detect when this occurs without additional
        // book keeping and the behaviour is safe, we do not guard against double termination.
        // All other kernels are checked to ensure that there are no pipeline errors.

        if (mTarget->getTypeId() != Kernel::TypeId::Pipeline || mTarget->hasAttribute(AttrId::InternallySynchronized)) {
            mTarget->addInternalScalar(sizeTy, TERMINATION_SIGNAL);
        } else {
            mTarget->addNonPersistentScalar(sizeTy, TERMINATION_SIGNAL);
        }
    } else {
        mTarget->addNonPersistentScalar(sizeTy, TERMINATION_SIGNAL);
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief reset
 ** ------------------------------------------------------------------------------------------------------------- */
template <typename Vec>
inline void reset(Vec & vec, const size_t n) {
    vec.resize(n);
    std::fill_n(vec.begin(), n, nullptr);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief callGenerateInitializeMethod
 ** ------------------------------------------------------------------------------------------------------------- */
inline void KernelCompiler::callGenerateInitializeMethod(BuilderRef b) {
    b->setCompiler(this);
    mCurrentMethod = mTarget->getInitializeFunction(b);
    mEntryPoint = BasicBlock::Create(b->getContext(), "entry", mCurrentMethod);
    b->SetInsertPoint(mEntryPoint);
    auto arg = mCurrentMethod->arg_begin();
    const auto arg_end = mCurrentMethod->arg_end();
    auto nextArg = [&]() {
        assert (arg != arg_end);
        Value * const v = &*arg;
        std::advance(arg, 1);
        return v;
    };
    if (LLVM_LIKELY(mTarget->isStateful())) {
        setHandle(nextArg());
    }
    if (LLVM_LIKELY(mTarget->isStateful())) {
        if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableMProtect))) {
            b->CreateMProtect(mSharedHandle, CBuilder::Protect::WRITE);
        }
    }
    initializeScalarMap(b, InitializeOptions::SkipThreadLocal);
    for (const auto & binding : mInputScalars) {
        b->setScalarField(binding.getName(), nextArg());
    }
    bindFamilyInitializationArguments(b, arg, arg_end);
    assert (arg == arg_end);    
    // TODO: we could permit shared managed buffers here if we passed in the buffer
    // into the init method. However, since there are no uses of this in any written
    // program, we currently prohibit it.
    initializeOwnedBufferHandles(b, InitializeOptions::SkipThreadLocal);
    // any kernel can set termination on initialization
    mTerminationSignalPtr = b->getScalarFieldPtr(TERMINATION_SIGNAL);
    b->CreateStore(b->getSize(KernelBuilder::TerminationCode::None), mTerminationSignalPtr);
    mTarget->generateInitializeMethod(b);
    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableMProtect) && mTarget->isStateful())) {
        b->CreateMProtect(mSharedHandle, CBuilder::Protect::READ);
    }
    b->CreateRet(b->CreateLoad(mTerminationSignalPtr));
    clearInternalStateAfterCodeGen();
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief bindFamilyInitializationArguments
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::bindFamilyInitializationArguments(BuilderRef /* b */, ArgIterator & /* arg */, const ArgIterator & /* arg_end */) const {

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief callGenerateInitializeThreadLocalMethod
 ** ------------------------------------------------------------------------------------------------------------- */
inline void KernelCompiler::callGenerateInitializeThreadLocalMethod(BuilderRef b) {
    if (mTarget->hasThreadLocal()) {
        b->setCompiler(this);
        assert (mSharedHandle == nullptr && mThreadLocalHandle == nullptr);
        mCurrentMethod = mTarget->getInitializeThreadLocalFunction(b);
        mEntryPoint = BasicBlock::Create(b->getContext(), "entry", mCurrentMethod);
        b->SetInsertPoint(mEntryPoint);
        auto arg = mCurrentMethod->arg_begin();
        auto nextArg = [&]() {
            assert (arg != mCurrentMethod->arg_end());
            Value * const v = &*arg;
            std::advance(arg, 1);
            return v;
        };
        if (LLVM_LIKELY(mTarget->isStateful())) {
            setHandle(nextArg());
        }
        mThreadLocalHandle = b->CreatePageAlignedMalloc(mTarget->getThreadLocalStateType());
        initializeScalarMap(b, InitializeOptions::IncludeThreadLocal);
        mTarget->generateInitializeThreadLocalMethod(b);
        b->CreateRet(mThreadLocalHandle);
        clearInternalStateAfterCodeGen();
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief callAllocateSharedInternalStreamSets
 ** ------------------------------------------------------------------------------------------------------------- */
inline void KernelCompiler::callGenerateAllocateSharedInternalStreamSets(BuilderRef b) {
    if (LLVM_UNLIKELY(mTarget->allocatesInternalStreamSets())) {
        b->setCompiler(this);
        assert (mSharedHandle == nullptr && mThreadLocalHandle == nullptr);
        mCurrentMethod = mTarget->getAllocateSharedInternalStreamSetsFunction(b);
        mEntryPoint = BasicBlock::Create(b->getContext(), "entry", mCurrentMethod);
        b->SetInsertPoint(mEntryPoint);
        auto arg = mCurrentMethod->arg_begin();
        auto nextArg = [&]() {
            assert (arg != mCurrentMethod->arg_end());
            Value * const v = &*arg;
            std::advance(arg, 1);
            return v;
        };
        if (LLVM_LIKELY(mTarget->isStateful())) {
            setHandle(nextArg());
        }
        Value * const expectedNumOfStrides = nextArg();
        initializeScalarMap(b, InitializeOptions::SkipThreadLocal);
        initializeOwnedBufferHandles(b, InitializeOptions::SkipThreadLocal);
        mTarget->generateAllocateSharedInternalStreamSetsMethod(b, expectedNumOfStrides);
        b->CreateRetVoid();
        clearInternalStateAfterCodeGen();
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief callAllocateThreadLocalInternalStreamSets
 ** ------------------------------------------------------------------------------------------------------------- */
inline void KernelCompiler::callGenerateAllocateThreadLocalInternalStreamSets(BuilderRef b) {
    if (LLVM_UNLIKELY(mTarget->allocatesInternalStreamSets() && mTarget->hasThreadLocal())) {
        b->setCompiler(this);
        assert (mSharedHandle == nullptr && mThreadLocalHandle == nullptr);
        mCurrentMethod = mTarget->getAllocateThreadLocalInternalStreamSetsFunction(b);
        mEntryPoint = BasicBlock::Create(b->getContext(), "entry", mCurrentMethod);
        b->SetInsertPoint(mEntryPoint);
        auto arg = mCurrentMethod->arg_begin();
        auto nextArg = [&]() {
            assert (arg != mCurrentMethod->arg_end());
            Value * const v = &*arg;
            std::advance(arg, 1);
            return v;
        };
        if (LLVM_LIKELY(mTarget->isStateful())) {
            setHandle(nextArg());
        }
        setThreadLocalHandle(nextArg());
        Value * const expectedNumOfStrides = nextArg();
        initializeScalarMap(b, InitializeOptions::IncludeThreadLocal);
        initializeOwnedBufferHandles(b, InitializeOptions::IncludeThreadLocal);
        mTarget->generateAllocateThreadLocalInternalStreamSetsMethod(b, expectedNumOfStrides);
        b->CreateRetVoid();
        clearInternalStateAfterCodeGen();
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getLCMOfFixedRateInputs
 ** ------------------------------------------------------------------------------------------------------------- */
/* static */ Rational KernelCompiler::getLCMOfFixedRateInputs(const Kernel * const target) {
    Rational rateLCM(1);
    bool first = true;
    const auto n = target->getNumOfStreamInputs();
    for (unsigned i = 0; i < n; ++i) {
        const Binding & input = target->getInputStreamSetBinding(i);
        const ProcessingRate & rate = input.getRate();
        if (LLVM_LIKELY(rate.isFixed())) {
            if (first) {
                rateLCM = rate.getRate();
                first = false;
            } else {
                rateLCM = lcm(rateLCM, rate.getRate());
            }
        }
    }
    return rateLCM;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getLCMOfFixedRateOutputs
 ** ------------------------------------------------------------------------------------------------------------- */
/* static */ Rational KernelCompiler::getLCMOfFixedRateOutputs(const Kernel * const target) {
    Rational rateLCM(1);
    bool first = true;
    const auto n = target->getNumOfStreamOutputs();
    for (unsigned i = 0; i < n; ++i) {
        const Binding & output = target->getOutputStreamSetBinding(i);
        const ProcessingRate & rate = output.getRate();
        if (LLVM_LIKELY(rate.isFixed())) {
            if (first) {
                rateLCM = rate.getRate();
                first = false;
            } else {
                rateLCM = lcm(rateLCM, rate.getRate());
            }
        }
    }
    return rateLCM;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief setDoSegmentProperties
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::setDoSegmentProperties(BuilderRef b, const ArrayRef<Value *> args) {

    // WARNING: any change to this must be reflected in Kernel::addDoSegmentDeclaration,
    // Kernel::getDoSegmentFields, KernelCompiler::getDoSegmentProperties,
    // and PipelineCompiler::writeKernelCall

    auto arg = args.begin();
    auto nextArg = [&]() {
        assert (arg != args.end());
        Value * const v = *arg; assert (v);
        std::advance(arg, 1);
        return v;
    };

    const auto enableAsserts = codegen::DebugOptionIsSet(codegen::EnableAsserts);

    if (LLVM_LIKELY(mTarget->isStateful())) {
        setHandle(nextArg());
        assert (mSharedHandle->getType()->getPointerElementType() == mTarget->getSharedStateType());
        if (LLVM_UNLIKELY(enableAsserts)) {
            b->CreateAssert(getHandle(), "%s: shared handle cannot be null", b->GetString(getName()));
        }
    }
    if (LLVM_UNLIKELY(mTarget->hasThreadLocal())) {
        setThreadLocalHandle(nextArg());
        assert (mThreadLocalHandle->getType()->getPointerElementType() == mTarget->getThreadLocalStateType());
        if (LLVM_UNLIKELY(enableAsserts)) {
            b->CreateAssert(getThreadLocalHandle(), "%s: thread local handle cannot be null", b->GetString(getName()));
        }
    }    
    const auto internallySynchronized = mTarget->hasAttribute(AttrId::InternallySynchronized);
    const auto greedy = mTarget->isGreedy();

    Rational fixedRateLCM{0};
    mFixedRateFactor = nullptr;
    if (LLVM_UNLIKELY(internallySynchronized || greedy)) {
        if (internallySynchronized) {
            mExternalSegNo = nextArg();
        }
        mNumOfStrides = nullptr;
        mIsFinal = nextArg();
    } else {
        mNumOfStrides = nextArg();
        mIsFinal = b->CreateIsNull(mNumOfStrides);
        mNumOfStrides = b->CreateSelect(mIsFinal, b->getSize(1), mNumOfStrides);
        if (LLVM_LIKELY(mTarget->hasFixedRateInput())) {
            fixedRateLCM = getLCMOfFixedRateInputs(mTarget);
            mFixedRateFactor = nextArg();
        }
    }

    initializeScalarMap(b, InitializeOptions::IncludeThreadLocal);

    // NOTE: the disadvantage of passing the stream pointers as a parameter is that it becomes more difficult
    // to access a stream set from a LLVM function call. We could create a stream-set aware function creation
    // and call system here but that is not an ideal way of handling this.

    const auto numOfInputs = getNumOfStreamInputs();

    reset(mProcessedInputItemPtr, numOfInputs);
    reset(mAccessibleInputItems, numOfInputs);
    reset(mAvailableInputItems, numOfInputs);
    reset(mUpdatableProcessedInputItemPtr, numOfInputs);

    #ifdef CHECK_IO_ADDRESS_RANGE
    auto checkStreamRange = [&](const StreamSetBuffer * const buffer, const Binding & binding, Value * const startItemCount) {

        SmallVector<char, 256> tmp;
        raw_svector_ostream out(tmp);
        out << "StreamSet " << getName() << ":" << binding.getName();

        PointerType * const int8PtrTy = b->getInt8PtrTy();

        ConstantInt * const ZERO = b->getSize(0);
        ConstantInt * const BLOCK_WIDTH = b->getSize(b->getBitBlockWidth());

        Value * const fromIndex = b->CreateUDiv(startItemCount, BLOCK_WIDTH);
        Value * const baseAddress = buffer->getBaseAddress(b);
        Value * const startPtr = buffer->getStreamBlockPtr(b, baseAddress, ZERO, fromIndex);
        Value * const start = b->CreatePointerCast(startPtr, int8PtrTy);

        Value * const endPos = b->CreateAdd(startItemCount, buffer->getCapacity(b));
        Value * const toIndex = b->CreateCeilUDiv(endPos, BLOCK_WIDTH);
        Value * const endPtr = buffer->getStreamBlockPtr(b, baseAddress, ZERO, toIndex);
        Value * const end = b->CreatePointerCast(endPtr, int8PtrTy);

        Value * const length = b->CreatePtrDiff(end, start);

        b->CreateAssert(b->CreateICmpULE(start, end),
                        "%s: illegal kernel I/O address range [0x%" PRIx64 ", 0x%" PRIx64 ")",
                        b->GetString(out.str()), start, end);

        b->CheckAddress(start, length, out.str());


    };
    #endif

    IntegerType * const sizeTy = b->getSizeTy();
    for (unsigned i = 0; i < numOfInputs; i++) {

        /// ----------------------------------------------------
        /// virtual base address
        /// ----------------------------------------------------
        StreamSetBuffer * const buffer = mStreamSetInputBuffers[i].get();
        assert (buffer && buffer->isLinear());
        assert (isa<ExternalBuffer>(buffer));

        const Binding & input = mInputStreamSets[i];
        Value * const virtualBaseAddress = b->CreatePointerCast(nextArg(), buffer->getPointerType());
        Value * const localHandle = b->CreateAllocaAtEntryPoint(buffer->getHandleType(b));
        buffer->setHandle(localHandle);
        buffer->setBaseAddress(b, virtualBaseAddress);

        if (LLVM_UNLIKELY(enableAsserts)) {
            b->CreateAssert(buffer->getBaseAddress(b), "%s.%s: virtual base address cannot be null",
                            b->GetString(getName()), b->GetString(input.getName()));
        }

        /// ----------------------------------------------------
        /// processed item count
        /// ----------------------------------------------------

        // NOTE: we create a redundant alloca to store the input param so that
        // Mem2Reg can convert it into a PHINode if the item count is updated in
        // a loop; otherwise, it will be discarded in favor of the param itself.

        const ProcessingRate & rate = input.getRate();
        Value * processed = nullptr;
        if (internallySynchronized || isAddressable(input)) {
            mUpdatableProcessedInputItemPtr[i] = nextArg();
            processed = b->CreateLoad(mUpdatableProcessedInputItemPtr[i]);
        } else if (LLVM_LIKELY(isCountable(input))) {
            processed = nextArg();
        } else { // isRelative
            const auto port = getStreamPort(rate.getReference());
            assert (port.Type == PortType::Input && port.Number < i);
            assert (mProcessedInputItemPtr[port.Number]);
            Value * const ref = b->CreateLoad(mProcessedInputItemPtr[port.Number]);
            processed = b->CreateMulRational(ref, rate.getRate());
        }
        assert (processed);
        assert (processed->getType() == sizeTy);
        AllocaInst * const processedItems = b->CreateAllocaAtEntryPoint(sizeTy);
        b->CreateStore(processed, processedItems);
        mProcessedInputItemPtr[i] = processedItems;
        /// ----------------------------------------------------
        /// accessible item count
        /// ----------------------------------------------------
        Value * accessible = nullptr;
        if (LLVM_UNLIKELY(internallySynchronized || requiresItemCount(input))) {
            accessible = nextArg();
        } else {
            accessible = b->CreateCeilUMulRational(mFixedRateFactor, rate.getRate() / fixedRateLCM);
        }
        assert (accessible);
        assert (accessible->getType() == sizeTy);
        mAccessibleInputItems[i] = accessible;
        Value * avail = b->CreateAdd(processed, accessible);
        mAvailableInputItems[i] = avail;
        if (input.hasLookahead()) {
            avail = b->CreateAdd(avail, b->getSize(input.getLookahead()));
        }
        buffer->setCapacity(b, avail);
        #ifdef CHECK_IO_ADDRESS_RANGE
        if (LLVM_UNLIKELY(enableAsserts)) {
            checkStreamRange(buffer, input, processed);
        }
        #endif
    }

    // set all of the output buffers
    const auto numOfOutputs = getNumOfStreamOutputs();
    reset(mProducedOutputItemPtr, numOfOutputs);
    reset(mInitiallyProducedOutputItems, numOfOutputs);    
    reset(mWritableOutputItems, numOfOutputs);
    reset(mConsumedOutputItems, numOfOutputs);
    reset(mUpdatableProducedOutputItemPtr, numOfOutputs);
    reset(mUpdatableOutputBaseVirtualAddressPtr, numOfOutputs);

    const auto canTerminate = canSetTerminateSignal();

    for (unsigned i = 0; i < numOfOutputs; i++) {

        /// ----------------------------------------------------
        /// logical buffer base address
        /// ----------------------------------------------------
        StreamSetBuffer * const buffer = mStreamSetOutputBuffers[i].get();
        assert (buffer && buffer->isLinear());

        const Binding & output = mOutputStreamSets[i];
        const auto isShared = output.hasAttribute(AttrId::SharedManagedBuffer);
        const auto isLocal = internallySynchronized || isShared || Kernel::isLocalBuffer(output, false);

        if (LLVM_UNLIKELY(isShared)) {
            Value * const handle = nextArg();
            assert (isa<DynamicBuffer>(buffer));
            buffer->setHandle(b->CreatePointerCast(handle, buffer->getHandlePointerType(b)));
        } else if (LLVM_UNLIKELY(isLocal)) {
            // If an output is a managed buffer, the address is stored within the state instead
            // of being passed in through the function call.
            mUpdatableOutputBaseVirtualAddressPtr[i] = nextArg();
            Value * handle = getScalarFieldPtr(b.get(), output.getName() + BUFFER_HANDLE_SUFFIX);
            buffer->setHandle(handle);
        } else {
            Value * const virtualBaseAddress = b->CreatePointerCast(nextArg(), buffer->getPointerType());
            Value * const localHandle = b->CreateAllocaAtEntryPoint(buffer->getHandleType(b));
            buffer->setHandle(localHandle);
            buffer->setBaseAddress(b, virtualBaseAddress);
            assert (isa<ExternalBuffer>(buffer));
        }
        assert (buffer->getHandle());
        if (LLVM_UNLIKELY(enableAsserts)) {
            b->CreateAssert(buffer->getBaseAddress(b), "%s.%s: virtual base address cannot be null",
                            b->GetString(getName()), b->GetString(output.getName()));
        }


        /// ----------------------------------------------------
        /// produced item count
        /// ----------------------------------------------------
        const ProcessingRate & rate = output.getRate();
        Value * produced = nullptr;
        if (LLVM_LIKELY(internallySynchronized || canTerminate || isAddressable(output))) {
            mUpdatableProducedOutputItemPtr[i] = nextArg();
            produced = b->CreateLoad(mUpdatableProducedOutputItemPtr[i]);
        } else if (LLVM_LIKELY(isCountable(output))) {
            produced = nextArg();
        } else { // isRelative
            // For now, if something is produced at a relative rate to another stream in a kernel that
            // may terminate, its final item count is inherited from its reference stream and cannot
            // be set independently. Should they be independent at early termination?
            const auto port = getStreamPort(rate.getReference());
            assert (port.Type == PortType::Input || (port.Type == PortType::Output && port.Number < i));
            const auto & items = (port.Type == PortType::Input) ? mProcessedInputItemPtr : mProducedOutputItemPtr;
            Value * const ref = b->CreateLoad(items[port.Number]);
            produced = b->CreateMulRational(ref, rate.getRate());
        }
        assert (produced);
        assert (produced->getType() == sizeTy);
        mInitiallyProducedOutputItems[i] = produced;
        AllocaInst * const producedItems = b->CreateAllocaAtEntryPoint(sizeTy);
        b->CreateStore(produced, producedItems);
        mProducedOutputItemPtr[i] = producedItems;
        /// ----------------------------------------------------
        /// writable / consumed item count
        /// ----------------------------------------------------
        Value * writable = nullptr;
        if (LLVM_UNLIKELY(isLocal)) {
            Value * const consumed = nextArg();
            assert (consumed->getType() == sizeTy);
            mConsumedOutputItems[i] = consumed;
            writable = buffer->getLinearlyWritableItems(b, produced, consumed);
            assert (writable && writable->getType() == sizeTy);
        } else {
            if (requiresItemCount(output)) {
                writable = nextArg();
                assert (writable && writable->getType() == sizeTy);
            } else if (mFixedRateFactor) {
                writable = b->CreateCeilUMulRational(mFixedRateFactor, rate.getRate() / fixedRateLCM);
                assert (writable && writable->getType() == sizeTy);
            }
            Value * capacity = nullptr;
            if (writable) {
                capacity = b->CreateAdd(produced, writable);
                buffer->setCapacity(b, capacity);
                #ifdef CHECK_IO_ADDRESS_RANGE
                if (LLVM_UNLIKELY(enableAsserts)) {
                    checkStreamRange(buffer, output, produced);
                }
                #endif
            } else {
                capacity = ConstantExpr::getNeg(b->getSize(1));
                buffer->setCapacity(b, capacity);
            }
        }
        mWritableOutputItems[i] = writable;
    }
    assert (arg == args.end());

    // initialize the termination signal if this kernel can set it
    mTerminationSignalPtr = nullptr;
    if (canTerminate) {
        mTerminationSignalPtr = b->getScalarFieldPtr(TERMINATION_SIGNAL);
        if (LLVM_UNLIKELY(enableAsserts)) {
            Value * const unterminated =
                b->CreateICmpEQ(b->CreateLoad(mTerminationSignalPtr), b->getSize(KernelBuilder::TerminationCode::None));
            b->CreateAssert(unterminated, getName() + ".doSegment was called after termination?");
        }
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getDoSegmentProperties
 *
 * Reverse of the setDoSegmentProperties operation; used by the PipelineKernel when constructing internal threads
 * to simplify passing of the state data.
 ** ------------------------------------------------------------------------------------------------------------- */
std::vector<Value *> KernelCompiler::getDoSegmentProperties(BuilderRef b) const {

    // WARNING: any change to this must be reflected in addDoSegmentDeclaration, getDoSegmentFields,
    // setDoSegmentProperties, and PipelineCompiler::writeKernelCall

    std::vector<Value *> props;
    if (LLVM_LIKELY(mTarget->isStateful())) {
        props.push_back(mSharedHandle); assert (mSharedHandle);
    }
    if (LLVM_UNLIKELY(mTarget->hasThreadLocal())) {
        props.push_back(mThreadLocalHandle); assert (mThreadLocalHandle);
    }
    const auto internallySynchronized = mTarget->hasAttribute(AttrId::InternallySynchronized);
    const auto greedy = mTarget->isGreedy();
    if (LLVM_UNLIKELY(internallySynchronized || greedy)) {
        if (internallySynchronized) {
            props.push_back(mExternalSegNo);
        }
        props.push_back(mIsFinal);
    } else {
        props.push_back(mNumOfStrides); assert (mNumOfStrides);
        if (LLVM_LIKELY(mTarget->hasFixedRateInput())) {
            props.push_back(mFixedRateFactor);
        }
    }

    PointerType * const voidPtrTy = b->getVoidPtrTy();

    const auto numOfInputs = getNumOfStreamInputs();
    for (unsigned i = 0; i < numOfInputs; i++) {
        /// ----------------------------------------------------
        /// logical buffer base address
        /// ----------------------------------------------------
        const auto & buffer = mStreamSetInputBuffers[i];
        props.push_back(b->CreatePointerCast(buffer->getBaseAddress(b), voidPtrTy));
        /// ----------------------------------------------------
        /// processed item count
        /// ----------------------------------------------------
        const Binding & input = mInputStreamSets[i];
        if (internallySynchronized || isAddressable(input)) {
            props.push_back(mProcessedInputItemPtr[i]);
        } else if (LLVM_LIKELY(isCountable(input))) {
            props.push_back(b->CreateLoad(mProcessedInputItemPtr[i]));
        }
        /// ----------------------------------------------------
        /// accessible item count
        /// ----------------------------------------------------
        if (internallySynchronized || requiresItemCount(input)) {
            props.push_back(mAccessibleInputItems[i]);
        }
    }

    // set all of the output buffers
    const auto numOfOutputs = getNumOfStreamOutputs();
    const auto canTerminate = canSetTerminateSignal();

    for (unsigned i = 0; i < numOfOutputs; i++) {
        /// ----------------------------------------------------
        /// logical buffer base address
        /// ----------------------------------------------------
        const auto & buffer = mStreamSetOutputBuffers[i];
        const Binding & output = mOutputStreamSets[i];

        const auto isShared = output.hasAttribute(AttrId::SharedManagedBuffer);
        const auto isLocal = internallySynchronized || isShared || Kernel::isLocalBuffer(output, false);

        Value * handle = nullptr;
        if (LLVM_UNLIKELY(isShared)) {            
            handle = b->CreatePointerCast(buffer->getHandle(), voidPtrTy);
        } else {
            if (LLVM_UNLIKELY(isLocal)) {
                // If an output is a managed buffer, the address is stored within the state instead
                // of being passed in through the function call.
                PointerType * const voidPtrPtrTy = voidPtrTy->getPointerTo();
                handle = b->CreatePointerCast(mUpdatableOutputBaseVirtualAddressPtr[i], voidPtrPtrTy);
            } else {
                handle = b->CreatePointerCast(buffer->getBaseAddress(b), voidPtrTy);
            }
        }
        props.push_back(handle);

        /// ----------------------------------------------------
        /// produced item count
        /// ----------------------------------------------------
        if (LLVM_LIKELY(internallySynchronized || canTerminate || isAddressable(output))) {
            props.push_back(mProducedOutputItemPtr[i]);
        } else if (LLVM_LIKELY(isCountable(output))) {
            props.push_back(b->CreateLoad(mProducedOutputItemPtr[i]));
        }
        /// ----------------------------------------------------
        /// writable / consumed item count
        /// ----------------------------------------------------
        if (LLVM_UNLIKELY(isLocal)) {
            props.push_back(mConsumedOutputItems[i]);
        } else if (requiresItemCount(output)) {
            props.push_back(mWritableOutputItems[i]);
        }
    }
    return props;
}


/** ------------------------------------------------------------------------------------------------------------- *
 * @brief callGenerateDoSegmentMethod
 ** ------------------------------------------------------------------------------------------------------------- */
inline void KernelCompiler::callGenerateDoSegmentMethod(BuilderRef b) {

    assert (mInputStreamSets.size() == mStreamSetInputBuffers.size());
    assert (mOutputStreamSets.size() == mStreamSetOutputBuffers.size());

    b->setCompiler(this);
    mCurrentMethod = mTarget->getDoSegmentFunction(b);
    mEntryPoint = BasicBlock::Create(b->getContext(), "entry", mCurrentMethod);
    b->SetInsertPoint(mEntryPoint);

    BEGIN_SCOPED_REGION
    Vec<Value *, 64> args;
    args.reserve(mCurrentMethod->arg_size());
    for (auto ArgI = mCurrentMethod->arg_begin(); ArgI != mCurrentMethod->arg_end(); ++ArgI) {
        args.push_back(&(*ArgI));
    }
    setDoSegmentProperties(b, args);
    END_SCOPED_REGION

    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableMProtect))) {
        b->CreateMProtect(mSharedHandle, CBuilder::Protect::WRITE);
    }

    mTarget->generateKernelMethod(b);

    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableMProtect))) {
        b->CreateMProtect(mSharedHandle, CBuilder::Protect::READ);
    }

    const auto numOfInputs = getNumOfStreamInputs();

    for (unsigned i = 0; i < numOfInputs; i++) {
        if (mUpdatableProcessedInputItemPtr[i]) {
            Value * const items = b->CreateLoad(mProcessedInputItemPtr[i]);
            b->CreateStore(items, mUpdatableProcessedInputItemPtr[i]);
        }
    }

    const auto numOfOutputs = getNumOfStreamOutputs();

    for (unsigned i = 0; i < numOfOutputs; i++) {
        // Write the virtual base address out to inform the pipeline of any changes
        const auto & buffer = mStreamSetOutputBuffers[i];
        if (mUpdatableOutputBaseVirtualAddressPtr[i]) {
            Value * const baseAddress = buffer->getBaseAddress(b);
            if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableAsserts))) {
                SmallVector<char, 256> tmp;
                raw_svector_ostream out(tmp);
                const Binding & output = mOutputStreamSets[i];
                out << getName() << ":%s is returning a virtual base address "
                                    "computed from a null base address.";
                b->CreateAssert(baseAddress, out.str(), b->GetString(output.getName()));
            }
            Constant * const LOG_2_BLOCK_WIDTH = b->getSize(floor_log2(b->getBitBlockWidth()));
            Constant * const ZERO = b->getSize(0);
            Value * produced = mInitiallyProducedOutputItems[i];
            // TODO: will LLVM optimizations replace the following with the already loaded value?
            // If not, re-loading it here may reduce register pressure / compilation time.
            if (mUpdatableProducedOutputItemPtr[i]) {
                produced = b->CreateLoad(mUpdatableProducedOutputItemPtr[i]);
            }
            Value * const blockIndex = b->CreateLShr(produced, LOG_2_BLOCK_WIDTH);
            Value * vba = buffer->getStreamLogicalBasePtr(b.get(), baseAddress, ZERO, blockIndex);
            vba = b->CreatePointerCast(vba, b->getVoidPtrTy());

            b->CreateStore(vba, mUpdatableOutputBaseVirtualAddressPtr[i]);
        }
        if (mUpdatableProducedOutputItemPtr[i]) {
            Value * const items = b->CreateLoad(mProducedOutputItemPtr[i]);
            b->CreateStore(items, mUpdatableProducedOutputItemPtr[i]);
        }
    }

    // return the termination signal (if one exists)
    if (mTerminationSignalPtr) {
        b->CreateRet(b->CreateLoad(mTerminationSignalPtr));
        mTerminationSignalPtr = nullptr;
    } else {
        b->CreateRetVoid();
    }
    clearInternalStateAfterCodeGen();
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief copyInternalState
 ** ------------------------------------------------------------------------------------------------------------- */
std::vector<Value *> KernelCompiler::storeDoSegmentState() const {

    const auto numOfInputs = getNumOfStreamInputs();
    const auto numOfOutputs = getNumOfStreamOutputs();

    std::vector<Value *> S;
    S.resize(8 + numOfInputs * 4 + numOfOutputs * 6);

    auto o = S.begin();

    auto append = [&](Value * v) {
        *o++ = v;
    };

    append(mSharedHandle);
    append(mThreadLocalHandle);
    append(mTerminationSignalPtr);
    append(mIsFinal);
    append(mNumOfStrides);
    append(mFixedRateFactor);
    append(mExternalSegNo);
    append(mNumOfStrides);

    auto copy = [&](const Vec<llvm::Value *> & V, const size_t n) {
        o = std::copy_n(V.begin(), n, o);
    };

    copy(mProcessedInputItemPtr, numOfInputs);
    copy(mAccessibleInputItems, numOfInputs);
    copy(mAvailableInputItems, numOfInputs);
    copy(mUpdatableProcessedInputItemPtr, numOfInputs);


    copy(mProducedOutputItemPtr, numOfOutputs);
    copy(mInitiallyProducedOutputItems, numOfOutputs);
    copy(mWritableOutputItems, numOfOutputs);
    copy(mConsumedOutputItems, numOfOutputs);
    copy(mUpdatableProducedOutputItemPtr, numOfOutputs);
    copy(mUpdatableOutputBaseVirtualAddressPtr, numOfOutputs);

    assert (o == S.end());

    return S;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief restoreInternalState
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::restoreDoSegmentState(const std::vector<Value *> & S) {

    auto o = S.begin();

    auto extract = [&](Value *& v) {
        assert (o != S.end());
        v = *o++;
    };

    extract(mSharedHandle);
    extract(mThreadLocalHandle);
    extract(mTerminationSignalPtr);
    extract(mIsFinal);
    extract(mNumOfStrides);
    extract(mFixedRateFactor);
    extract(mExternalSegNo);
    extract(mNumOfStrides);

    auto revert = [&](Vec<llvm::Value *> & V, const size_t n) {
        assert (static_cast<size_t>(std::distance(o, S.end())) >= n);
        assert (V.size() == n);
        std::copy_n(o, n, V.begin());
        o += n;
    };

    const auto numOfInputs = getNumOfStreamInputs();
    revert(mProcessedInputItemPtr, numOfInputs);
    revert(mAccessibleInputItems, numOfInputs);
    revert(mAvailableInputItems, numOfInputs);
    revert(mUpdatableProcessedInputItemPtr, numOfInputs);

    const auto numOfOutputs = getNumOfStreamOutputs();
    revert(mProducedOutputItemPtr, numOfOutputs);
    revert(mInitiallyProducedOutputItems, numOfOutputs);
    revert(mWritableOutputItems, numOfOutputs);
    revert(mConsumedOutputItems, numOfOutputs);
    revert(mUpdatableProducedOutputItemPtr, numOfOutputs);
    revert(mUpdatableOutputBaseVirtualAddressPtr, numOfOutputs);

    assert (o == S.end());


}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief callGenerateFinalizeThreadLocalMethod
 ** ------------------------------------------------------------------------------------------------------------- */
inline void KernelCompiler::callGenerateFinalizeThreadLocalMethod(BuilderRef b) {
    if (mTarget->hasThreadLocal()) {
        b->setCompiler(this);
        mCurrentMethod = mTarget->getFinalizeThreadLocalFunction(b);
        mEntryPoint = BasicBlock::Create(b->getContext(), "entry", mCurrentMethod);
        b->SetInsertPoint(mEntryPoint);
        auto arg = mCurrentMethod->arg_begin();
        auto nextArg = [&]() {
            assert (arg != mCurrentMethod->arg_end());
            Value * const v = &*arg;
            std::advance(arg, 1);
            return v;
        };
        if (LLVM_LIKELY(mTarget->isStateful())) {
            setHandle(nextArg());
        }
        mThreadLocalHandle = nextArg();
        initializeScalarMap(b, InitializeOptions::IncludeThreadLocal);
        mTarget->generateFinalizeThreadLocalMethod(b);
        b->CreateRetVoid();
        clearInternalStateAfterCodeGen();
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief callGenerateFinalizeMethod
 ** ------------------------------------------------------------------------------------------------------------- */
inline void KernelCompiler::callGenerateFinalizeMethod(BuilderRef b) {

    b->setCompiler(this);
    mCurrentMethod = mTarget->getFinalizeFunction(b);
    mEntryPoint = BasicBlock::Create(b->getContext(), "entry", mCurrentMethod);
    b->SetInsertPoint(mEntryPoint);
    if (LLVM_LIKELY(mTarget->isStateful())) {
        auto args = mCurrentMethod->arg_begin();
        setHandle(&*(args++));
        assert (args == mCurrentMethod->arg_end());
    }
    initializeScalarMap(b, InitializeOptions::SkipThreadLocal);
    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableMProtect))) {
        b->CreateMProtect(mSharedHandle,CBuilder::Protect::WRITE);
    }
    initializeOwnedBufferHandles(b, InitializeOptions::SkipThreadLocal);
    mTarget->generateFinalizeMethod(b); // may be overridden by the Kernel subtype
    const auto outputs = getFinalOutputScalars(b);
    if (LLVM_LIKELY(mTarget->isStateful())) {
        b->CreateFree(mSharedHandle);
    }

    if (outputs.empty()) {
        b->CreateRetVoid();
    } else {
        const auto n = outputs.size();
        if (n == 1) {
            b->CreateRet(outputs[0]);
        } else {
            b->CreateAggregateRet(outputs.data(), n);
        }
    }
    clearInternalStateAfterCodeGen();



}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getFinalOutputScalars
 ** ------------------------------------------------------------------------------------------------------------- */
std::vector<Value *> KernelCompiler::getFinalOutputScalars(BuilderRef b) {
    const auto n = mOutputScalars.size();
    std::vector<Value *> outputs(n);
    for (unsigned i = 0; i < n; ++i) {
        outputs[i] = b->CreateLoad(getScalarFieldPtr(b.get(), mOutputScalars[i].getName()));
    }
    return outputs;
}

template <typename IndexTy>
static bool checkIndexedTypeInternal(StructType *ST, ArrayRef<IndexTy> IdxList) {
  // Handle the special case of the empty set index set, which is always valid.
  if (IdxList.empty())
    return true;

  const auto n = ST->getStructNumElements();

  for (unsigned i = 0; i < n; ++i) {
      Type * ty = ST->getStructElementType(i);
        if (!ty->isSized())   {
            errs() << i << " is unsized: ";
            ty->print(errs(), false);
            errs() << "\n";
            return false;
        }
  }

  // If there is at least one index, the top level type must be sized, otherwise
  // it cannot be 'stepped over'.
  if (!cast<StructType>(ST)->isSized()) {
     errs() << "unsized?";
     return false;
  }

  Type * Agg = ST;
  unsigned CurIdx = 1;
  for (; CurIdx != IdxList.size(); ++CurIdx) {
    CompositeType *CT = dyn_cast<CompositeType>(Agg);
    if (!CT || CT->isPointerTy()) {
        errs() << CurIdx << " is not composite?";
        return false;
    }
    IndexTy Index = IdxList[CurIdx];
    if (!CT->indexValid(Index)) {
        errs() << CurIdx << " is not valid index?";
        return false;
    }
    Agg = CT->getTypeAtIndex(Index);
  }
  if (CurIdx != IdxList.size()) {
    errs() << CurIdx << " has incorrect elements " << IdxList.size();
  }


  return CurIdx == IdxList.size();
}

bool checkIndexedType(Type *Ty, ArrayRef<Value *> IdxList) {
    return checkIndexedTypeInternal(cast<StructType>(Ty), IdxList);
}

//bool isSized(StructType * type, SmallPtrSetImpl<Type*> *Visited) {

//    if (type->isOpaque())
//      return false;


//    // Okay, our struct is sized if all of the elements are, but if one of the
//    // elements is opaque, the struct isn't sized *yet*, but may become sized in
//    // the future, so just bail out without caching.
//    for (auto I = type->element_begin(), E = type->element_end(); I != E; ++I)
//      if (!(*I)->isSized(Visited))
//        return false;

//    // Here we cheat a bit and cast away const-ness. The goal is to memoize when
//    // we find a sized type, as types can only move from opaque to sized, not the
//    // other way.
//    const_cast<StructType*>(type)->setSubclassData(
//      type->getSubclassData() | StructType::SCDB_IsSized);
//    return true;
// }

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief initializeScalarMap
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::initializeScalarMap(BuilderRef b, const InitializeOptions options) {

    FixedArray<Value *, 3> indices;
    indices[0] = b->getInt32(0);

    const auto sharedTy = mTarget->getSharedStateType();

    const auto threadLocalTy = mTarget->getThreadLocalStateType();

    #ifndef NDEBUG
    auto verifyStateType = [](Value * const handle, StructType * const stateType) {
        if (handle == nullptr && stateType == nullptr) {
            return true;
        }
        if (handle == nullptr || stateType == nullptr) {
            return false;
        }
        if (handle->getType() != stateType->getPointerTo()) {
            return false;
        }
        assert (!stateType->isOpaque());
        const auto n = stateType->getStructNumElements();
        assert ((n % 2) == 0);
        for (unsigned i = 0; i < n; i += 2) {
            assert (isa<StructType>(stateType->getStructElementType(i)));
        }
        return true;
    };
    assert ("incorrect shared handle/type!" && verifyStateType(mSharedHandle, sharedTy));
    if (options == InitializeOptions::IncludeThreadLocal) {
        assert ("incorrect thread local handle/type!" && verifyStateType(mThreadLocalHandle, threadLocalTy));
    }
    #undef CHECK_TYPES
    #endif

    mScalarFieldMap.clear();

    auto addToScalarFieldMap = [&](StringRef bindingName, Value * const scalar, Type * const expectedType) {
        const auto i = mScalarFieldMap.insert(std::make_pair(bindingName, scalar));
        if (LLVM_UNLIKELY(!i.second)) {
            SmallVector<char, 256> tmp;
            raw_svector_ostream out(tmp);
            out << "Kernel " << getName() << " contains two scalar or alias fields named " << bindingName;
            report_fatal_error(out.str());
        }
        if (expectedType) {
            Type * const actualType = scalar->getType()->getPointerElementType();
            if (LLVM_UNLIKELY(actualType != expectedType)) {
                SmallVector<char, 256> tmp;
                raw_svector_ostream out(tmp);
                out << "Scalar " << getName() << '.' << bindingName << " was expected to be a ";
                expectedType->print(out);
                out << " but was stored as a ";
                actualType->print(out);
                report_fatal_error(out.str());
            }
        }
    };

    flat_set<unsigned> sharedGroups;
    flat_set<unsigned> threadLocalGroups;

    for (const auto & scalar : mInternalScalars) {
        assert (scalar.getValueType());
        switch (scalar.getScalarType()) {
            case ScalarType::Internal:
                sharedGroups.insert(scalar.getGroup());
                break;
            case ScalarType::ThreadLocal:
                threadLocalGroups.insert(scalar.getGroup());
                break;
            default: break;
        }
    }

    std::vector<unsigned> sharedIndex(sharedGroups.size() + 2, 0);
    std::vector<unsigned> threadLocalIndex(threadLocalGroups.size(), 0);

    auto enumerate = [&](const Bindings & bindings, const unsigned groupId) {
        indices[1] = b->getInt32(groupId * 2);
        auto & k = sharedIndex[groupId];
        for (const auto & binding : bindings) {
            assert ((groupId * 2) < sharedTy->getStructNumElements());
            assert (k < sharedTy->getStructElementType(groupId * 2)->getStructNumElements());
            assert (sharedTy->getStructElementType(groupId * 2)->getStructElementType(k) == binding.getType());
            indices[2] = b->getInt32(k++);
            assert (checkIndexedType(sharedTy, indices));
            Value * const scalar = b->CreateGEP(sharedTy, mSharedHandle, indices);
            addToScalarFieldMap(binding.getName(), scalar, binding.getType());
        }
    };

    enumerate(mInputScalars, 0);


    for (const auto & binding : mInternalScalars) {
        Value * scalar = nullptr;

        auto getGroupIndex = [&](const flat_set<unsigned> & groups) {
            const auto f = groups.find(binding.getGroup());
            assert (f != groups.end());
            return std::distance(groups.begin(), f);
        };

        switch (binding.getScalarType()) {
            case ScalarType::Internal:
                assert (mSharedHandle);
                BEGIN_SCOPED_REGION
                const auto j = getGroupIndex(sharedGroups) + 1;
                indices[1] = b->getInt32(j * 2);
                auto & k = sharedIndex[j];
                assert ((j * 2) < sharedTy->getStructNumElements());
                assert (k < sharedTy->getStructElementType(j * 2)->getStructNumElements());
                assert (sharedTy->getStructElementType(j * 2)->getStructElementType(k) == binding.getValueType());
                indices[2] = b->getInt32(k++);
                scalar = b->CreateGEP(sharedTy, mSharedHandle, indices);
                END_SCOPED_REGION
                break;
            case ScalarType::ThreadLocal:
                if (options == InitializeOptions::SkipThreadLocal) continue;
                assert (mThreadLocalHandle);
                BEGIN_SCOPED_REGION
                const auto j = getGroupIndex(threadLocalGroups);
                indices[1] = b->getInt32(j * 2);
                auto & k = threadLocalIndex[j];
                assert ((j * 2) < threadLocalTy->getStructNumElements());
                assert (k < threadLocalTy->getStructElementType(j * 2)->getStructNumElements());
                assert (threadLocalTy->getStructElementType(j * 2)->getStructElementType(k) == binding.getValueType());
                indices[2] = b->getInt32(k++);
                assert (checkIndexedType(threadLocalTy, indices));
                scalar = b->CreateGEP(threadLocalTy, mThreadLocalHandle, indices);
                END_SCOPED_REGION
                break;
            case ScalarType::NonPersistent:
                scalar = b->CreateAllocaAtEntryPoint(binding.getValueType());
                b->CreateStore(Constant::getNullValue(binding.getValueType()), scalar);
                break;
            default: llvm_unreachable("I/O scalars cannot be internal");
        }
        addToScalarFieldMap(binding.getName(), scalar, binding.getValueType());
    }

    enumerate(mOutputScalars, sharedGroups.size() + 1);

    // finally add any aliases
    for (const auto & alias : mScalarAliasMap) {
        const auto f = mScalarFieldMap.find(alias.second);
        if (f != mScalarFieldMap.end()) {
            addToScalarFieldMap(alias.first, f->second, nullptr);
        }
    }

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addAlias
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::addAlias(llvm::StringRef alias, llvm::StringRef scalarName) {
    mScalarAliasMap.emplace_back(alias, scalarName);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief initializeBindingMap
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::initializeIOBindingMap() {

    auto enumerate = [&](const Bindings & bindings, const BindingType type) {
        const auto n = bindings.size();
        for (unsigned i = 0; i < n; ++i) {
            const auto & binding = bindings[i];
            mBindingMap.insert(std::make_pair(binding.getName(), BindingMapEntry{type, i}));
        }
    };

    enumerate(mInputScalars, BindingType::ScalarInput);
    enumerate(mOutputScalars, BindingType::ScalarOutput);
    enumerate(mInputStreamSets, BindingType::StreamInput);
    enumerate(mOutputStreamSets, BindingType::StreamOutput);

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief initializeOwnedBufferHandles
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::initializeOwnedBufferHandles(BuilderRef b, const InitializeOptions /* options */) {
    const auto numOfOutputs = getNumOfStreamOutputs();
    for (unsigned i = 0; i < numOfOutputs; i++) {
        const Binding & output = mOutputStreamSets[i];
        if (LLVM_UNLIKELY(Kernel::isLocalBuffer(output, false))) {
            Value * const handle = getScalarFieldPtr(b.get(), output.getName() + BUFFER_HANDLE_SUFFIX);
            const auto & buffer = mStreamSetOutputBuffers[i]; assert (buffer.get());
            buffer->setHandle(handle);
        }
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getBinding
 ** ------------------------------------------------------------------------------------------------------------- */
const BindingMapEntry & KernelCompiler::getBinding(const BindingType type, const llvm::StringRef name) const {

    const auto f = mBindingMap.find(name);
    if (f != mBindingMap.end()) {
        const BindingMapEntry & entry = f->second;
        assert (entry.Type == type);
        return entry;
    }

    SmallVector<char, 256> tmp;
    raw_svector_ostream out(tmp);
    out << "Kernel " << getName() << " does not contain an ";
    switch (type) {
        case BindingType::ScalarInput:
        case BindingType::StreamInput:
            out << "input"; break;
        case BindingType::ScalarOutput:
        case BindingType::StreamOutput:
            out << "output"; break;
    }
    out << ' ';
    switch (type) {
        case BindingType::ScalarInput:
        case BindingType::ScalarOutput:
            out << "scalar"; break;
        case BindingType::StreamInput:
        case BindingType::StreamOutput:
            out << "streamset"; break;
    }
    out << " named \"" << name << "\"\n"
           "Currently contains:";


    auto listAvailableBindings = [&](const Bindings & bindings) {
        if (LLVM_UNLIKELY(bindings.empty())) {
            out << "<no bindings>";
        } else {
            char joiner = ' ';
            for (const auto & binding : bindings) {
                out << joiner << binding.getName();
                joiner = ',';
            }
        }
        out << '\n';
    };

    switch (type) {
        case BindingType::ScalarInput:
            listAvailableBindings(mInputScalars); break;
        case BindingType::ScalarOutput:
            listAvailableBindings(mOutputScalars); break;
        case BindingType::StreamInput:
            listAvailableBindings(mInputStreamSets); break;
        case BindingType::StreamOutput:
            listAvailableBindings(mOutputStreamSets); break;
    }

    report_fatal_error(out.str());
}


/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getStreamPort
 ** ------------------------------------------------------------------------------------------------------------- */
StreamSetPort KernelCompiler::getStreamPort(const StringRef name) const {

    // NOTE: temporary refactoring step to limit changes outside of the kernel class

    static_assert(static_cast<unsigned>(BindingType::StreamInput) == static_cast<unsigned>(PortType::Input), "");
    static_assert(static_cast<unsigned>(BindingType::StreamOutput) == static_cast<unsigned>(PortType::Output), "");

    const auto f = mBindingMap.find(name);
    if (LLVM_LIKELY(f != mBindingMap.end())) {

        const BindingMapEntry & entry = f->second;
        switch (entry.Type) {
            case BindingType::StreamInput:
            case BindingType::StreamOutput:
                return StreamSetPort(static_cast<PortType>(entry.Type), entry.Index);
            default: break;
        }
    }

    SmallVector<char, 256> tmp;
    raw_svector_ostream out(tmp);
    out << "Kernel " << getName() << " does not contain a streamset named: \"" << name << "\"";
    report_fatal_error(out.str());
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getScalarValuePtr
 ** ------------------------------------------------------------------------------------------------------------- */
Value * KernelCompiler::getScalarFieldPtr(KernelBuilder * /* b */, const StringRef name) const {
    if (LLVM_UNLIKELY(mScalarFieldMap.empty())) {
        SmallVector<char, 256> tmp;
        raw_svector_ostream out(tmp);
        out << "Scalar map for " << getName() << " was not initialized prior to calling getScalarValuePtr";
        report_fatal_error(out.str());
    } else {
        const auto f = mScalarFieldMap.find(name);
        if (LLVM_UNLIKELY(f == mScalarFieldMap.end())) {
            #ifdef NDEBUG
            SmallVector<char, 1024> tmp;
            raw_svector_ostream out(tmp);
            #else
            auto & out = errs();
            #endif
            out << "Scalar map for " << getName() << " does not contain " << name << "\n\n"
                "Currently contains:";
            char spacer = ' ';
            for (const auto & entry : mScalarFieldMap) {
                out << spacer << entry.getKey();
                spacer = ',';
            }
            #ifdef NDEBUG
            report_fatal_error(out.str());
            #else
            out << "\n";
            assert (false);
            #endif

        }
        return f->second;
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getScalarValuePtr
 ** ------------------------------------------------------------------------------------------------------------- */
bool KernelCompiler::hasScalarField(const llvm::StringRef name) const {
    return mScalarFieldMap.count(name) == 0;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getLowerBound
 ** ------------------------------------------------------------------------------------------------------------- */
Rational KernelCompiler::getLowerBound(const Binding & binding) const {
    const ProcessingRate & rate = binding.getRate();
    if (rate.hasReference()) {
        return rate.getLowerBound() * getLowerBound(getStreamBinding(rate.getReference()));
    } else {
        return rate.getLowerBound();
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getUpperBound
 ** ------------------------------------------------------------------------------------------------------------- */
Rational KernelCompiler::getUpperBound(const Binding & binding) const {
    const ProcessingRate & rate = binding.getRate();
    if (rate.hasReference()) {
        return rate.getUpperBound() * getUpperBound(getStreamBinding(rate.getReference()));
    } else {
        return rate.getUpperBound();
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief requiresOverflow
 ** ------------------------------------------------------------------------------------------------------------- */
bool KernelCompiler::requiresOverflow(const Binding & binding) const {
    const ProcessingRate & rate = binding.getRate();
    if (rate.isFixed() || binding.hasAttribute(AttrId::BlockSize)) {
        return false;
    } else if (rate.isRelative()) {
        return requiresOverflow(getStreamBinding(rate.getReference()));
    } else {
        return true;
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief clearInternalStateAfterCodeGen
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::clearInternalStateAfterCodeGen() {
    for (const auto & buffer : mStreamSetInputBuffers) {
        buffer->setHandle(nullptr);
    }
    for (const auto & buffer : mStreamSetOutputBuffers) {
        buffer->setHandle(nullptr);
    }
    mScalarFieldMap.clear();
    mSharedHandle = nullptr;
    mThreadLocalHandle = nullptr;
    mExternalSegNo = nullptr;
    mCurrentMethod = nullptr;
    mEntryPoint = nullptr;
    mIsFinal = nullptr;
    mNumOfStrides = nullptr;
    mTerminationSignalPtr = nullptr;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief runInternalOptimizationPasses
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::runInternalOptimizationPasses(Module * const m) {

    #ifndef NDEBUG
    SmallVector<char, 256> tmp;
    raw_svector_ostream msg(tmp);
    if (LLVM_UNLIKELY(verifyModule(*m, &msg, nullptr))) {
        m->print(errs(), nullptr);
        report_fatal_error(msg.str());
    }
    #endif

    // Attempt to promote all of the allocas in the entry block into PHI nodes
    // and delete any unnecessary Alloca and GEP instructions.

    SmallVector<AllocaInst *, 32> allocas;

    for (Function & f : *m) {
        if (f.empty()) continue;

        BasicBlock & bb = f.getEntryBlock();

        Instruction * inst = bb.getFirstNonPHIOrDbgOrLifetime();
        while (inst) {
            for (unsigned i = 0; i < inst->getNumOperands(); ++i) {
                Value * const op = inst->getOperand(i);
                if (op == nullptr) {
                    report_fatal_error("null operand");
                }
            }


            Instruction * const nextNode = inst->getNextNode();
            if (isa<AllocaInst>(inst) || isa<GetElementPtrInst>(inst)) {
                if (LLVM_UNLIKELY(inst->getNumUses() == 0)) {
                    inst->eraseFromParent();
                    inst = nextNode;
                    continue;
                }
            }
            if (isa<AllocaInst>(inst)) {
                if (isAllocaPromotable(cast<AllocaInst>(inst))) {
                    allocas.push_back(cast<AllocaInst>(inst));
                }
            }
            inst = nextNode;
        }

        if (allocas.empty()) continue;

        DominatorTree dt(f);
        PromoteMemToReg(allocas, dt);
        allocas.clear();
    }



}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief constructor
 ** ------------------------------------------------------------------------------------------------------------- */
KernelCompiler::KernelCompiler(not_null<Kernel *> kernel) noexcept
: mTarget(kernel)
, mInputStreamSets(kernel->mInputStreamSets)
, mOutputStreamSets(kernel->mOutputStreamSets)
, mInputScalars(kernel->mInputScalars)
, mOutputScalars(kernel->mOutputScalars)
, mInternalScalars(kernel->mInternalScalars) {
    initializeIOBindingMap();
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief destructor
 ** ------------------------------------------------------------------------------------------------------------- */
KernelCompiler::~KernelCompiler() {

}

}
