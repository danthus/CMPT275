/*
 *  Copyright (c) 2016 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 */

    
#include <kernels/streamset.h>
#include <vector>
#include <IR_Gen/idisa_builder.h>
#include <llvm/IR/Type.h>

using namespace parabix;

enum SS_struct_index {iProducer_pos = 0, iConsumer_pos = 1, iEnd_of_input = 2, iBuffer_ptr = 3};

llvm::Value * StreamSetBuffer::getProducerPosPtr(Value * bufferStructPtr) {
    return iBuilder->CreateGEP(bufferStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iProducer_pos)});
}

void StreamSetBuffer::setProducerPos(Value * bufferStructPtr, llvm::Value * pos) {
    iBuilder->CreateStore(pos, getProducerPosPtr(bufferStructPtr));
}

llvm::Value * StreamSetBuffer::getConsumerPosPtr(Value * bufferStructPtr) {
    return iBuilder->CreateGEP(bufferStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iConsumer_pos)});
}

void StreamSetBuffer::setConsumerPos(Value * bufferStructPtr, Value * pos) {
    iBuilder->CreateStore(pos, getConsumerPosPtr(bufferStructPtr));
}

llvm::Value * StreamSetBuffer::getEndOfInputPtr(Value * bufferStructPtr) {
    return iBuilder->CreateGEP(bufferStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iEnd_of_input)});
}

void StreamSetBuffer::setEndOfInput(Value * bufferStructPtr) {
    iBuilder->CreateStore(ConstantInt::get(iBuilder->getInt1Ty(), 1), getEndOfInputPtr(bufferStructPtr));
}

void StreamSetBuffer::allocateBuffer() {
    Type * const size_ty = iBuilder->getSizeTy();
    Type * const int1ty = iBuilder->getInt1Ty();
    mStreamSetBufferPtr = iBuilder->CreateCacheAlignedAlloca(mStreamSetType, iBuilder->getSize(mBufferBlocks));
    mStreamSetStructPtr = iBuilder->CreateCacheAlignedAlloca(mStreamSetStructType);
    iBuilder->CreateStore(ConstantInt::get(size_ty, 0), iBuilder->CreateGEP(mStreamSetStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iProducer_pos)}));
    iBuilder->CreateStore(ConstantInt::get(size_ty, 0), iBuilder->CreateGEP(mStreamSetStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iConsumer_pos)}));
    iBuilder->CreateStore(ConstantInt::get(int1ty, 0), iBuilder->CreateGEP(mStreamSetStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iEnd_of_input)}));
    iBuilder->CreateStore(mStreamSetBufferPtr, iBuilder->CreateGEP(mStreamSetStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iBuffer_ptr)}));
}

// Single Block Buffer
// For a single block buffer, the block pointer is always the buffer base pointer.
llvm::Value * SingleBlockBuffer::getStreamSetBlockPointer(llvm::Value * bufferStructPtr, llvm::Value *) {
    Value * handle = iBuilder->CreateGEP(bufferStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iBuffer_ptr)});
    return iBuilder->CreateLoad(handle);
}


// External File Buffer

void ExternalFileBuffer::setStreamSetBuffer(llvm::Value * ptr, Value * fileSize) {
    
    Type * const size_ty = iBuilder->getSizeTy();
    Type * const int1ty = iBuilder->getInt1Ty();
    
    PointerType * t = getStreamBufferPointerType();    
    mStreamSetBufferPtr = iBuilder->CreatePointerBitCastOrAddrSpaceCast(ptr, t);
    
    mStreamSetStructPtr = iBuilder->CreateCacheAlignedAlloca(mStreamSetStructType);
    iBuilder->CreateStore(fileSize, iBuilder->CreateGEP(mStreamSetStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iProducer_pos)}));
    iBuilder->CreateStore(ConstantInt::get(size_ty, 0), iBuilder->CreateGEP(mStreamSetStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iConsumer_pos)}));
    iBuilder->CreateStore(ConstantInt::get(int1ty, 1), iBuilder->CreateGEP(mStreamSetStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iEnd_of_input)}));
    iBuilder->CreateStore(mStreamSetBufferPtr, iBuilder->CreateGEP(mStreamSetStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iBuffer_ptr)}));
}

void ExternalFileBuffer::setEmptyBuffer(llvm::Value * ptr) {
    
    Type * const size_ty = iBuilder->getSizeTy();
    Type * const int1ty = iBuilder->getInt1Ty();
    
    PointerType * t = getStreamBufferPointerType();    
    mStreamSetBufferPtr = iBuilder->CreatePointerBitCastOrAddrSpaceCast(ptr, t);
    
    mStreamSetStructPtr = iBuilder->CreateCacheAlignedAlloca(mStreamSetStructType);
    iBuilder->CreateStore(ConstantInt::get(size_ty, 0), iBuilder->CreateGEP(mStreamSetStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iProducer_pos)}));
    iBuilder->CreateStore(ConstantInt::get(size_ty, 0), iBuilder->CreateGEP(mStreamSetStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iConsumer_pos)}));
    iBuilder->CreateStore(ConstantInt::get(int1ty,0), iBuilder->CreateGEP(mStreamSetStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iEnd_of_input)}));
    iBuilder->CreateStore(mStreamSetBufferPtr, iBuilder->CreateGEP(mStreamSetStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iBuffer_ptr)}));
}

void ExternalFileBuffer::allocateBuffer() {
    throw std::runtime_error("External buffers cannot be allocated.");
}

