#ifndef PIPELINE_COMPILER_HPP
#define PIPELINE_COMPILER_HPP

#include <kernels/pipeline_kernel.h>
#include <kernels/streamset.h>
#include <kernels/kernel_builder.h>
#include <toolchain/toolchain.h>
#include <boost/container/flat_set.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/topological_sort.hpp>
#include <boost/range/adaptor/reversed.hpp>
//#include <boost/serialization/strong_typedef.hpp>
#include <boost/math/common_factor_rt.hpp>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/ADT/STLExtras.h>
#include <queue>

//#define PRINT_DEBUG_MESSAGES

using namespace boost;
using namespace boost::math;
using namespace boost::adaptors;
using boost::container::flat_set;
using boost::container::flat_map;
using namespace llvm;

inline static unsigned floor_log2(const unsigned v) {
    assert ("log2(0) is undefined!" && v != 0);
    return 31 - __builtin_clz(v);
}

namespace kernel {

#include <util/enum_flags.hpp>

using Port = Kernel::Port;
using StreamPort = Kernel::StreamSetPort;
using AttrId = Attribute::KindId;
using RateValue = ProcessingRate::RateValue;
using RateId = ProcessingRate::KindId;
using Scalars = PipelineKernel::Scalars;
using Kernels = PipelineKernel::Kernels;
using CallBinding = PipelineKernel::CallBinding;
using BuilderRef = const std::unique_ptr<kernel::KernelBuilder> &;

// TODO: replace ints used for port #s with the following
// BOOST_STRONG_TYPEDEF(unsigned, PortNumber)

#warning TODO: these graphs are similar; look into streamlining their generation.

// TODO: split pipeline vertex into input and output vertices to keep all graphs DAGs
// without having to delete edges.

enum class BufferType : unsigned {
    Internal = 0
    , External = 1
    , Managed = 2
};

struct BufferNode {
    Value *             TotalItems;


    kernel::Kernel *    Kernel;
    StreamSetBuffer *   Buffer;

    RateValue           Lower;
    RateValue           Upper;

    unsigned            Overflow;
    unsigned            Fasimile;

    BufferType          Type;

    BufferNode() : TotalItems(nullptr), Kernel(nullptr), Buffer(nullptr), Lower(), Upper(), Overflow(0), Fasimile(0), Type(BufferType::Internal) {}
};

struct BufferRateData {

    RateValue Minimum;
    RateValue Maximum;
    unsigned  Port;

    BufferRateData() = default;

    BufferRateData(const unsigned port, RateValue min, RateValue max)
    : Minimum(std::move(min)), Maximum(std::move(max)), Port(port) { }
};

using BufferGraph = adjacency_list<vecS, vecS, bidirectionalS, BufferNode, BufferRateData>;

template <typename vertex_descriptor>
using RelationshipMap = flat_map<const Relationship *, vertex_descriptor>;

using BufferMap = RelationshipMap<BufferGraph::vertex_descriptor>;

struct ConsumerNode {
    Value * Consumed = nullptr;
    PHINode * PhiNode = nullptr;
};

enum : unsigned { FAKE_CONSUMER = (std::numeric_limits<unsigned>::max()) };

using ConsumerGraph = adjacency_list<vecS, vecS, bidirectionalS, ConsumerNode, unsigned>;

template <typename Value>
using StreamSetBufferMap = flat_map<const StreamSetBuffer *, Value>;

template <typename Value>
using KernelMap = flat_map<const Kernel *, Value>;

using TerminationGraph = adjacency_list<hash_setS, vecS, bidirectionalS, Value *>;

using ScalarDependencyGraph = adjacency_list<vecS, vecS, bidirectionalS, Value *, unsigned>;

struct OverflowRequirement {
    unsigned copyBack;
    unsigned facsimile;
    OverflowRequirement() = default;
    OverflowRequirement(const unsigned copyBack, const unsigned copyForward)
    : copyBack(copyBack), facsimile(copyForward) { }
};

using OverflowRequirements = StreamSetBufferMap<OverflowRequirement>;

struct PopCountData {
    // compilation state
    PHINode *    PhiNode;
    Value *      Processed;
    unsigned     Encountered;
    Value *      InitialOffset;

