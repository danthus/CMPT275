/*
 *  Copyright (c) 2015 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 */


#include "editdscan_kernel.h"
#include <llvm/IR/Intrinsics.h>
#include <IDISA/idisa_builder.h>
#include <llvm/Support/raw_os_ostream.h>
#include <iostream>

using namespace llvm;

namespace kernel {

Value * generateCountForwardZeroes(IDISA::IDISA_Builder * iBuilder, Value * bits) {
    Value * cttzFunc = Intrinsic::getDeclaration(iBuilder->getModule(), Intrinsic::cttz, bits->getType());
    return iBuilder->CreateCall(cttzFunc, std::vector<Value *>({bits, ConstantInt::get(iBuilder->getInt1Ty(), 0)}));
}

void editdScanKernel::generateDoBlockMethod() {
    IDISA::IDISA_Builder::InsertPoint savePoint = iBuilder->saveIP();
    Module * m = iBuilder->getModule();
    Function * scanWordFunction = generateScanWordRoutine(m);
    const unsigned fieldCount = iBuilder->getBitBlockWidth() / mScanwordBitWidth;
    Type * T = iBuilder->getIntNTy(mScanwordBitWidth);
    Type * scanwordVectorType =  VectorType::get(T, fieldCount);

    Function * doBlockFunction = m->getFunction(mKernelName + doBlock_suffix);

    iBuilder->SetInsertPoint(BasicBlock::Create(iBuilder->getContext(), "entry", doBlockFunction, 0));
    Value * kernelStuctParam = getParameter(doBlockFunction, "self");
    Value * blockNo = getScalarField(kernelStuctParam, blockNoScalar);
    Value * scanwordPos = iBuilder->CreateMul(blockNo, ConstantInt::get(blockNo->getType(), iBuilder->getBitBlockWidth()));
    
    Value * matchResultsPtr = getStreamSetBlockPtr(kernelStuctParam, "matchResults", blockNo);

    std::vector<Value * > matchWordVectors;
    for(int d = 0; d <= mEditDistance; d++){
        Value * matches = iBuilder->CreateBlockAlignedLoad(matchResultsPtr, {iBuilder->getInt32(0), iBuilder->getInt32(d)});
        matchWordVectors.push_back(iBuilder->CreateBitCast(matches, scanwordVectorType));
    }
    
    for(unsigned i = 0; i < fieldCount; ++i){       
        for(int d = 0; d <= mEditDistance; d++){
            Value * matchWord = iBuilder->CreateExtractElement(matchWordVectors[d], ConstantInt::get(T, i));
            iBuilder->CreateCall(scanWordFunction, {matchWord, iBuilder->getInt32(d), scanwordPos});
        }
        scanwordPos = iBuilder->CreateAdd(scanwordPos, ConstantInt::get(T, mScanwordBitWidth));

    }
    iBuilder -> CreateRetVoid();
    iBuilder->restoreIP(savePoint);
}

Function * editdScanKernel::generateScanWordRoutine(Module * m) {

    Type * T = iBuilder->getIntNTy(mScanwordBitWidth);
    LLVMContext & ctxt = m->getContext();

    Function * scanFunc = cast<Function>(m->getOrInsertFunction("scan_word", Type::getVoidTy(ctxt), T, iBuilder->getInt32Ty(), T, nullptr));
    scanFunc->setCallingConv(CallingConv::C);
    Function::arg_iterator args = scanFunc->arg_begin();

    Value * matchWord = &*(args++);
    matchWord->setName("matchWord");
    Value * dist = &*(args++);
    dist->setName("dist");
    Value * basePos = &*(args++);
    basePos->setName("basePos");

    Constant * matchProcessor = m->getOrInsertFunction("wrapped_report_pos", Type::getVoidTy(ctxt), T, iBuilder->getInt32Ty(), nullptr);

    BasicBlock * entryBlock = BasicBlock::Create(m->getContext(), "entry", scanFunc, 0);

    BasicBlock * matchesCondBlock = BasicBlock::Create(m->getContext(), "matchesCond", scanFunc, 0);
    BasicBlock * matchesLoopBlock = BasicBlock::Create(m->getContext(), "matchesLoop", scanFunc, 0);
    BasicBlock * matchesDoneBlock = BasicBlock::Create(m->getContext(), "matchesDone", scanFunc, 0);

    iBuilder->SetInsertPoint(entryBlock);
    iBuilder->CreateBr(matchesCondBlock);

    iBuilder->SetInsertPoint(matchesCondBlock);
    PHINode * matches_phi = iBuilder->CreatePHI(T, 2, "matches");
    matches_phi->addIncoming(matchWord, entryBlock);
    Value * have_matches_cond = iBuilder->CreateICmpUGT(matches_phi, ConstantInt::get(T, 0));
    iBuilder->CreateCondBr(have_matches_cond, matchesLoopBlock, matchesDoneBlock);

    iBuilder->SetInsertPoint(matchesLoopBlock);
    Value * match_pos = iBuilder->CreateAdd(generateCountForwardZeroes(iBuilder, matches_phi), basePos);
    Value * matches_new = iBuilder->CreateAnd(matches_phi, iBuilder->CreateSub(matches_phi, ConstantInt::get(T, 1)));
    matches_phi->addIncoming(matches_new, matchesLoopBlock);
    iBuilder->CreateCall(matchProcessor, std::vector<Value *>({match_pos, dist}));
    iBuilder->CreateBr(matchesCondBlock);

    iBuilder->SetInsertPoint(matchesDoneBlock);
    iBuilder -> CreateRetVoid();

    return scanFunc;

}

}