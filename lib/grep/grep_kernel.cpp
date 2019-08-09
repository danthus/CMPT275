/*
 *  Copyright (c) 2018 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 */

#include <grep/grep_kernel.h>

#include <grep/grep_engine.h>
#include <kernel/core/kernel_builder.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <pablo/codegenstate.h>
#include <toolchain/pablo_toolchain.h>
#include <pablo/builder.hpp>
#include <pablo/pe_ones.h>          // for Ones
#include <pablo/pe_var.h>           // for Var
#include <pablo/pe_zeroes.h>        // for Zeroes
#include <pablo/pe_infile.h>
#include <pablo/boolean.h>
#include <pablo/pe_count.h>
#include <pablo/pe_matchstar.h>
#include <pablo/pe_pack.h>
#include <re/adt/printer_re.h>
#include <re/adt/re_cc.h>
#include <re/adt/re_name.h>
#include <re/alphabet/alphabet.h>
#include <re/toolchain/toolchain.h>
#include <re/compile/re_reverse.h>
#include <re/cc/cc_compiler.h>         // for CC_Compiler
#include <re/cc/cc_compiler_target.h>
#include <re/cc/multiplex_CCs.h>
#include <re/compile/re_compiler.h>

using namespace kernel;
using namespace pablo;
using namespace re;
using namespace llvm;

void UTF8_index::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    std::unique_ptr<cc::CC_Compiler> ccc;
    bool useDirectCC = getInput(0)->getType()->getArrayNumElements() == 1;
    if (useDirectCC) {
        ccc = make_unique<cc::Direct_CC_Compiler>(getEntryScope(), pb.createExtract(getInput(0), pb.getInteger(0)));
    } else {
        ccc = make_unique<cc::Parabix_CC_Compiler_Builder>(getEntryScope(), getInputStreamSet("source"));
    }

    Zeroes * const ZEROES = pb.createZeroes();
    PabloAST * const u8pfx = ccc->compileCC(makeByte(0xC0, 0xFF));


    Var * const nonFinal = pb.createVar("nonFinal", u8pfx);
    Var * const u8invalid = pb.createVar("u8invalid", ZEROES);
    Var * const valid_pfx = pb.createVar("valid_pfx", u8pfx);

    auto it = pb.createScope();
    pb.createIf(u8pfx, it);
    PabloAST * const u8pfx2 = ccc->compileCC(makeByte(0xC2, 0xDF), it);
    PabloAST * const u8pfx3 = ccc->compileCC(makeByte(0xE0, 0xEF), it);
    PabloAST * const u8pfx4 = ccc->compileCC(makeByte(0xF0, 0xF4), it);
    PabloAST * const u8suffix = ccc->compileCC("u8suffix", makeByte(0x80, 0xBF), it);

    //
    // Two-byte sequences
    Var * const anyscope = it.createVar("anyscope", ZEROES);
    auto it2 = it.createScope();
    it.createIf(u8pfx2, it2);
    it2.createAssign(anyscope, it2.createAdvance(u8pfx2, 1));


    //
    // Three-byte sequences
    Var * const EF_invalid = it.createVar("EF_invalid", ZEROES);
    auto it3 = it.createScope();
    it.createIf(u8pfx3, it3);
    PabloAST * const u8scope32 = it3.createAdvance(u8pfx3, 1);
    it3.createAssign(nonFinal, it3.createOr(nonFinal, u8scope32));
    PabloAST * const u8scope33 = it3.createAdvance(u8pfx3, 2);
    PabloAST * const u8scope3X = it3.createOr(u8scope32, u8scope33);
    it3.createAssign(anyscope, it3.createOr(anyscope, u8scope3X));
    PabloAST * const E0_invalid = it3.createAnd(it3.createAdvance(ccc->compileCC(makeByte(0xE0), it3), 1), ccc->compileCC(makeByte(0x80, 0x9F), it3));
    PabloAST * const ED_invalid = it3.createAnd(it3.createAdvance(ccc->compileCC(makeByte(0xED), it3), 1), ccc->compileCC(makeByte(0xA0, 0xBF), it3));
    PabloAST * const EX_invalid = it3.createOr(E0_invalid, ED_invalid);
    it3.createAssign(EF_invalid, EX_invalid);

    //
    // Four-byte sequences
    auto it4 = it.createScope();
    it.createIf(u8pfx4, it4);
    PabloAST * const u8scope42 = it4.createAdvance(u8pfx4, 1, "u8scope42");
    PabloAST * const u8scope43 = it4.createAdvance(u8scope42, 1, "u8scope43");
    PabloAST * const u8scope44 = it4.createAdvance(u8scope43, 1, "u8scope44");
    PabloAST * const u8scope4nonfinal = it4.createOr(u8scope42, u8scope43);
    it4.createAssign(nonFinal, it4.createOr(nonFinal, u8scope4nonfinal));
    PabloAST * const u8scope4X = it4.createOr(u8scope4nonfinal, u8scope44);
    it4.createAssign(anyscope, it4.createOr(anyscope, u8scope4X));
    PabloAST * const F0_invalid = it4.createAnd(it4.createAdvance(ccc->compileCC(makeByte(0xF0), it4), 1), ccc->compileCC(makeByte(0x80, 0x8F), it4));
    PabloAST * const F4_invalid = it4.createAnd(it4.createAdvance(ccc->compileCC(makeByte(0xF4), it4), 1), ccc->compileCC(makeByte(0x90, 0xBF), it4));
    PabloAST * const FX_invalid = it4.createOr(F0_invalid, F4_invalid);
    it4.createAssign(EF_invalid, it4.createOr(EF_invalid, FX_invalid));

    //
    // Invalid cases
    PabloAST * const legalpfx = it.createOr(it.createOr(u8pfx2, u8pfx3), u8pfx4);
    //  Any scope that does not have a suffix byte, and any suffix byte that is not in
    //  a scope is a mismatch, i.e., invalid UTF-8.
    PabloAST * const mismatch = it.createXor(anyscope, u8suffix);
    //
    PabloAST * const pfx_invalid = it.createXor(valid_pfx, legalpfx);
    it.createAssign(u8invalid, it.createOr(pfx_invalid, it.createOr(mismatch, EF_invalid)));
    PabloAST * const u8valid = it.createNot(u8invalid, "u8valid");
    //
    //
    it.createAssign(nonFinal, it.createAnd(nonFinal, u8valid));
    //pb.createAssign(nonFinal, pb.createOr(nonFinal, CRLF));
    //PabloAST * unterminatedLineAtEOF = pb.createAtEOF(pb.createAdvance(pb.createNot(LineBreak), 1), "unterminatedLineAtEOF");

    Var * const u8index = getOutputStreamVar("u8index");
    PabloAST * u8final = pb.createInFile(pb.createNot(nonFinal));
    pb.createAssign(pb.createExtract(u8index, pb.getInteger(0)), u8final);
}

