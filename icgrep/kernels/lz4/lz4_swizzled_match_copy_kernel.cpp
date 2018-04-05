//
// Created by wxy325 on 2018/3/9.
//

#include "lz4_swizzled_match_copy_kernel.h"
#include <kernels/kernel_builder.h>
#include <kernels/streamset.h>
#include <toolchain/toolchain.h>


using namespace llvm;
using namespace kernel;
using namespace std;

Value* LZ4SwizzledMatchCopyKernel::loadInt64NumberInput(const unique_ptr<KernelBuilder> &iBuilder, string bufferName, Value* offset) {
    // GEP here is safe
    Value* SIZE_BIT_BLOCK_WIDTH = iBuilder->getSize(iBuilder->getBitBlockWidth());
    Value* inputLocalBlockIndex = iBuilder->CreateUDiv(offset, SIZE_BIT_BLOCK_WIDTH);
    Value* inputLocalBlockOffset = iBuilder->CreateURem(offset, SIZE_BIT_BLOCK_WIDTH);

    Value* blockBasePtr = iBuilder->getInputStreamBlockPtr(bufferName, iBuilder->getSize(0), inputLocalBlockIndex);
    blockBasePtr = iBuilder->CreatePointerCast(blockBasePtr, iBuilder->getInt64Ty()->getPointerTo());
    // GEP here is safe
    return iBuilder->CreateLoad(iBuilder->CreateGEP(blockBasePtr, inputLocalBlockOffset));
}

