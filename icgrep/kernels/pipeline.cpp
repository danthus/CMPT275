/*
 *  Copyright (c) 2016 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 */

#include "pipeline.h"
#include "utf_encoding.h"

#include <kernels/scanmatchgen.h>
#include <kernels/s2p_kernel.h>
#include <kernels/instance.h>

#include <pablo/function.h>
#include <pablo/pablo_compiler.h>
#include <pablo/pablo_toolchain.h>

#include <llvm/Support/CommandLine.h>

static cl::opt<unsigned> SegmentSize("segment-size", cl::desc("Segment Size"), cl::value_desc("positive integer"), cl::init(1));

using namespace pablo;
using namespace kernel;

PipelineBuilder::PipelineBuilder(Module * m, IDISA::IDISA_Builder * b)
: mMod(m)
, iBuilder(b)
, mBitBlockType(b->getBitBlockType())
, mBlockSize(b->getBitBlockWidth()) {

}

PipelineBuilder::~PipelineBuilder() {
    delete mS2PKernel;
    delete mICgrepKernel;
    delete mScanMatchKernel;
}

void PipelineBuilder::CreateKernels(PabloFunction * function, bool isNameExpression){
    mS2PKernel = new KernelBuilder(iBuilder, "s2p", SegmentSize);
    mICgrepKernel = new KernelBuilder(iBuilder, "icgrep", SegmentSize);
    mScanMatchKernel = new KernelBuilder(iBuilder, "scanMatch", SegmentSize);
    generateS2PKernel(mMod, iBuilder, mS2PKernel);
    generateScanMatch(mMod, iBuilder, 64, mScanMatchKernel, isNameExpression);
    pablo_function_passes(function);
    PabloCompiler pablo_compiler(mMod, iBuilder);
    try {
        pablo_compiler.setKernel(mICgrepKernel);
        pablo_compiler.compile(function);
        delete function;
        releaseSlabAllocatorMemory();
    } catch (std::runtime_error e) {
        delete function;
        releaseSlabAllocatorMemory();
        std::cerr << "Runtime error: " << e.what() << std::endl;
        exit(1);
    }
}