UTF8_index::UTF8_index(const std::unique_ptr<kernel::KernelBuilder> & kb, StreamSet * Source, StreamSet * u8index)
: PabloKernel(kb, "UTF8_index_" + std::to_string(Source->getNumElements()) + "x" + std::to_string(Source->getFieldWidth()),
// input
{Binding{"source", Source}},
// output
{Binding{"u8index", u8index}}) {

}

void GrepKernelOptions::setIndexingAlphabet(const cc::Alphabet * a) {mIndexingAlphabet = a;}
void GrepKernelOptions::setRE(RE * e) {mRE = e;}
void GrepKernelOptions::setPrefixRE(RE * e) {mPrefixRE = e;}
void GrepKernelOptions::setSource(StreamSet * s) {mSource = s;}
void GrepKernelOptions::setCombiningStream(GrepCombiningType t, StreamSet * toCombine){
    mCombiningType = t;
    mCombiningStream = toCombine;
}
void GrepKernelOptions::setResults(StreamSet * r) {mResults = r;}

void GrepKernelOptions::addAlphabet(std::shared_ptr<cc::Alphabet> a, StreamSet * basis) {
    mAlphabets.emplace_back(a, basis);
}

Bindings GrepKernelOptions::streamSetInputBindings() {
    Bindings inputs;
    if (mExternals.empty()) {
        inputs.emplace_back("basis", mSource);
    } else {
        inputs.emplace_back("basis", mSource, FixedRate());
    }
    if (mCombiningType != GrepCombiningType::None) {
        inputs.emplace_back("toCombine", mCombiningStream);
    }
    for (const auto & a : mExternals) {
        inputs.emplace_back(a);
    }
    for (const auto & a : mAlphabets) {
        inputs.emplace_back(a.first->getName() + "_basis", a.second);
    }
    return inputs;
}