    // analysis state
    RateValue    FieldWidth;
    bool         HasArray;
    bool         HasNegatedArray;
    bool         UsesConsumedCount;
    bool         AlwaysNegated;

    PopCountData() = default;
};

enum CountingType : unsigned {
    Unknown = 0
    , Positive = 1
    , Negative = 2
    , Both = Positive | Negative
};

ENABLE_ENUM_FLAGS(CountingType)

struct PopCountEdge {
    CountingType Type;
    unsigned     Port;
    PopCountEdge() : Type(Unknown), Port(0) { }
    PopCountEdge(const CountingType type, const unsigned port) : Type(type), Port(port) { }
};

using PopCountGraph = adjacency_list<vecS, vecS, bidirectionalS, no_property, PopCountEdge>;

class PipelineCompiler {
public:

    PipelineCompiler(BuilderRef b, PipelineKernel * const pipelineKernel);

    void addPipelineKernelProperties(BuilderRef b);
    void generateInitializeMethod(BuilderRef b);
    void generateSingleThreadKernelMethod(BuilderRef b);
    void generateMultiThreadKernelMethod(BuilderRef b, const unsigned numOfThreads);
    void generateFinalizeMethod(BuilderRef b);
    std::vector<Value *> getFinalOutputScalars(BuilderRef b);

protected:

// internal pipeline state construction functions

    void addInternalKernelProperties(BuilderRef b, const unsigned kernelIndex);

// main pipeline functions

    void start(BuilderRef b, Value * const initialSegNo);
    void setActiveKernel(BuilderRef b, const unsigned index);
    void synchronize(BuilderRef b);
    void executeKernel(BuilderRef b);
    void end(BuilderRef b, const unsigned step);

// internal pipeline functions

    LLVM_READNONE StructType * getThreadStateType(BuilderRef b);
    AllocaInst * allocateThreadState(BuilderRef b, const unsigned segOffset);
    Value * setThreadState(BuilderRef b, Value * threadState);
    void deallocateThreadState(BuilderRef b, Value * const threadState);

    LLVM_READNONE StructType * getLocalStateType(BuilderRef b);
    void allocateThreadLocalState(BuilderRef b, Value * const localState);
    void setThreadLocalState(BuilderRef b, Value * const localState);
    void deallocateThreadLocalState(BuilderRef b, Value * const localState);

// inter-kernel functions

    void checkForSufficientInputDataAndOutputSpace(BuilderRef b);
    void determineNumOfLinearStrides(BuilderRef b);
    void calculateNonFinalItemCounts(BuilderRef b);
    void calculateFinalItemCounts(BuilderRef b);
    void writeKernelCall(BuilderRef b);
    void computeFullyProcessedItemCounts(BuilderRef b);
    void writeCopyBackLogic(BuilderRef b);
    void writeCopyForwardLogic(BuilderRef b);

    void zeroFillPartiallyWrittenOutputStreams(BuilderRef b);
    void initializeKernelCallPhis(BuilderRef b);
    void initializeKernelExitPhis(BuilderRef b);

    void readInitialProducedItemCounts(BuilderRef b);
    void computeMinimumConsumedItemCounts(BuilderRef b);
    void writeFinalConsumedItemCounts(BuilderRef b);
    void readFinalProducedItemCounts(BuilderRef b);
    void releaseCurrentSegment(BuilderRef b);
    void writeCopyToOverflowLogic(BuilderRef b);
    void checkForSufficientInputData(BuilderRef b, const unsigned inputPort);
    void checkForSufficientOutputSpaceOrExpand(BuilderRef b, const unsigned outputPort);
    void incrementItemCountsOfCountableRateStreams(BuilderRef b);
    enum class OverflowCopy { Forwards, Backwards };
    Value * writeOverflowCopy(BuilderRef b, const StreamSetBuffer * const buffer, const OverflowCopy direction, Value * const itemsToCopy) const;


// intra-kernel functions