llvm::Value * ExternalFileBuffer::getStreamSetBlockPointer(llvm::Value * bufferStructPtr, llvm::Value * blockNo) {
    Value * handle = iBuilder->CreateGEP(bufferStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iBuffer_ptr)});
    Value * bufPtr = iBuilder->CreateLoad(handle);
    return iBuilder->CreateGEP(bufPtr, blockNo);
}

// Circular Stack Allocated Buffer

llvm::Value * CircularBuffer::getStreamSetBlockPointer(llvm::Value * bufferStructPtr, llvm::Value * blockNo) {
    assert (blockNo->getType()->isIntegerTy());

    Value * handle = iBuilder->CreateGEP(bufferStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iBuffer_ptr)});
    Value * bufPtr = iBuilder->CreateLoad(handle);
    Value * offset = nullptr;
    if (mBufferBlocks == 1) {
        offset = ConstantInt::getNullValue(iBuilder->getSizeTy());
    } else if ((mBufferBlocks & (mBufferBlocks - 1)) == 0) { // is power of 2
        offset = iBuilder->CreateAnd(blockNo, ConstantInt::get(blockNo->getType(), mBufferBlocks - 1));
    } else {
        offset = iBuilder->CreateURem(blockNo, ConstantInt::get(blockNo->getType(), mBufferBlocks));
    }
    return iBuilder->CreateGEP(bufPtr, offset);
}

llvm::Value * LinearCopybackBuffer::getStreamSetBlockPointer(llvm::Value * bufferStructPtr, llvm::Value * blockNo) {
    Constant * blockWidth = iBuilder->getSize(iBuilder->getStride());
    Value * consumerPos_ptr = iBuilder->CreateGEP(bufferStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iConsumer_pos)});
    Value * consumerPos = iBuilder->CreateLoad(consumerPos_ptr);
    Value * consumerBlock = iBuilder->CreateUDiv(consumerPos, blockWidth);
    Value * handle = iBuilder->CreateGEP(bufferStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iBuffer_ptr)});
    Value * bufPtr = iBuilder->CreateLoad(handle);
    return iBuilder->CreateGEP(bufPtr, iBuilder->CreateSub(blockNo, consumerBlock));
}

void LinearCopybackBuffer::setConsumerPos(Value * bufferStructPtr, Value * newConsumerPos) {
    Type * const i8 = iBuilder->getInt8Ty();
    Type * const i8_ptr = i8->getPointerTo(mAddrSpace);
    IntegerType * const sizeTy = iBuilder->getSizeTy();

    Module * const M = iBuilder->getModule();

    Function * const current = iBuilder->GetInsertBlock()->getParent();
    BasicBlock * const copyBackBody = BasicBlock::Create(M->getContext(), "copy_back", current, 0);
    BasicBlock * const setConsumerPosExit = BasicBlock::Create(M->getContext(), "setConsumerPos_done", current, 0);
    Constant * const blockWidth = ConstantInt::get(sizeTy, iBuilder->getStride());

    Constant * const one = ConstantInt::get(sizeTy, 1);

    Value * const consumerPosPtr = getConsumerPosPtr(bufferStructPtr);
    Value * const consumerPos = iBuilder->CreateLoad(consumerPosPtr);

    // Ensure that the new consumer position is no less than the current position.
    newConsumerPos = iBuilder->CreateSelect(iBuilder->CreateICmpULT(newConsumerPos, consumerPos), consumerPos, newConsumerPos);
    Value * producerPos = iBuilder->CreateLoad(iBuilder->CreateGEP(bufferStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iProducer_pos)}));

    // Ensure that the new consumer position is no greater than the current producer position.
    Value * new_pos_lt_producer_pos = iBuilder->CreateICmpULT(newConsumerPos, producerPos);
    newConsumerPos = iBuilder->CreateSelect(new_pos_lt_producer_pos, newConsumerPos, producerPos);

    // Now, the new_consumer_pos is at most = to the producer_pos; if =, we're done.
    iBuilder->CreateCondBr(new_pos_lt_producer_pos, copyBackBody, setConsumerPosExit);
    iBuilder->SetInsertPoint(copyBackBody);
    
    Value * new_consumer_block = iBuilder->CreateUDiv(newConsumerPos, blockWidth);
    Value * lastProducerBlock = iBuilder->CreateUDiv(iBuilder->CreateSub(producerPos, one), blockWidth);
    Value * copyBlocks = iBuilder->CreateAdd(iBuilder->CreateSub(lastProducerBlock, new_consumer_block), one);

    DataLayout dl(iBuilder->getModule());

    Constant * blockBytes = ConstantInt::get(sizeTy, dl.getTypeAllocSize(mStreamSetType) * iBuilder->getStride());

    Value * copyLength = iBuilder->CreateMul(copyBlocks, blockBytes);

    // Must copy back one full block for each of the streams in the stream set.
    Value * handle = iBuilder->CreateGEP(bufferStructPtr, {iBuilder->getInt32(0), iBuilder->getInt32(iBuffer_ptr)});
    Value * bufferPtr = iBuilder->CreateLoad(handle);
    Value * const consumerBlock = iBuilder->CreateUDiv(consumerPos, blockWidth);
    Value * copyFrom = iBuilder->CreateGEP(bufferPtr, iBuilder->CreateSub(new_consumer_block, consumerBlock));
    unsigned alignment = iBuilder->getBitBlockWidth() / 8;
    iBuilder->CreateMemMove(iBuilder->CreateBitCast(bufferPtr, i8_ptr), iBuilder->CreateBitCast(copyFrom, i8_ptr), copyLength, alignment);
    iBuilder->CreateBr(setConsumerPosExit);
    // Copy back done, store the new consumer position.
    iBuilder->SetInsertPoint(setConsumerPosExit);

    iBuilder->CreateStore(newConsumerPos, consumerPosPtr);
}    