Bindings GrepKernelOptions::streamSetOutputBindings() {
    return {Binding{"matches", mResults, FixedRate(), Add1()}};
}

Bindings GrepKernelOptions::scalarInputBindings() {
    return {};
}

Bindings GrepKernelOptions::scalarOutputBindings() {
    return {};
}

std::string GrepKernelOptions::getSignature() {
    if (mSignature == "") {
        mSignature = std::to_string(mSource->getNumElements()) + "x" + std::to_string(mSource->getFieldWidth());
        if (mSource->getFieldWidth() == 8) {
            mSignature += ":" + std::to_string(grep::ByteCClimit);
        }
        mSignature += "/" + mIndexingAlphabet->getName();
        for (const auto & e : mExternals) {
            mSignature += "_" + e.getName();
        }
        for (const auto & a: mAlphabets) {
            mSignature += "_" + a.first->getName();
        }
        if (mCombiningType == GrepCombiningType::Exclude) {
            mSignature += "&~";
        } else if (mCombiningType == GrepCombiningType::Include) {
            mSignature += "|=";
        }
        if (mPrefixRE) {
            mSignature += ":" + Printer_RE::PrintRE(mPrefixRE);
        }
        mSignature += ":" + Printer_RE::PrintRE(mRE);
    }
    return mSignature;
}

ICGrepKernel::ICGrepKernel(const std::unique_ptr<kernel::KernelBuilder> & b, std::unique_ptr<GrepKernelOptions> options)
: PabloKernel(b, AnnotateWithREflags("ic") + getStringHash(options->getSignature()),
options->streamSetInputBindings(),
options->streamSetOutputBindings(),
options->scalarInputBindings(),
options->scalarOutputBindings()),
mOptions(std::move(options)) {
    addAttribute(InfrequentlyUsed());
}

std::string ICGrepKernel::makeSignature(const std::unique_ptr<kernel::KernelBuilder> &) const {
    return mOptions->getSignature();
}

void ICGrepKernel::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    std::unique_ptr<cc::CC_Compiler> ccc;
    bool useDirectCC = getInput(0)->getType()->getArrayNumElements() == 1;
    if (useDirectCC) {
        ccc = make_unique<cc::Direct_CC_Compiler>(getEntryScope(), pb.createExtract(getInput(0), pb.getInteger(0)));
    } else {
        ccc = make_unique<cc::Parabix_CC_Compiler_Builder>(getEntryScope(), getInputStreamSet("basis"));
    }
    //cc::Parabix_CC_Compiler ccc(getEntryScope(), getInputStreamSet("basis"), mOptions->mBasisSetNumbering);
    RE_Compiler re_compiler(getEntryScope(), *ccc.get(), *(mOptions->mIndexingAlphabet));
    for (const auto & e : mOptions->mExternals) {
        re_compiler.addPrecompiled(e.getName(), pb.createExtract(getInputStreamVar(e.getName()), pb.getInteger(0)));
    }
    for (const auto & a : mOptions->mAlphabets) {
        auto & alpha = a.first;
        auto mpx_basis = getInputStreamSet(alpha->getName() + "_basis");
        re_compiler.addAlphabet(alpha, mpx_basis);
    }
    Var * const final_matches = pb.createVar("final_matches", pb.createZeroes());
    if (mOptions->mPrefixRE) {
        PabloAST * const prefixMatches = re_compiler.compile(mOptions->mPrefixRE);
        PabloBlock * scope1 = getEntryScope()->createScope();
        pb.createIf(prefixMatches, scope1);

        PabloAST * u8bytes = pb.createExtract(getInput(0), pb.getInteger(0));
        PabloAST * nybbles[2];
        nybbles[0] = scope1->createPackL(scope1->getInteger(8), u8bytes);
        nybbles[1] = scope1->createPackH(scope1->getInteger(8), u8bytes);

        PabloAST * bitpairs[4];
        for (unsigned i = 0; i < 2; i++) {
            bitpairs[2*i] = scope1->createPackL(scope1->getInteger(4), nybbles[i]);
            bitpairs[2*i + 1] = scope1->createPackH(scope1->getInteger(4), nybbles[i]);
        }

        std::vector<PabloAST *> basis(8);
        for (unsigned i = 0; i < 4; i++) {
            basis[2*i] = scope1->createPackL(scope1->getInteger(2), bitpairs[i]);
            basis[2*i + 1] = scope1->createPackH(scope1->getInteger(2), bitpairs[i]);
        }

        cc::Parabix_CC_Compiler_Builder ccc(scope1, basis);
        RE_Compiler re_compiler(scope1, ccc, *(mOptions->mIndexingAlphabet));
        scope1->createAssign(final_matches, re_compiler.compile(mOptions->mRE, prefixMatches));
    } else {
        pb.createAssign(final_matches, re_compiler.compile(mOptions->mRE));
    }
    Var * const output = getOutputStreamVar("matches");
    if (mOptions->mCombiningType == GrepCombiningType::None) {
        pb.createAssign(pb.createExtract(output, pb.getInteger(0)),final_matches);
    } else {
        PabloAST * toCombine = pb.createExtract(getInputStreamVar("toCombine"), pb.getInteger(0));
        if (mOptions->mCombiningType == GrepCombiningType::Exclude) {
            pb.createAssign(pb.createExtract(output, pb.getInteger(0)), pb.createAnd(toCombine, pb.createNot(final_matches)));
        } else {
            pb.createAssign(pb.createExtract(output, pb.getInteger(0)), pb.createOr(toCombine, final_matches));
        }
    }
}