    void branchToTargetOrLoopExit(BuilderRef b, Value * const cond, BasicBlock * const target);
    void expandOutputBuffers(BuilderRef b);
    void expandOutputBuffer(BuilderRef b, const unsigned outputPort, Value * const hasEnough, BasicBlock * const target);

    Value * getAlreadyProcessedItemCount(BuilderRef b, const unsigned inputPort);
    Value * getAlreadyProducedItemCount(BuilderRef b, const unsigned outputPort);

    Value * getInputStrideLength(BuilderRef b, const unsigned inputPort);
    Value * getOutputStrideLength(BuilderRef b, const unsigned outputPort);
    Value * getInitialStrideLength(BuilderRef b, const Port port, const unsigned portNum);
    Value * getMaximumStrideLength(BuilderRef b, const Port port, const unsigned portNum);
    Value * calculateNumOfLinearItems(BuilderRef b, const Port portType,  const unsigned portNum);
    Value * getAccessibleInputItems(BuilderRef b, const unsigned inputPort);
    Value * getNumOfAccessibleStrides(BuilderRef b, const unsigned inputPort);
    Value * getNumOfWritableStrides(BuilderRef b, const unsigned outputPort);
    Value * getWritableOutputItems(BuilderRef b, const unsigned outputPort);
    Value * calculateBufferExpansionSize(BuilderRef b, const unsigned outputPort);
    Value * willNotOverwriteOverflow(BuilderRef b, const unsigned outputPort);
    Value * addLookahead(BuilderRef b, const unsigned inputPort, Value * itemCount) const;
    Value * subtractLookahead(BuilderRef b, const unsigned inputPort, Value * itemCount) const;
    Constant * getLookahead(BuilderRef b, const unsigned inputPort) const;
    Value * truncateBlockSize(BuilderRef b, const Binding & binding, Value * itemCount, Value * all) const;
    Value * getTotalItemCount(BuilderRef b, const unsigned inputPort) const;
    Value * hasProducerTerminated(BuilderRef b, const unsigned inputPort) const;
    Value * getInitialTerminationSignal(BuilderRef b) const;
    void setTerminated(BuilderRef b, Value * const terminated);
    void resetMemoizedFields();

// pop-count functions

    void addPopCountScalarsToPipelineKernel(BuilderRef b, const unsigned index);

    void initializePopCounts();
    void writePopCountComputationLogic(BuilderRef b);

    void initializePopCountReferenceItemCount(BuilderRef b, const unsigned bufferVertex, not_null<Value *> produced);
    void createPopCountReferenceCounts(BuilderRef b);
    void computeMinimumPopCountReferenceCounts(BuilderRef b);
    void updatePopCountReferenceCounts(BuilderRef b);
    LLVM_READNONE Value * getPopCountReferenceConsumedCount(BuilderRef b, const unsigned bufferVertex);
    LLVM_READNONE Value * getPopCountInitialOffset(BuilderRef b, const Binding & binding, const unsigned bufferVertex, PopCountData & pc);

    Value * getMinimumNumOfLinearPopCountItems(BuilderRef b, const Binding & binding);
    Value * getMaximumNumOfPopCountStrides(BuilderRef b, const Binding & binding, not_null<Value *> sourceItemCount, Constant * const lookAhead = nullptr);
    Value * getNumOfLinearPopCountItems(BuilderRef b, const Binding & binding);

    Value * getReferenceStreamOffset(BuilderRef b, const Binding & binding);

    Value * getPopCountArray(BuilderRef b, const unsigned inputPort);
    Value * getNegatedPopCountArray(BuilderRef b, const unsigned inputPort);
    Value * getIndividualPopCountArray(BuilderRef b, const unsigned inputPort, const unsigned index);