void LZ4SwizzledMatchCopyKernel::generateMultiBlockLogic(const std::unique_ptr<KernelBuilder> &iBuilder, llvm::Value * const numOfStrides)  {
    // Const
    Constant *SIZE_ZERO = iBuilder->getSize(0);
    Constant *SIZE_ONE = iBuilder->getSize(1);
    Constant *SIZE_BIT_BLOCK_WIDTH = iBuilder->getSize(iBuilder->getBitBlockWidth());
    Constant *SIZE_PDEP_WIDTH = iBuilder->getSize(mPDEPWidth);

    BasicBlock* exitBlock = iBuilder->CreateBasicBlock("exitBlock");

    Value *itemsToDo = mAvailableItemCount[3];
    Value *processedItemCount = mInitialProcessedItemCount[3];
    Value *totalItemCount = iBuilder->CreateAdd(itemsToDo, processedItemCount);
    Value *isFinalBlock = iBuilder->CreateICmpULT(itemsToDo, iBuilder->getSize(4 * 1024 * 1024));
    this->mIsFinalBlock = isFinalBlock;
    iBuilder->setTerminationSignal(isFinalBlock);

    Value *previousProducedItemCount = iBuilder->getProducedItemCount("outputStreamSet0");

    // Space Calculation
    Value *outputBufferBlocks = iBuilder->getSize(
            this->getAnyStreamSetBuffer("outputStreamSet0")->getBufferBlocks());

    Value *outputBlocks = iBuilder->getSize(4 * 1024 * 1024 / iBuilder->getBitBlockWidth()); // Always be 4MB


    BasicBlock* processBlock = iBuilder->CreateBasicBlock("processBlock");
    Value* isInputEnough = iBuilder->CreateOr(isFinalBlock, iBuilder->CreateICmpUGE(itemsToDo, iBuilder->getSize(4 * 1024 * 1024)));

    iBuilder->CreateCondBr(isInputEnough, processBlock, exitBlock);

    iBuilder->SetInsertPoint(processBlock);
    // Output Copy
    this->generateOutputCopy(iBuilder, outputBlocks);

    Value *newProducedItemCount = iBuilder->getProducedItemCount("outputStreamSet0");

    BasicBlock *copyEndBlock = iBuilder->CreateBasicBlock("copyEnd");
    iBuilder->CreateBr(copyEndBlock);
    iBuilder->SetInsertPoint(copyEndBlock);

    // Match Copy
    BasicBlock *processExitBlock = iBuilder->CreateBasicBlock("exit_block");

    Value *initM0StartProcessIndex = iBuilder->getProcessedItemCount("m0Start");
    Value *totalM0StartItemsCount = iBuilder->CreateAdd(initM0StartProcessIndex, mAvailableItemCount[0]);

    Value *initMatchOffset = iBuilder->getScalarField("pendingMatchOffset");
    Value *initMatchLength = iBuilder->getScalarField("pendingMatchLength");
    Value *initMatchPos = iBuilder->getScalarField("pendingMatchPos");

    BasicBlock *matchCopyLoopCon = iBuilder->CreateBasicBlock("matchCopyLoopCon");
    iBuilder->CreateBr(matchCopyLoopCon);

    iBuilder->SetInsertPoint(matchCopyLoopCon);


    PHINode *phiProcessIndex = iBuilder->CreatePHI(iBuilder->getSizeTy(), 3);
    phiProcessIndex->addIncoming(initM0StartProcessIndex, copyEndBlock);

    PHINode *phiMatchOffset = iBuilder->CreatePHI(iBuilder->getSizeTy(), 3);
    phiMatchOffset->addIncoming(initMatchOffset, copyEndBlock);

    PHINode *phiMatchLength = iBuilder->CreatePHI(iBuilder->getSizeTy(), 3);
    phiMatchLength->addIncoming(initMatchLength, copyEndBlock);

    PHINode *phiMatchPos = iBuilder->CreatePHI(iBuilder->getSizeTy(), 3);
    phiMatchPos->addIncoming(initMatchPos, copyEndBlock);

    BasicBlock *loadNextMatchInfoConBlock = iBuilder->CreateBasicBlock("loadNewMatchInfoConBlock");
    BasicBlock *loadNextMatchInfoBodyBlock = iBuilder->CreateBasicBlock("loadNewMatchInfoBodyBlock");

    BasicBlock *matchCopyConBlock = iBuilder->CreateBasicBlock("matchCopyConBlock");
    BasicBlock *matchCopyBodyBlock = iBuilder->CreateBasicBlock("matchCopyBodyBlock");


    iBuilder->CreateCondBr(
            iBuilder->CreateICmpEQ(phiMatchLength, iBuilder->getSize(0)),
            loadNextMatchInfoConBlock,
            matchCopyConBlock
    );


    iBuilder->SetInsertPoint(loadNextMatchInfoConBlock);

    Value *hasMoreMatchInfo = iBuilder->CreateICmpULT(phiProcessIndex, totalM0StartItemsCount);
    iBuilder->CreateCondBr(hasMoreMatchInfo, loadNextMatchInfoBodyBlock, processExitBlock);

    iBuilder->SetInsertPoint(loadNextMatchInfoBodyBlock);


    Value *m0StartBaseOffset = iBuilder->CreateURem(initM0StartProcessIndex, SIZE_BIT_BLOCK_WIDTH);
    Value *m0StartLoadOffset = iBuilder->CreateAdd(m0StartBaseOffset,
                                                   iBuilder->CreateSub(phiProcessIndex, initM0StartProcessIndex));


    Value *newM0Start = this->loadInt64NumberInput(iBuilder, "m0Start", m0StartLoadOffset);
    Value *newM0End = this->loadInt64NumberInput(iBuilder, "m0End", m0StartLoadOffset);
    Value *newMatchOffset = this->loadInt64NumberInput(iBuilder, "matchOffset", m0StartLoadOffset);

    Value *depositStart = newM0Start;

    Value *depositEnd = iBuilder->CreateAdd(newM0End, iBuilder->getInt64(1));
    Value *newMatchLength = iBuilder->CreateSub(depositEnd, depositStart);
    phiProcessIndex->addIncoming(iBuilder->CreateAdd(phiProcessIndex, SIZE_ONE), iBuilder->GetInsertBlock());

    phiMatchPos->addIncoming(depositStart, iBuilder->GetInsertBlock());
    phiMatchOffset->addIncoming(newMatchOffset, iBuilder->GetInsertBlock());
    phiMatchLength->addIncoming(newMatchLength, iBuilder->GetInsertBlock());

    iBuilder->CreateBr(matchCopyLoopCon);


    iBuilder->SetInsertPoint(matchCopyConBlock);
    Value *hasNotReachEnd = iBuilder->CreateICmpULT(phiMatchPos, newProducedItemCount);
//    iBuilder->CallPrintInt("newProducedItemCount", newProducedItemCount);
    iBuilder->CreateCondBr(hasNotReachEnd, matchCopyBodyBlock, processExitBlock);

    iBuilder->SetInsertPoint(matchCopyBodyBlock);


    Value* matchCopyFromPos = iBuilder->CreateSub(phiMatchPos, phiMatchOffset);
    Value* outputBufferSize = iBuilder->CreateMul(outputBufferBlocks, SIZE_BIT_BLOCK_WIDTH);
    Value* matchCopyFromOffset = iBuilder->CreateURem(matchCopyFromPos, outputBufferSize);
    Value* matchCopyFromBlockIndex = iBuilder->CreateUDiv(matchCopyFromOffset, SIZE_PDEP_WIDTH);
    Value* matchCopyFromBlockOffset = iBuilder->CreateURem(matchCopyFromOffset, SIZE_PDEP_WIDTH);

    Value* matchCopyTargetBlockIndex = iBuilder->CreateUDiv(iBuilder->CreateSub(phiMatchPos, previousProducedItemCount), SIZE_PDEP_WIDTH);
    Value* matchCopyTargetBlockOffset = iBuilder->CreateURem(phiMatchPos, SIZE_PDEP_WIDTH);


    Value* matchCopyFromRemain = iBuilder->CreateSub(SIZE_PDEP_WIDTH, matchCopyFromBlockOffset);
    Value* matchCopyTargetRemain = iBuilder->CreateSub(SIZE_PDEP_WIDTH, matchCopyTargetBlockOffset);

    Value* currentCopySize = iBuilder->CreateUMin(matchCopyFromRemain, matchCopyTargetRemain);
    currentCopySize = iBuilder->CreateUMin(currentCopySize, phiMatchOffset);
    currentCopySize = iBuilder->CreateUMin(currentCopySize, phiMatchLength);
    currentCopySize = iBuilder->CreateUMin(currentCopySize, iBuilder->CreateSub(newProducedItemCount, phiMatchPos));
    currentCopySize = iBuilder->CreateSelect(iBuilder->CreateICmpEQ(currentCopySize, SIZE_ZERO), SIZE_ONE, currentCopySize); //Workaround for the last byte
    Value* singleMask = iBuilder->CreateSub(
            iBuilder->CreateSelect( // When currentCopySize == SIZE_PDEP_WIDTH, shl will cause overflow
                    iBuilder->CreateICmpEQ(currentCopySize, SIZE_PDEP_WIDTH),
                    SIZE_ZERO,
                    iBuilder->CreateShl(SIZE_ONE, iBuilder->CreateAdd(matchCopyFromBlockOffset, currentCopySize))
            ),
            iBuilder->CreateShl(SIZE_ONE, matchCopyFromBlockOffset)
    );
    Value* fullMask = iBuilder->simd_fill(mPDEPWidth, singleMask);

    for (int i = 0; i < mStreamSize; i++) {
        Value* rawOutputBasePtr = iBuilder->getRawOutputPointer("outputStreamSet" + std::to_string(i), SIZE_ZERO);
        rawOutputBasePtr = iBuilder->CreatePointerCast(rawOutputBasePtr, iBuilder->getBitBlockType()->getPointerTo());
        Value* matchCopyFromBlockPtr = iBuilder->CreateGEP(rawOutputBasePtr, matchCopyFromBlockIndex);

        Value* fromBlockValue = iBuilder->CreateLoad(matchCopyFromBlockPtr);

        Value* copiedValue = iBuilder->simd_and(fromBlockValue, fullMask);

        Value* outputBlockBasePtr = iBuilder->CreatePointerCast(iBuilder->getOutputStreamBlockPtr("outputStreamSet" + std::to_string(i), SIZE_ZERO), iBuilder->getBitBlockType()->getPointerTo());
        Value* outputTargetBlockPtr = iBuilder->CreateGEP(outputBlockBasePtr, matchCopyTargetBlockIndex);
        Value* targetOriginalValue = iBuilder->CreateLoad(outputTargetBlockPtr);

        Value* finalValue = iBuilder->simd_or(
                targetOriginalValue,
                iBuilder->CreateShl(
                        iBuilder->CreateLShr(
                                copiedValue,
                                iBuilder->simd_fill(mPDEPWidth, matchCopyFromBlockOffset)
                        ),
                        iBuilder->simd_fill(mPDEPWidth, matchCopyTargetBlockOffset)
                )
        );


//        iBuilder->CallPrintRegister("targetOriginalValue", targetOriginalValue);
//        iBuilder->CallPrintRegister("finalValue", finalValue);
//        iBuilder->CallPrintInt("matchCopyTargetBlockOffset", matchCopyTargetBlockOffset);
//        iBuilder->CallPrintInt("currentCopySize", currentCopySize);
        iBuilder->CreateStore(finalValue, outputTargetBlockPtr);
    }

    phiProcessIndex->addIncoming(phiProcessIndex, iBuilder->GetInsertBlock());
    phiMatchOffset->addIncoming(phiMatchOffset, iBuilder->GetInsertBlock());
    phiMatchPos->addIncoming(iBuilder->CreateAdd(phiMatchPos, currentCopySize), iBuilder->GetInsertBlock());
    phiMatchLength->addIncoming(iBuilder->CreateSub(phiMatchLength, currentCopySize), iBuilder->GetInsertBlock());

    iBuilder->CreateBr(matchCopyLoopCon);

    iBuilder->SetInsertPoint(processExitBlock);
    iBuilder->setScalarField("pendingMatchOffset", phiMatchOffset);
    iBuilder->setScalarField("pendingMatchLength", phiMatchLength);
    iBuilder->setScalarField("pendingMatchPos", phiMatchPos);
    iBuilder->setProcessedItemCount("m0Start", phiProcessIndex);
    iBuilder->setProcessedItemCount("m0End", phiProcessIndex);
    iBuilder->setProcessedItemCount("matchOffset", phiProcessIndex);

    iBuilder->CreateBr(exitBlock);
    iBuilder->SetInsertPoint(exitBlock);
}