// Helper to compute stream set inputs to pass into PabloKernel constructor.
Bindings ByteBitGrepKernel::makeInputBindings(StreamSet * const basis, const Externals & externals) {
    Bindings inputs;
    inputs.emplace_back("basis", basis);
    for (const auto & e : externals) {
        inputs.emplace_back(e.first, e.second);
    }
    return inputs;
}


ByteBitGrepSignature::ByteBitGrepSignature(RE * prefix, RE * suffix)
: mPrefixRE(prefix)
, mSuffixRE(suffix)
, mSignature(Printer_RE::PrintRE(mPrefixRE) + Printer_RE::PrintRE(mSuffixRE) ) {
}

ByteBitGrepKernel::ByteBitGrepKernel(const std::unique_ptr<kernel::KernelBuilder> & b, RE * const prefixRE, RE * const suffixRE, StreamSet * const Source, StreamSet * const matches, const Externals externals)
: ByteBitGrepSignature(prefixRE, suffixRE)
, PabloKernel(b, AnnotateWithREflags("bBc") + getStringHash(mSignature),
// inputs
makeInputBindings(Source, externals),
// output
{Binding{"matches", matches, FixedRate(), Add1()}}) {
    addAttribute(InfrequentlyUsed());
}

std::string ByteBitGrepKernel::makeSignature(const std::unique_ptr<kernel::KernelBuilder> &) const {
    return mSignature;
}


void ByteBitGrepKernel::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    PabloAST * u8bytes = pb.createExtract(getInput(0), pb.getInteger(0));
    cc::Direct_CC_Compiler dcc(getEntryScope(), u8bytes);
    RE_Compiler re_byte_compiler(getEntryScope(), dcc);

    const auto numOfInputs = getNumOfInputs();
    for (unsigned i = 1; i < numOfInputs; ++i) {
        const Binding & input = getInputStreamSetBinding(i);
        re_byte_compiler.addPrecompiled(input.getName(), pb.createExtract(getInputStreamVar(input.getName()), pb.getInteger(0)));
    }

    PabloAST * const prefixMatches = re_byte_compiler.compile(mPrefixRE);
    Var * const final_matches = pb.createVar("final_matches", pb.createZeroes());
    PabloBlock * scope1 = getEntryScope()->createScope();
    pb.createIf(prefixMatches, scope1);

    PabloAST * nybbles[2];
    nybbles[0] = scope1->createPackL(scope1->getInteger(8), u8bytes);
    nybbles[1] = scope1->createPackH(scope1->getInteger(8), u8bytes);

    PabloAST * bitpairs[4];
    for (unsigned i = 0; i < 2; i++) {
        bitpairs[2*i] = scope1->createPackL(scope1->getInteger(4), nybbles[i]);
        bitpairs[2*i + 1] = scope1->createPackH(scope1->getInteger(4), nybbles[i]);
    }

    std::vector<PabloAST *> basis(8);
    for (unsigned i = 0; i < 4; i++) {
        basis[2*i] = scope1->createPackL(scope1->getInteger(2), bitpairs[i]);
        basis[2*i + 1] = scope1->createPackH(scope1->getInteger(2), bitpairs[i]);
    }

    cc::Parabix_CC_Compiler_Builder ccc(scope1, basis);
    RE_Compiler re_compiler(scope1, ccc);
    scope1->createAssign(final_matches, re_compiler.compile(mSuffixRE, prefixMatches));
    Var * const output = getOutputStreamVar("matches");
    pb.createAssign(pb.createExtract(output, pb.getInteger(0)), final_matches);
}