    LLVM_READNONE unsigned getPopCountReferencePort(const Kernel * kernel, const ProcessingRate & rate) const;
    LLVM_READNONE unsigned getPopCountReferenceBuffer(const Kernel * kernel, const ProcessingRate & rate) const;

    LLVM_READNONE StructType * getPopCountThreadLocalStateType(BuilderRef b);
    void allocatePopCountArrays(BuilderRef b, Value * base);
    void deallocatePopCountArrays(BuilderRef b, Value * base);

// pop-count analysis

    PopCountGraph makePopCountGraph() const;

    LLVM_READNONE PopCountData & getPopCountReference(const unsigned bufferVertex) ;
    LLVM_READNONE PopCountData analyzePopCountReference(const unsigned bufferVertex) const;
    LLVM_READNONE RateValue popCountReferenceFieldWidth(const unsigned bufferVertex) const;
    LLVM_READNONE bool popCountReferenceCanUseConsumedItemCount(const unsigned bufferVertex) const;
    LLVM_READNONE bool popCountReferenceRequiresBaseOffset(const unsigned bufferVertex) const;
    LLVM_READNONE bool popCountBufferIsUsedBySingleKernel(const unsigned bufferVertex) const;
    LLVM_READNONE std::pair<bool, bool> popCountReferenceRequiresPopCountArray(const unsigned bufferVertex) const;
    LLVM_READNONE bool popCountReferenceIsAlwaysNegated(const unsigned bufferVertex) const;

    template <typename LambdaFunction>
    void forEachOutputBufferThatIsAPopCountReference(const unsigned kernelIndex, LambdaFunction func);

    template <typename LambdaFunction>
    void forEachPopCountReferenceInputPort(const unsigned kernelIndex, LambdaFunction func);

// consumer recording

    ConsumerGraph makeConsumerGraph() const;
    void createConsumedPhiNodes(BuilderRef b);
    void initializeConsumedItemCount(BuilderRef b, const unsigned bufferVertex, Value * const produced);
    void setConsumedItemCount(BuilderRef b, const unsigned bufferVertex, Value * const consumed) const;
    Value * getConsumedItemCount(BuilderRef b, const unsigned outputPort) const;

// buffer analysis/management functions

    BufferGraph makeBufferGraph(BuilderRef b);
    void addBufferHandlesToPipelineKernel(BuilderRef b, const unsigned index);
    void enumerateBufferProducerBindings(const unsigned producer, const Bindings & bindings, BufferGraph & G, BufferMap & M);
    void enumerateBufferConsumerBindings(const unsigned consumer, const Bindings & bindings, BufferGraph & G, BufferMap & M);
    BufferRateData getBufferRateData(const unsigned index, const unsigned port, bool input);

    void constructBuffers(BuilderRef b);
    void loadBufferHandles(BuilderRef b);
    void releaseBuffers(BuilderRef b);
    LLVM_READNONE bool requiresCopyBack(const unsigned bufferVertex) const;
    LLVM_READNONE bool requiresFacsimile(const unsigned bufferVertex) const;
    LLVM_READNONE unsigned getCopyBack(const unsigned bufferVertex) const;
    LLVM_READNONE unsigned getFacsimile(const unsigned bufferVertex) const;
    BufferType getOutputBufferType(const unsigned outputPort) const;

    Value * getLogicalInputBaseAddress(BuilderRef b, const unsigned inputPort);
    Value * getLogicalOutputBaseAddress(BuilderRef b, const unsigned outputPort);
    Value * calculateLogicalBaseAddress(BuilderRef b, const Binding & binding, const StreamSetBuffer * const buffer, Value * const itemCount);

// cycle counter functions

    void startOptionalCycleCounter(BuilderRef b);
    void updateOptionalCycleCounter(BuilderRef b);
    void printOptionalCycleCounter(BuilderRef b);

// analysis functions