void LZ4SwizzledMatchCopyKernel::generateOutputCopy(const std::unique_ptr<KernelBuilder> &iBuilder, llvm::Value* outputBlocks) {
    Value *SIZE_ZERO = iBuilder->getSize(0);
    Value *SIZE_BIT_BLOCK_WIDTH = iBuilder->getSize(iBuilder->getBitBlockWidth());
    Type* bytePtrType = iBuilder->getInt8PtrTy();

    Value *previousProcessed = iBuilder->getProcessedItemCount("sourceStreamSet0");


    Value *itemsToDo = mAvailableItemCount[3];
    Value *copySize = iBuilder->CreateMul(outputBlocks, SIZE_BIT_BLOCK_WIDTH);
    Value* actualCopySize = iBuilder->CreateUMin(itemsToDo, copySize);
    Value* copyByte = iBuilder->CreateUDivCeil(iBuilder->CreateMul(copySize, iBuilder->getSize(mStreamCount)), iBuilder->getSize(8)); // i8


    for (int i = 0; i < mStreamSize; i++) {
        Value *inputBasePtr = iBuilder->getInputStreamBlockPtr("sourceStreamSet" + std::to_string(i), SIZE_ZERO);
        Value *outputBasePtr = iBuilder->getOutputStreamBlockPtr("outputStreamSet" + std::to_string(i), SIZE_ZERO);
        iBuilder->CreateMemCpy(
                iBuilder->CreatePointerCast(outputBasePtr, bytePtrType),
                iBuilder->CreatePointerCast(inputBasePtr, bytePtrType),
                copyByte,
                1 // Not align guaranteed in final block
        );
    }
    Value *newProcessed = iBuilder->CreateAdd(previousProcessed, actualCopySize);
    iBuilder->setProcessedItemCount("sourceStreamSet0", newProcessed);
//    iBuilder->CallPrintInt("swizzledMatchCopy:newProcessed", newProcessed);
    iBuilder->setProducedItemCount("outputStreamSet0", newProcessed);
}