void MatchedLinesKernel::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    PabloAST * matchResults = pb.createExtract(getInputStreamVar("matchResults"), pb.getInteger(0));
    PabloAST * lineBreaks = pb.createExtract(getInputStreamVar("lineBreaks"), pb.getInteger(0));
    PabloAST * notLB = pb.createNot(lineBreaks);
    PabloAST * match_follow = pb.createMatchStar(matchResults, notLB);
    Var * const matchedLines = getOutputStreamVar("matchedLines");
    pb.createAssign(pb.createExtract(matchedLines, pb.getInteger(0)), pb.createAnd(match_follow, lineBreaks, "matchedLines"));
}

MatchedLinesKernel::MatchedLinesKernel (const std::unique_ptr<kernel::KernelBuilder> & iBuilder, StreamSet * OriginalMatches, StreamSet * LineBreakStream, StreamSet * Matches)
: PabloKernel(iBuilder, "MatchedLines",
// inputs
{Binding{"matchResults", OriginalMatches}
,Binding{"lineBreaks", LineBreakStream, FixedRate()}},
// output
{Binding{"matchedLines", Matches}}) {

}


void InvertMatchesKernel::generateDoBlockMethod(const std::unique_ptr<KernelBuilder> & iBuilder) {
    Value * input = iBuilder->loadInputStreamBlock("matchedLines", iBuilder->getInt32(0));
    Value * lbs = iBuilder->loadInputStreamBlock("lineBreaks", iBuilder->getInt32(0));
    Value * inverted = iBuilder->CreateAnd(iBuilder->CreateNot(input), lbs, "inverted");
    iBuilder->storeOutputStreamBlock("nonMatches", iBuilder->getInt32(0), inverted);
}

InvertMatchesKernel::InvertMatchesKernel(const std::unique_ptr<kernel::KernelBuilder> & b, StreamSet * OriginalMatches, StreamSet * LineBreakStream, StreamSet * Matches)
: BlockOrientedKernel(b, "Invert",
// Inputs
{Binding{"matchedLines", OriginalMatches},
 Binding{"lineBreaks", LineBreakStream}},
// Outputs
{Binding{"nonMatches", Matches}},
// Input/Output Scalars and internal state
{}, {}, {}) {

}


void PopcountKernel::generatePabloMethod() {
    auto pb = getEntryScope();
    const auto toCount = pb->createExtract(getInputStreamVar("toCount"), pb->getInteger(0));
    pablo::Var * countResult = getOutputScalarVar("countResult");

    pb->createAssign(countResult, pb->createCount(pb->createInFile(toCount)));
}

PopcountKernel::PopcountKernel (const std::unique_ptr<kernel::KernelBuilder> & iBuilder, StreamSet * const toCount, Scalar * countResult)
: PabloKernel(iBuilder, "Popcount",
{Binding{"toCount", toCount}},
{},
{},
{Binding{"countResult", countResult}}) {

}