    std::vector<unsigned> lexicalOrderingOfStreamIO() const;
    TerminationGraph makeTerminationGraph() const;
    ScalarDependencyGraph makeScalarDependencyGraph() const;

// misc. functions

    Value * getFunctionFromKernelState(BuilderRef b, Type * const type, const std::string & suffix) const;
    Value * getInitializationFunction(BuilderRef b) const;
    Value * getDoSegmentFunction(BuilderRef b) const;
    Value * getFinalizeFunction(BuilderRef b) const;

    std::string makeKernelName(const unsigned kernelIndex) const;
    std::string makeBufferName(const unsigned kernelIndex, const Binding & binding) const;
    unsigned getInputBufferVertex(const unsigned inputPort) const;
    unsigned getInputBufferVertex(const unsigned kernelVertex, const unsigned inputPort) const;
    StreamSetBuffer * getInputBuffer(const unsigned inputPort) const;
    unsigned getOutputBufferVertex(const unsigned outputPort) const;
    unsigned getOutputBufferVertex(const unsigned kernelVertex, const unsigned outputPort) const;
    StreamSetBuffer * getOutputBuffer(const unsigned outputPort) const;

    static LLVM_READNONE const Binding & getBinding(const Kernel * kernel, const Port port, const unsigned i) {
        if (port == Port::Input) {
            return kernel->getInputStreamSetBinding(i);
        } else if (port == Port::Output) {
            return kernel->getOutputStreamSetBinding(i);
        }
        llvm_unreachable("unknown port binding type!");
    }

    void printBufferGraph(const BufferGraph & G, raw_ostream & out);

    LLVM_READNONE const Binding & getInputBinding(const Kernel * const producer, const unsigned index) const;

    LLVM_READNONE const Binding & getOutputBinding(const Kernel * const consumer, const unsigned index) const;

    void writeOutputScalars(BuilderRef b, const unsigned u, std::vector<Value *> & args);

    void itemCountSanityCheck(BuilderRef b, const Binding & binding, const std::string & pastLabel,
                              Value * const itemCount, Value * const expected);

protected:

    PipelineKernel * const                      mPipelineKernel;
    const Kernels &                             mPipeline;

    OwnedStreamSetBuffers                       mOwnedBuffers;
    unsigned                                    mKernelIndex = 0;
    Kernel *                                    mKernel = nullptr;

    // pipeline state
    PHINode *                                   mTerminatedPhi = nullptr;
    PHINode *                                   mTerminatedFlag = nullptr;
    PHINode *                                   mSegNo = nullptr;
    BasicBlock *                                mPipelineLoop = nullptr;
    BasicBlock *                                mKernelEntry = nullptr;
    BasicBlock *                                mKernelLoopEntry = nullptr;
    BasicBlock *                                mKernelLoopCall = nullptr;
    BasicBlock *                                mKernelLoopExit = nullptr;
    BasicBlock *                                mKernelLoopExitPhiCatch = nullptr;
    BasicBlock *                                mKernelExit = nullptr;
    BasicBlock *                                mPipelineEnd = nullptr;
    std::vector<Value *>                        mOutputScalars;

    // kernel state
    Value *                                     mNumOfLinearStrides = nullptr;
    Value *                                     mTerminationExplicitly = nullptr;

    std::vector<unsigned>                       mPortOrdering;

    std::vector<Value *>                        mAlreadyProcessedItemCount; // entering the stride
    std::vector<Value *>                        mInputStrideLength;
    std::vector<Value *>                        mAccessibleInputItems;
    std::vector<PHINode *>                      mLinearInputItemsPhi;
    std::vector<Value *>                        mFullyProcessedItemCount; // exiting the kernel

    std::vector<Value *>                        mInitiallyProducedItemCount; // entering the *kernel*
    std::vector<Value *>                        mAlreadyProducedItemCount; // entering the stride
    std::vector<Value *>                        mOutputStrideLength;
    std::vector<Value *>                        mWritableOutputItems;
    std::vector<PHINode *>                      mLinearOutputItemsPhi;