Function * PipelineBuilder::ExecuteKernels() {
    Type * const int64ty = iBuilder->getInt64Ty();
    Type * const int8PtrTy = iBuilder->getInt8PtrTy();
    Type * const inputType = PointerType::get(ArrayType::get(StructType::get(mMod->getContext(), std::vector<Type *>({ArrayType::get(mBitBlockType, 8)})), 1), 0);

    Function * const main = cast<Function>(mMod->getOrInsertFunction("Main", Type::getVoidTy(mMod->getContext()), inputType, int64ty, int8PtrTy, iBuilder->getInt1Ty(), nullptr));
    main->setCallingConv(CallingConv::C);
    Function::arg_iterator args = main->arg_begin();

    Value * const inputStream = args++;
    inputStream->setName("input");
    Value * const bufferSize = args++;
    bufferSize->setName("bufferSize");
    Value * const fileName = args++;
    fileName->setName("fileName");
    Value * const finalLineUnterminated = args++;
    finalLineUnterminated->setName("finalLineUnterminated");

    iBuilder->SetInsertPoint(BasicBlock::Create(mMod->getContext(), "entry", main,0));


    BasicBlock * entryBlock = iBuilder->GetInsertBlock();
    BasicBlock * segmentCondBlock = nullptr;
    BasicBlock * segmentBodyBlock = nullptr;
    const unsigned segmentSize = SegmentSize;
    if (segmentSize > 1) {
        segmentCondBlock = BasicBlock::Create(mMod->getContext(), "segmentCond", main, 0);
        segmentBodyBlock = BasicBlock::Create(mMod->getContext(), "segmentBody", main, 0);
    }
    BasicBlock * fullCondBlock = BasicBlock::Create(mMod->getContext(), "fullCond", main, 0);
    BasicBlock * fullBodyBlock = BasicBlock::Create(mMod->getContext(), "fullBody", main, 0);
    BasicBlock * finalBlock = BasicBlock::Create(mMod->getContext(), "final", main, 0);
    BasicBlock * finalPartialBlock = BasicBlock::Create(mMod->getContext(), "partial", main, 0);
    BasicBlock * finalEmptyBlock = BasicBlock::Create(mMod->getContext(), "empty", main, 0);
    BasicBlock * endBlock = BasicBlock::Create(mMod->getContext(), "end", main, 0);
    BasicBlock * unterminatedBlock = BasicBlock::Create(mMod->getContext(), "unterminated", main, 0);
    BasicBlock * exitBlock = BasicBlock::Create(mMod->getContext(), "exit", main, 0);

    Instance * s2pInstance = mS2PKernel->instantiate(inputStream);
    Instance * icGrepInstance = mICgrepKernel->instantiate(s2pInstance->getOutputStreamBuffer());
    Instance * scanMatchInstance = mScanMatchKernel->instantiate(icGrepInstance->getOutputStreamBuffer());

    scanMatchInstance->setInternalState("FileBuf", iBuilder->CreateBitCast(inputStream, int8PtrTy));
    scanMatchInstance->setInternalState("FileSize", bufferSize);
    scanMatchInstance->setInternalState("FileName", fileName);

    Value * initialBufferSize = nullptr;
    BasicBlock * initialBlock = nullptr;

    if (segmentSize > 1) {
        iBuilder->CreateBr(segmentCondBlock);
        iBuilder->SetInsertPoint(segmentCondBlock);
        PHINode * remainingBytes = iBuilder->CreatePHI(int64ty, 2, "remainingBytes");
        remainingBytes->addIncoming(bufferSize, entryBlock);
        Constant * const step = ConstantInt::get(int64ty, mBlockSize * segmentSize);
        Value * segmentCondTest = iBuilder->CreateICmpULT(remainingBytes, step);
        iBuilder->CreateCondBr(segmentCondTest, fullCondBlock, segmentBodyBlock);
        iBuilder->SetInsertPoint(segmentBodyBlock);
        for (unsigned i = 0; i < segmentSize; ++i) {
            s2pInstance->CreateDoBlockCall();
        }
        for (unsigned i = 0; i < segmentSize; ++i) {
            icGrepInstance->CreateDoBlockCall();
        }
        for (unsigned i = 0; i < segmentSize; ++i) {
            scanMatchInstance->CreateDoBlockCall();
        }
        remainingBytes->addIncoming(iBuilder->CreateSub(remainingBytes, step), segmentBodyBlock);
        iBuilder->CreateBr(segmentCondBlock);
        initialBufferSize = remainingBytes;
        initialBlock = segmentCondBlock;
    } else {
        initialBufferSize = bufferSize;
        initialBlock = entryBlock;
        iBuilder->CreateBr(fullCondBlock);
    }

    iBuilder->SetInsertPoint(fullCondBlock);
    PHINode * remainingBytes = iBuilder->CreatePHI(int64ty, 2, "remainingBytes");
    remainingBytes->addIncoming(initialBufferSize, initialBlock);

    Constant * const step = ConstantInt::get(int64ty, mBlockSize);
    Value * fullCondTest = iBuilder->CreateICmpULT(remainingBytes, step);
    iBuilder->CreateCondBr(fullCondTest, finalBlock, fullBodyBlock);

    iBuilder->SetInsertPoint(fullBodyBlock);
    s2pInstance->CreateDoBlockCall();
    icGrepInstance->CreateDoBlockCall();
    scanMatchInstance->CreateDoBlockCall();

    remainingBytes->addIncoming(iBuilder->CreateSub(remainingBytes, step), fullBodyBlock);
    iBuilder->CreateBr(fullCondBlock);

    iBuilder->SetInsertPoint(finalBlock);
    Value * const b4 = s2pInstance->getOutputStream(4);
    Value * const b6 = s2pInstance->getOutputStream(6);
    Value * emptyBlockCond = iBuilder->CreateICmpEQ(remainingBytes, ConstantInt::get(int64ty, 0));
    iBuilder->CreateCondBr(emptyBlockCond, finalEmptyBlock, finalPartialBlock);


    iBuilder->SetInsertPoint(finalPartialBlock);
    s2pInstance->CreateDoBlockCall();
    iBuilder->CreateBr(endBlock);

    iBuilder->SetInsertPoint(finalEmptyBlock);
    s2pInstance->clearOutputStreamSet();
    iBuilder->CreateBr(endBlock);

    iBuilder->SetInsertPoint(endBlock);
    Value * isFinalLineUnterminated = iBuilder->CreateICmpEQ(finalLineUnterminated, ConstantInt::getNullValue(finalLineUnterminated->getType()));
    iBuilder->CreateCondBr(isFinalLineUnterminated, exitBlock, unterminatedBlock);
    
    iBuilder->SetInsertPoint(unterminatedBlock);

    Value * remaining = iBuilder->CreateZExt(remainingBytes, iBuilder->getIntNTy(mBlockSize));
    Value * EOF_pos = iBuilder->CreateShl(ConstantInt::get(iBuilder->getIntNTy(mBlockSize), 1), remaining);
    EOF_pos = iBuilder->CreateBitCast(EOF_pos, mBitBlockType);


    Value * b4val = iBuilder->CreateBlockAlignedLoad(b4);
    b4val = iBuilder->CreateOr(b4val, EOF_pos);
    iBuilder->CreateBlockAlignedStore(b4val, b4);

    Value * b6val = iBuilder->CreateBlockAlignedLoad(b6);
    b6val = iBuilder->CreateOr(b6val, EOF_pos);
    iBuilder->CreateBlockAlignedStore(b6val, b6);

    iBuilder->CreateBr(exitBlock);

    iBuilder->SetInsertPoint(exitBlock);

    icGrepInstance->CreateDoBlockCall();
    scanMatchInstance->CreateDoBlockCall();
    iBuilder->CreateRetVoid();

    return main;
}