void AbortOnNull::generateMultiBlockLogic(const std::unique_ptr<KernelBuilder> & b, llvm::Value * const numOfStrides) {
    Module * const m = b->getModule();
    DataLayout DL(m);
    IntegerType * const intPtrTy = DL.getIntPtrType(m->getContext());
    Type * voidPtrTy = b->getVoidPtrTy();
    const auto blocksPerStride = getStride() / b->getBitBlockWidth();
    Constant * const BLOCKS_PER_STRIDE = b->getSize(blocksPerStride);
    BasicBlock * const entry = b->GetInsertBlock();
    BasicBlock * const strideLoop = b->CreateBasicBlock("strideLoop");
    BasicBlock * const stridesDone = b->CreateBasicBlock("stridesDone");
    BasicBlock * const nullByteDetection = b->CreateBasicBlock("nullByteDetection");
    BasicBlock * const nullByteFound = b->CreateBasicBlock("nullByteFound");
    BasicBlock * const finalStride = b->CreateBasicBlock("finalStride");
    BasicBlock * const segmentDone = b->CreateBasicBlock("segmentDone");

    Value * const numOfBlocks = b->CreateMul(numOfStrides, BLOCKS_PER_STRIDE);
    Value * itemsToDo = b->getAccessibleItemCount("byteData");
    //
    // Fast loop to prove that there are no null bytes in a multiblock region.
    // We repeatedly combine byte packs using a SIMD unsigned min operation
    // (implemented as a Select/ICmpULT combination).
    //
    Value * byteStreamBasePtr = b->getInputStreamBlockPtr("byteData", b->getSize(0), b->getSize(0));
    Value * outputStreamBasePtr = b->getOutputStreamBlockPtr("untilNull", b->getSize(0), b->getSize(0));

    //
    // We set up a a set of eight accumulators to accumulate the minimum byte
    // values seen at each position in a block.   The initial min value at
    // each position is 0xFF (all ones).
    Value * blockMin[8];
    for (unsigned i = 0; i < 8; i++) {
        blockMin[i] = b->fwCast(8, b->allOnes());
    }
    // If we're in the final block bypass the fast loop.
    b->CreateCondBr(mIsFinal, finalStride, strideLoop);

    b->SetInsertPoint(strideLoop);
    PHINode * const baseBlockIndex = b->CreatePHI(b->getSizeTy(), 2);
    baseBlockIndex->addIncoming(ConstantInt::get(baseBlockIndex->getType(), 0), entry);
    PHINode * const blocksRemaining = b->CreatePHI(b->getSizeTy(), 2);
    blocksRemaining->addIncoming(numOfBlocks, entry);
    for (unsigned i = 0; i < 8; i++) {
        Value * next = b->CreateBlockAlignedLoad(b->CreateGEP(byteStreamBasePtr, {baseBlockIndex, b->getSize(i)}));
        b->CreateBlockAlignedStore(next, b->CreateGEP(outputStreamBasePtr, {baseBlockIndex, b->getSize(i)}));
        next = b->fwCast(8, next);
        blockMin[i] = b->CreateSelect(b->CreateICmpULT(next, blockMin[i]), next, blockMin[i]);
    }
    Value * nextBlockIndex = b->CreateAdd(baseBlockIndex, ConstantInt::get(baseBlockIndex->getType(), 1));
    Value * nextRemaining = b->CreateSub(blocksRemaining, ConstantInt::get(blocksRemaining->getType(), 1));
    baseBlockIndex->addIncoming(nextBlockIndex, strideLoop);
    blocksRemaining->addIncoming(nextRemaining, strideLoop);
    b->CreateCondBr(b->CreateICmpUGT(nextRemaining, ConstantInt::getNullValue(blocksRemaining->getType())), strideLoop, stridesDone);

    b->SetInsertPoint(stridesDone);
    // Combine the 8 blockMin values.
    for (unsigned i = 0; i < 4; i++) {
        blockMin[i] = b->CreateSelect(b->CreateICmpULT(blockMin[i], blockMin[i+4]), blockMin[i], blockMin[i+4]);
    }
    for (unsigned i = 0; i < 2; i++) {
        blockMin[i] = b->CreateSelect(b->CreateICmpULT(blockMin[i], blockMin[i+4]), blockMin[i], blockMin[i+2]);
    }
    blockMin[0] = b->CreateSelect(b->CreateICmpULT(blockMin[0], blockMin[1]), blockMin[0], blockMin[1]);
    Value * anyNull = b->bitblock_any(b->simd_eq(8, blockMin[0], b->allZeroes()));

    b->CreateCondBr(anyNull, nullByteDetection, segmentDone);


    b->SetInsertPoint(finalStride);
    b->CreateMemCpy(b->CreatePointerCast(outputStreamBasePtr, voidPtrTy), b->CreatePointerCast(byteStreamBasePtr, voidPtrTy), itemsToDo, 1);
    b->CreateBr(nullByteDetection);

    b->SetInsertPoint(nullByteDetection);
    //  Find the exact location using memchr, which should be fast enough.
    //
    Value * ptrToNull = b->CreateMemChr(b->CreatePointerCast(byteStreamBasePtr, voidPtrTy), b->getInt32(0), itemsToDo);
    Value * ptrAddr = b->CreatePtrToInt(ptrToNull, intPtrTy);
    b->CreateCondBr(b->CreateICmpEQ(ptrAddr, ConstantInt::getNullValue(intPtrTy)), segmentDone, nullByteFound);

    // A null byte has been located; set the termination code and call the signal handler.
    b->SetInsertPoint(nullByteFound);
    Value * nullPosn = b->CreateSub(b->CreatePtrToInt(ptrToNull, intPtrTy), b->CreatePtrToInt(byteStreamBasePtr, intPtrTy));
    b->setFatalTerminationSignal();
    Function * const dispatcher = m->getFunction("signal_dispatcher"); assert (dispatcher);
    Value * handler = b->getScalarField("handler_address");
    b->CreateCall(dispatcher, {handler, ConstantInt::get(b->getInt32Ty(), static_cast<unsigned>(grep::GrepSignal::BinaryFile))});
    b->CreateBr(segmentDone);

    b->SetInsertPoint(segmentDone);
    PHINode * const produced = b->CreatePHI(b->getSizeTy(), 3);
    produced->addIncoming(nullPosn, nullByteFound);
    produced->addIncoming(itemsToDo, stridesDone);
    produced->addIncoming(itemsToDo, nullByteDetection);
    Value * producedCount = b->getProducedItemCount("untilNull");
    producedCount = b->CreateAdd(producedCount, produced);
    b->setProducedItemCount("untilNull", producedCount);
}