Value* LZ4SwizzledMatchCopyKernel::getMaximumMatchCopyBlock(const std::unique_ptr<KernelBuilder> &iBuilder) {
    Value *SIZE_BIT_BLOCK_WIDTH = iBuilder->getSize(iBuilder->getBitBlockWidth());
    Value *SIZE_ZERO = iBuilder->getSize(0);
    Value *SIZE_ONE = iBuilder->getSize(1);
    Value *m0EndInitOffset = iBuilder->CreateURem(iBuilder->getProcessedItemCount("m0End"), SIZE_BIT_BLOCK_WIDTH);
    Value *m0EndItemsToDo = mAvailableItemCount[1];
    Value *m0EndBasePtr = iBuilder->getInputStreamBlockPtr("m0End", SIZE_ZERO);
    m0EndBasePtr = iBuilder->CreatePointerCast(m0EndBasePtr, iBuilder->getInt64Ty()->getPointerTo());
    Value *lastM0 = iBuilder->CreateLoad(
            iBuilder->CreateGEP(
                    m0EndBasePtr,
                    iBuilder->CreateSub(
                            iBuilder->CreateAdd(m0EndInitOffset, m0EndItemsToDo),
                            SIZE_ONE
                    )

            )
    );
    Value *lastDepositPosition = iBuilder->CreateAdd(lastM0, SIZE_ONE);

    Value *currentMaxBlock = iBuilder->CreateSelect(
            this->mIsFinalBlock,
            iBuilder->CreateUDivCeil(lastDepositPosition, SIZE_BIT_BLOCK_WIDTH),
            iBuilder->CreateUDiv(lastDepositPosition, SIZE_BIT_BLOCK_WIDTH)
    );

    // Produced Item Count will always be full bitblock except for final block
    Value *previousProducedBlocks = iBuilder->CreateUDiv(
            iBuilder->getProducedItemCount("outputStreamSet0"),
            SIZE_BIT_BLOCK_WIDTH
    );

    // (m0 + 1) / BitBlockWidth - produceItemCount / BitBlockWidth
    return iBuilder->CreateSub(currentMaxBlock, previousProducedBlocks);
}