    // debug + misc state
    Value *                                     mCycleCountStart = nullptr;
    PHINode *                                   mDeadLockCounter = nullptr;
    Value *                                     mPipelineProgress = nullptr;
    PHINode *                                   mHasProgressedPhi = nullptr;
    PHINode *                                   mAlreadyProgressedPhi = nullptr;

    // popcount state
    Value *                                     mPopCountState;
    flat_map<unsigned, PopCountData>            mPopCountData;


    // analysis state
    BufferGraph                                 mBufferGraph;
    ConsumerGraph                               mConsumerGraph;
    ScalarDependencyGraph                       mScalarDependencyGraph;
    TerminationGraph                            mTerminationGraph;
    PopCountGraph                               mPopCountGraph;

};

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief constructor
 ** ------------------------------------------------------------------------------------------------------------- */
inline PipelineCompiler::PipelineCompiler(BuilderRef b, PipelineKernel * const pipelineKernel)
: mPipelineKernel(pipelineKernel)
, mPipeline(pipelineKernel->mKernels)
, mBufferGraph(makeBufferGraph(b))
, mConsumerGraph(makeConsumerGraph())
, mScalarDependencyGraph(makeScalarDependencyGraph())
, mTerminationGraph(makeTerminationGraph())
, mPopCountGraph(makePopCountGraph()) {


}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getInputBuffer
 ** ------------------------------------------------------------------------------------------------------------- */
inline unsigned PipelineCompiler::getInputBufferVertex(const unsigned inputPort) const {
    return getInputBufferVertex(mKernelIndex, inputPort);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getInputBuffer
 ** ------------------------------------------------------------------------------------------------------------- */
inline unsigned PipelineCompiler::getInputBufferVertex(const unsigned kernelVertex, const unsigned inputPort) const {
    for (const auto e : make_iterator_range(in_edges(kernelVertex, mBufferGraph))) {
        if (mBufferGraph[e].Port == inputPort) {
            return source(e, mBufferGraph);
        }
    }
    llvm_unreachable("input buffer not found");
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getInputBuffer
 ** ------------------------------------------------------------------------------------------------------------- */
inline StreamSetBuffer * PipelineCompiler::getInputBuffer(const unsigned inputPort) const {
    return mBufferGraph[getInputBufferVertex(inputPort)].Buffer;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getOutputBufferVertex
 ** ------------------------------------------------------------------------------------------------------------- */
inline unsigned PipelineCompiler::getOutputBufferVertex(const unsigned outputPort) const {
    return getOutputBufferVertex(mKernelIndex, outputPort);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getOutputBufferVertex
 ** ------------------------------------------------------------------------------------------------------------- */
inline unsigned PipelineCompiler::getOutputBufferVertex(const unsigned kernelVertex, const unsigned outputPort) const {
    for (const auto e : make_iterator_range(out_edges(kernelVertex, mBufferGraph))) {
        if (mBufferGraph[e].Port == outputPort) {
            return target(e, mBufferGraph);
        }
    }
    llvm_unreachable("output buffer not found");
}


/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getOutputBuffer
 ** ------------------------------------------------------------------------------------------------------------- */
inline StreamSetBuffer * PipelineCompiler::getOutputBuffer(const unsigned outputPort) const {
    return mBufferGraph[getOutputBufferVertex(outputPort)].Buffer;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief storedInKernel
 ** ------------------------------------------------------------------------------------------------------------- */
inline LLVM_READNONE bool storedInNestedKernel(const Binding & output) {
    return output.getRate().isUnknown() || output.hasAttribute(AttrId::ManagedBuffer);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief upperBound
 ** ------------------------------------------------------------------------------------------------------------- */
inline LLVM_READNONE RateValue upperBound(not_null<const Kernel *> kernel, const Binding & binding) {
    assert (kernel->getStride() > 0);
//    const auto ub = kernel->getUpperBound(binding);
//    const auto stride = kernel->getStride();
//    return (ub == 0) ? stride : ub * stride;
    return kernel->getUpperBound(binding) * kernel->getStride();
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief lowerBound
 ** ------------------------------------------------------------------------------------------------------------- */
inline LLVM_READNONE RateValue lowerBound(not_null<const Kernel *> kernel, const Binding & binding) {
    assert (kernel->getStride() > 0);
    return kernel->getLowerBound(binding) * kernel->getStride();
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief makeKernelName
 ** ------------------------------------------------------------------------------------------------------------- */
inline LLVM_READNONE std::string PipelineCompiler::makeKernelName(const unsigned kernelIndex) const {
    return PipelineKernel::makeKernelName(mPipeline[kernelIndex], kernelIndex);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief makeBufferName
 ** ------------------------------------------------------------------------------------------------------------- */
inline LLVM_READNONE std::string PipelineCompiler::makeBufferName(const unsigned kernelIndex, const Binding & binding) const {
    return PipelineKernel::makeBufferName(mPipeline[kernelIndex], kernelIndex, binding);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getRelationship
 ** ------------------------------------------------------------------------------------------------------------- */
inline const Relationship * getRelationship(not_null<const Relationship *> r) {
    return r.get();
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getRelationship
 ** ------------------------------------------------------------------------------------------------------------- */
inline const Relationship * getRelationship(const Binding & b) {
    return getRelationship(b.getRelationship());
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getLog2SizeWidth
 ** ------------------------------------------------------------------------------------------------------------- */
LLVM_READNONE inline Constant * getLog2SizeWidth(BuilderRef b) {
    return b->getSize(std::log2(b->getSizeTy()->getBitWidth()));
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getLog2BlockWidth
 ** ------------------------------------------------------------------------------------------------------------- */
LLVM_READNONE inline Constant * getLog2BlockWidth(BuilderRef b) {
    return b->getSize(std::log2(b->getBitBlockWidth()));
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getItemWidth
 ** ------------------------------------------------------------------------------------------------------------- */
LLVM_READNONE inline unsigned getItemWidth(const Type * ty ) {
    if (LLVM_LIKELY(isa<ArrayType>(ty))) {
        ty = ty->getArrayElementType();
    }
    return cast<IntegerType>(ty->getVectorElementType())->getBitWidth();
}

template <typename Graph>
inline typename graph_traits<Graph>::edge_descriptor first_in_edge(const typename graph_traits<Graph>::vertex_descriptor u, const Graph & G) {
    return *in_edges(u, G).first;
}

template <typename Graph>
inline typename graph_traits<Graph>::edge_descriptor in_edge(const typename graph_traits<Graph>::vertex_descriptor u, const Graph & G) {
    assert (in_degree(u, G) == 1);
    return first_in_edge(u, G);
}

template <typename Graph>
inline typename graph_traits<Graph>::vertex_descriptor parent(const typename graph_traits<Graph>::vertex_descriptor u, const Graph & G) {
    return source(in_edge(u, G), G);
}

template <typename Graph>
inline typename graph_traits<Graph>::edge_descriptor first_out_edge(const typename graph_traits<Graph>::vertex_descriptor u, const Graph & G) {
    return *out_edges(u, G).first;
}

template <typename Graph>
inline typename graph_traits<Graph>::edge_descriptor out_edge(const typename graph_traits<Graph>::vertex_descriptor u, const Graph & G) {
    assert (out_degree(u, G) == 1);
    return first_out_edge(u, G);
}

template <typename Graph>
inline typename graph_traits<Graph>::vertex_descriptor child(const typename graph_traits<Graph>::vertex_descriptor u, const Graph & G) {
    return target(out_edge(u, G), G);
}

} // end of namespace

#endif // PIPELINE_COMPILER_HPP