AbortOnNull::AbortOnNull(const std::unique_ptr<kernel::KernelBuilder> & b, StreamSet * const InputStream, StreamSet * const OutputStream, Scalar * callbackObject)
: MultiBlockKernel(b, "AbortOnNull",
// inputs
{Binding{"byteData", InputStream, FixedRate(), Principal()}},
// outputs
{Binding{ "untilNull", OutputStream, FixedRate(), Deferred()}},
// input scalars
{Binding{"handler_address", callbackObject}},
{}, {}) {
    addAttribute(CanTerminateEarly());
    addAttribute(MayFatallyTerminate());
}

ContextSpan::ContextSpan(const std::unique_ptr<kernel::KernelBuilder> & b, StreamSet * const markerStream, StreamSet * const contextStream, unsigned before, unsigned after)
: PabloKernel(b, "ContextSpan-" + std::to_string(before) + "+" + std::to_string(after),
              // input
{Binding{"markerStream", markerStream, FixedRate(1), LookAhead(before)}},
              // output
{Binding{"contextStream", contextStream}}),
mBeforeContext(before), mAfterContext(after) {
}

void ContextSpan::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    Var * markerStream = pb.createExtract(getInputStreamVar("markerStream"), pb.getInteger(0));
    PabloAST * contextStart = pb.createLookahead(markerStream, pb.getInteger(mBeforeContext));
    unsigned lgth = mBeforeContext + 1 + mAfterContext;
    PabloAST * consecutive = contextStart;
    unsigned consecutiveCount = 1;
    for (unsigned i = 1; i <= lgth/2; i *= 2) {
        consecutiveCount += i;
        consecutive = pb.createOr(consecutive,
                                  pb.createAdvance(consecutive, i),
                                  "consecutive" + std::to_string(consecutiveCount));
    }
    if (consecutiveCount < lgth) {
        consecutive = pb.createOr(consecutive,
                                  pb.createAdvance(consecutive, lgth - consecutiveCount),
                                  "consecutive" + std::to_string(lgth));
    }
    pb.createAssign(pb.createExtract(getOutputStreamVar("contextStream"), pb.getInteger(0)), pb.createInFile(consecutive));
}