LZ4SwizzledMatchCopyKernel::LZ4SwizzledMatchCopyKernel(const std::unique_ptr<kernel::KernelBuilder> &iBuilder, unsigned streamCount/*=4*/, unsigned streamSize/*=2*/, unsigned swizzleFactor/*=4*/, unsigned PDEP_width/*64*/)
        : MultiBlockKernel("LZ4SwizzledMatchCopyKernel",
        // Inputs
                           {
                                   Binding{iBuilder->getStreamSetTy(1, 64), "m0Start", BoundedRate(0, 1), AlwaysConsume()},
                                   Binding{iBuilder->getStreamSetTy(1, 64), "m0End", BoundedRate(0, 1), AlwaysConsume()},
                                   Binding{iBuilder->getStreamSetTy(1, 64), "matchOffset", BoundedRate(0, 1), AlwaysConsume()},

                           },
        // Outputs
                           {},
        // Arguments
                           {
                                   Binding{iBuilder->getSizeTy(), "fileSize"} //TODO remove
                           },
                           {},
                           {
                                   Binding{iBuilder->getSizeTy(), "currentProcessIndex"},
                                   Binding{iBuilder->getSizeTy(), "pendingMatchPos"},
                                   Binding{iBuilder->getSizeTy(), "pendingMatchOffset"},
                                   Binding{iBuilder->getSizeTy(), "pendingMatchLength"},
                           })
        , mSwizzleFactor(swizzleFactor)
        , mPDEPWidth(PDEP_width)
        , mStreamSize(streamSize)
        , mStreamCount(streamCount) {

    assert((mSwizzleFactor == (iBuilder->getBitBlockWidth() / PDEP_width)) && "swizzle factor must equal bitBlockWidth / PDEP_width");
    assert((mPDEPWidth == 64 || mPDEPWidth == 32) && "PDEP width must be 32 or 64");
    this->setStride(4 * 1024 * 1024);
    addAttribute(MustExplicitlyTerminate());

    mStreamSetInputs.push_back(Binding{iBuilder->getStreamSetTy(streamCount), "sourceStreamSet0", BoundedRate(0, 1), {Swizzled(), DisableTemporaryBuffer()}});
    mStreamSetOutputs.push_back(Binding{iBuilder->getStreamSetTy(streamCount), "outputStreamSet0", BoundedRate(0, 1), DisableTemporaryBuffer()});

    for (int i = 1; i < streamSize; i++) {
        mStreamSetInputs.push_back(Binding{iBuilder->getStreamSetTy(streamCount), "sourceStreamSet" + std::to_string(i), RateEqualTo("sourceStreamSet0"), {Swizzled(), DisableTemporaryBuffer()}});
        mStreamSetOutputs.push_back(Binding{iBuilder->getStreamSetTy(streamCount), "outputStreamSet" + std::to_string(i), RateEqualTo("outputStreamSet0"), DisableTemporaryBuffer()});
    }
}
