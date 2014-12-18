/*
 *  Copyright (c) 2014 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

/*
 *  Copyright (c) 2014 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#include <pablo/pablo_compiler.h>
#include <pablo/codegenstate.h>
#include <pablo/printer_pablos.h>
#include <cc/cc_namemap.hpp>
#include <re/re_name.h>
#include <stdexcept>
#include <include/simd-lib/bitblock.hpp>

#ifdef USE_LLVM_3_4
#include <llvm/Analysis/Verifier.h>
#include <llvm/Assembly/PrintModulePass.h>
#include <llvm/Linker.h>
#endif
#ifdef USE_LLVM_3_5
#include <llvm/IR/Verifier.h>
#endif

#include <llvm/Pass.h>
#include <llvm/PassManager.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/Passes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/Support/MathExtras.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Compiler.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Host.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/IR/IRBuilder.h>

//#define DUMP_GENERATED_IR

extern "C" {
  void wrapped_print_register(BitBlock bit_block) {
      print_register<BitBlock>("", bit_block);
  }
}

#define CREATE_GENERAL_CODE_CATEGORY(SUFFIX) \
SUFFIX * f##SUFFIX = nullptr; \
extern "C" { \
    BitBlock __get_category_##SUFFIX(Basis_bits &basis_bits) { \
        if (f##SUFFIX == nullptr) f##SUFFIX = new SUFFIX(); \
        Struct_##SUFFIX output; \
        f##SUFFIX->do_block(basis_bits, output); \
        return output.cc; \
    } \
}

CREATE_GENERAL_CODE_CATEGORY(Cc)
CREATE_GENERAL_CODE_CATEGORY(Cf)
CREATE_GENERAL_CODE_CATEGORY(Cn)
CREATE_GENERAL_CODE_CATEGORY(Co)
CREATE_GENERAL_CODE_CATEGORY(Cs)
CREATE_GENERAL_CODE_CATEGORY(Ll)
CREATE_GENERAL_CODE_CATEGORY(Lm)
CREATE_GENERAL_CODE_CATEGORY(Lo)
CREATE_GENERAL_CODE_CATEGORY(Lt)
CREATE_GENERAL_CODE_CATEGORY(Lu)
CREATE_GENERAL_CODE_CATEGORY(Mc)
CREATE_GENERAL_CODE_CATEGORY(Me)
CREATE_GENERAL_CODE_CATEGORY(Mn)
CREATE_GENERAL_CODE_CATEGORY(Nd)
CREATE_GENERAL_CODE_CATEGORY(Nl)
CREATE_GENERAL_CODE_CATEGORY(No)
CREATE_GENERAL_CODE_CATEGORY(Pc)
CREATE_GENERAL_CODE_CATEGORY(Pd)
CREATE_GENERAL_CODE_CATEGORY(Pe)
CREATE_GENERAL_CODE_CATEGORY(Pf)
CREATE_GENERAL_CODE_CATEGORY(Pi)
CREATE_GENERAL_CODE_CATEGORY(Po)
CREATE_GENERAL_CODE_CATEGORY(Ps)
CREATE_GENERAL_CODE_CATEGORY(Sc)
CREATE_GENERAL_CODE_CATEGORY(Sk)
CREATE_GENERAL_CODE_CATEGORY(Sm)
CREATE_GENERAL_CODE_CATEGORY(So)
CREATE_GENERAL_CODE_CATEGORY(Zl)
CREATE_GENERAL_CODE_CATEGORY(Zp)
CREATE_GENERAL_CODE_CATEGORY(Zs)

#undef CREATE_GENERAL_CODE_CATEGORY

namespace pablo {

PabloCompiler::PabloCompiler(const std::vector<Var*> & basisBits)
: mBasisBits(basisBits)
, mMod(new Module("icgrep", getGlobalContext()))
, mBasicBlock(nullptr)
, mExecutionEngine(nullptr)
, mBitBlockType(VectorType::get(IntegerType::get(mMod->getContext(), 64), BLOCK_SIZE / 64))
, mBasisBitsInputPtr(nullptr)
, mCarryQueueIdx(0)
, mCarryQueuePtr(nullptr)
, mNestingDepth(0)
, mCarryQueueSize(0)
, mAdvanceQueueIdx(0)
, mAdvanceQueuePtr(nullptr)
, mAdvanceQueueSize(0)
, mZeroInitializer(ConstantAggregateZero::get(mBitBlockType))
, mOneInitializer(ConstantVector::getAllOnesValue(mBitBlockType))
, mFunctionType(nullptr)
, mFunction(nullptr)
, mBasisBitsAddr(nullptr)
, mOutputAddrPtr(nullptr)
, mMaxNestingDepth(0)
{
    //Create the jit execution engine.up
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();
    DefineTypes();
    DeclareFunctions();
}

PabloCompiler::~PabloCompiler()
{
    delete mMod;
    delete fPs;
    delete fNl;
    delete fNo;
    delete fLo;
    delete fLl;
    delete fLm;
    delete fNd;
    delete fPc;
    delete fLt;
    delete fLu;
    delete fPf;
    delete fPd;
    delete fPe;
    delete fPi;
    delete fPo;
    delete fMe;
    delete fMc;
    delete fMn;
    delete fSk;
    delete fSo;
    delete fSm;
    delete fSc;
    delete fZl;
    delete fCo;
    delete fCn;
    delete fCc;
    delete fCf;
    delete fCs;
    delete fZp;
    delete fZs;
}

LLVM_Gen_RetVal PabloCompiler::compile(PabloBlock & pb)
{
    mNestingDepth = 0;
    mMaxNestingDepth = 0;
    mCarryQueueSize = 0;
    mAdvanceQueueSize = 0;
    Examine(pb.statements());
    mCarryQueueVector.resize(mCarryQueueSize);
    mAdvanceQueueVector.resize(mAdvanceQueueSize);
    std::string errMessage;
    EngineBuilder builder(mMod);
    builder.setErrorStr(&errMessage);
    builder.setMCPU(sys::getHostCPUName());
    builder.setUseMCJIT(true);
    builder.setOptLevel(mMaxNestingDepth ? CodeGenOpt::Level::Less : CodeGenOpt::Level::None);
    mExecutionEngine = builder.create();
    if (mExecutionEngine == nullptr) {
        throw std::runtime_error("Could not create ExecutionEngine: " + errMessage);
    }

    if (!mCalleeMap.empty()) {
        DeclareCallFunctions();
    }

    Function::arg_iterator args = mFunction->arg_begin();
    mBasisBitsAddr = args++;
    mBasisBitsAddr->setName("basis_bits");
    mCarryQueuePtr = args++;
    mCarryQueuePtr->setName("carry_q");
    mAdvanceQueuePtr = args++;
    mAdvanceQueuePtr->setName("advance_q");
    mOutputAddrPtr = args++;
    mOutputAddrPtr->setName("output");

    //Create the carry and advance queues.
    mCarryQueueIdx = 0;
    mAdvanceQueueIdx = 0;
    mNestingDepth = 0;
    mMaxNestingDepth = 0;
    mBasicBlock = BasicBlock::Create(mMod->getContext(), "parabix_entry", mFunction,0);

    //The basis bits structure
    for (unsigned i = 0; i != mBasisBits.size(); ++i) {
        IRBuilder<> b(mBasicBlock);
        Value* indices[] = {b.getInt64(0), b.getInt32(i)};
        const String * const name = mBasisBits[i]->getName();
        Value * gep = b.CreateGEP(mBasisBitsAddr, indices);
        LoadInst * basisBit = b.CreateAlignedLoad(gep, BLOCK_SIZE/8, false, name->str());
        mMarkerMap.insert(std::make_pair(name, basisBit));
    }

    //Generate the IR instructions for the function.
    compileStatements(pb.statements());

    assert (mCarryQueueIdx == mCarryQueueSize);
    assert (mAdvanceQueueIdx == mAdvanceQueueSize);
    assert (mNestingDepth == 0);
    //Terminate the block
    ReturnInst::Create(mMod->getContext(), mBasicBlock);

    //Un-comment this line in order to display the IR that has been generated by this module.
    #if defined(DUMP_GENERATED_IR)
    mMod->dump();
    #endif



    //Create a verifier.  The verifier will print an error message if our module is malformed in any way.
    #ifdef USE_LLVM_3_5
    verifyModule(*mMod, &dbgs());
    #endif
    #ifdef USE_LLVM_3_4
    verifyModule(*mMod, PrintMessageAction);
    #endif

    //Use the pass manager to run optimizations on the function.
    FunctionPassManager fpm(mMod);
 #ifdef USE_LLVM_3_5
    mMod->setDataLayout(mExecutionEngine->getDataLayout());
    // Set up the optimizer pipeline.  Start with registering info about how the target lays out data structures.
    fpm.add(new DataLayoutPass(mMod));
#endif
#ifdef USE_LLVM_3_4
    fpm.add(new DataLayout(*mExecutionEngine->getDataLayout()));
#endif
    fpm.doInitialization();
    fpm.run(*mFunction);

    mExecutionEngine->finalizeObject();

    LLVM_Gen_RetVal retVal;
    //Return the required size of the carry queue and a pointer to the process_block function.
    retVal.carry_q_size = mCarryQueueVector.size();
    retVal.advance_q_size = mAdvanceQueueVector.size();
    retVal.process_block_fptr = mExecutionEngine->getPointerToFunction(mFunction);

    return retVal;
}

void PabloCompiler::DefineTypes()
{
    StructType * structBasisBits = mMod->getTypeByName("struct.Basis_bits");
    if (structBasisBits == nullptr) {
        structBasisBits = StructType::create(mMod->getContext(), "struct.Basis_bits");
    }
    std::vector<Type*>StructTy_struct_Basis_bits_fields;
    for (int i = 0; i != mBasisBits.size(); i++)
    {
        StructTy_struct_Basis_bits_fields.push_back(mBitBlockType);
    }
    if (structBasisBits->isOpaque()) {
        structBasisBits->setBody(StructTy_struct_Basis_bits_fields, /*isPacked=*/false);
    }
    mBasisBitsInputPtr = PointerType::get(structBasisBits, 0);

    std::vector<Type*>functionTypeArgs;
    functionTypeArgs.push_back(mBasisBitsInputPtr);

    //The carry q array.
    //A pointer to the BitBlock vector.
    functionTypeArgs.push_back(PointerType::get(mBitBlockType, 0));
    // Advance q array
    functionTypeArgs.push_back(PointerType::get(mBitBlockType, 0));

    //The output structure.
    StructType * outputStruct = mMod->getTypeByName("struct.Output");
    if (!outputStruct) {
        outputStruct = StructType::create(mMod->getContext(), "struct.Output");
    }
    if (outputStruct->isOpaque()) {
        std::vector<Type*>fields;
        fields.push_back(mBitBlockType);
        fields.push_back(mBitBlockType);
        outputStruct->setBody(fields, /*isPacked=*/false);
    }
    PointerType* outputStructPtr = PointerType::get(outputStruct, 0);

    //The &output parameter.
    functionTypeArgs.push_back(outputStructPtr);

    mFunctionType = FunctionType::get(
     /*Result=*/Type::getVoidTy(mMod->getContext()),
     /*Params=*/functionTypeArgs,
     /*isVarArg=*/false);
}

void PabloCompiler::DeclareFunctions()
{
    //This function can be used for testing to print the contents of a register from JIT'd code to the terminal window.
    //mFunc_print_register = mMod->getOrInsertFunction("wrapped_print_register", Type::getVoidTy(getGlobalContext()), mXi64Vect, NULL);
    //mExecutionEngine->addGlobalMapping(cast<GlobalValue>(mFunc_print_register), (void *)&wrapped_print_register);
    // to call->  b.CreateCall(mFunc_print_register, unicode_category);

#ifdef USE_UADD_OVERFLOW
#ifdef USE_TWO_UADD_OVERFLOW
    // Type Definitions for llvm.uadd.with.overflow.carryin.i128 or .i256
    std::vector<Type*>StructTy_0_fields;
    StructTy_0_fields.push_back(IntegerType::get(mMod->getContext(), BLOCK_SIZE));
    StructTy_0_fields.push_back(IntegerType::get(mMod->getContext(), 1));
    StructType *StructTy_0 = StructType::get(mMod->getContext(), StructTy_0_fields, /*isPacked=*/false);

    std::vector<Type*>FuncTy_1_args;
    FuncTy_1_args.push_back(IntegerType::get(mMod->getContext(), BLOCK_SIZE));
    FuncTy_1_args.push_back(IntegerType::get(mMod->getContext(), BLOCK_SIZE));
    FunctionType* FuncTy_1 = FunctionType::get(
                                              /*Result=*/StructTy_0,
                                              /*Params=*/FuncTy_1_args,
                                              /*isVarArg=*/false);

    mFunctionUaddOverflow = mMod->getFunction("llvm.uadd.with.overflow.i" +
                                              std::to_string(BLOCK_SIZE));
    if (!mFunctionUaddOverflow) {
        mFunctionUaddOverflow= Function::Create(
          /*Type=*/ FuncTy_1,
          /*Linkage=*/ GlobalValue::ExternalLinkage,
          /*Name=*/ "llvm.uadd.with.overflow.i" + std::to_string(BLOCK_SIZE), mMod); // (external, no body)
        mFunctionUaddOverflow->setCallingConv(CallingConv::C);
    }
    AttributeSet mFunctionUaddOverflowPAL;
    {
        SmallVector<AttributeSet, 4> Attrs;
        AttributeSet PAS;
        {
          AttrBuilder B;
          B.addAttribute(Attribute::NoUnwind);
          B.addAttribute(Attribute::ReadNone);
          PAS = AttributeSet::get(mMod->getContext(), ~0U, B);
        }

        Attrs.push_back(PAS);
        mFunctionUaddOverflowPAL = AttributeSet::get(mMod->getContext(), Attrs);
    }
    mFunctionUaddOverflow->setAttributes(mFunctionUaddOverflowPAL);
#else
    // Type Definitions for llvm.uadd.with.overflow.carryin.i128 or .i256
    std::vector<Type*>StructTy_0_fields;
    StructTy_0_fields.push_back(IntegerType::get(mMod->getContext(), BLOCK_SIZE));
    StructTy_0_fields.push_back(IntegerType::get(mMod->getContext(), 1));
    StructType *StructTy_0 = StructType::get(mMod->getContext(), StructTy_0_fields, /*isPacked=*/false);

    std::vector<Type*>FuncTy_1_args;
    FuncTy_1_args.push_back(IntegerType::get(mMod->getContext(), BLOCK_SIZE));
    FuncTy_1_args.push_back(IntegerType::get(mMod->getContext(), BLOCK_SIZE));
    FuncTy_1_args.push_back(IntegerType::get(mMod->getContext(), 1));
    FunctionType* FuncTy_1 = FunctionType::get(
                                              /*Result=*/StructTy_0,
                                              /*Params=*/FuncTy_1_args,
                                              /*isVarArg=*/false);

    mFunctionUaddOverflowCarryin = mMod->getFunction("llvm.uadd.with.overflow.carryin.i" +
                                              std::to_string(BLOCK_SIZE));
    if (!mFunctionUaddOverflowCarryin) {
        mFunctionUaddOverflowCarryin = Function::Create(
          /*Type=*/ FuncTy_1,
          /*Linkage=*/ GlobalValue::ExternalLinkage,
          /*Name=*/ "llvm.uadd.with.overflow.carryin.i" + std::to_string(BLOCK_SIZE), mMod); // (external, no body)
        mFunctionUaddOverflowCarryin->setCallingConv(CallingConv::C);
    }
    AttributeSet mFunctionUaddOverflowCarryinPAL;
    {
        SmallVector<AttributeSet, 4> Attrs;
        AttributeSet PAS;
        {
          AttrBuilder B;
          B.addAttribute(Attribute::NoUnwind);
          B.addAttribute(Attribute::ReadNone);
          PAS = AttributeSet::get(mMod->getContext(), ~0U, B);
        }

        Attrs.push_back(PAS);
        mFunctionUaddOverflowCarryinPAL = AttributeSet::get(mMod->getContext(), Attrs);
    }
    mFunctionUaddOverflowCarryin->setAttributes(mFunctionUaddOverflowCarryinPAL);
#endif
#endif

    //Starts on process_block
    SmallVector<AttributeSet, 5> Attrs;
    AttributeSet PAS;
    {
        AttrBuilder B;
        B.addAttribute(Attribute::ReadOnly);
        B.addAttribute(Attribute::NoCapture);
        PAS = AttributeSet::get(mMod->getContext(), 1U, B);
    }
    Attrs.push_back(PAS);
    {
        AttrBuilder B;
        B.addAttribute(Attribute::NoCapture);
        PAS = AttributeSet::get(mMod->getContext(), 2U, B);
    }
    Attrs.push_back(PAS);
    {
        AttrBuilder B;
        B.addAttribute(Attribute::NoCapture);
        PAS = AttributeSet::get(mMod->getContext(), 3U, B);
    }
    Attrs.push_back(PAS);
    {
        AttrBuilder B;
        B.addAttribute(Attribute::NoCapture);
        PAS = AttributeSet::get(mMod->getContext(), 4U, B);
    }
    Attrs.push_back(PAS);
    {
        AttrBuilder B;
        B.addAttribute(Attribute::NoUnwind);
        B.addAttribute(Attribute::UWTable);
        PAS = AttributeSet::get(mMod->getContext(), ~0U, B);
    }
    AttributeSet AttrSet = AttributeSet::get(mMod->getContext(), Attrs);

    //Create the function that will be generated.
    mFunction = mMod->getFunction("process_block");
    if (!mFunction) {
        mFunction = Function::Create(
            /*Type=*/mFunctionType,
            /*Linkage=*/GlobalValue::ExternalLinkage,
            /*Name=*/"process_block", mMod);
        mFunction->setCallingConv(CallingConv::C);
    }
    mFunction->setAttributes(AttrSet);
}

void PabloCompiler::Examine(StatementList & stmts) {
    for (Statement * stmt : stmts) {
        if (Assign * assign = dyn_cast<Assign>(stmt)) {
            Examine(assign->getExpr());
        }
        if (Next * next = dyn_cast<Next>(stmt)) {
            Examine(next->getExpr());
        }
        else if (If * ifStatement = dyn_cast<If>(stmt)) {
            const auto preIfCarryCount = mCarryQueueSize;
            const auto preIfAdvanceCount = mAdvanceQueueSize;
            Examine(ifStatement->getCondition());
            Examine(ifStatement->getBody());
            int ifCarryCount = mCarryQueueSize - preIfCarryCount;
            int ifAdvanceCount = mAdvanceQueueSize - preIfAdvanceCount;
            if ((ifCarryCount + ifAdvanceCount) > 1) {
              ++mAdvanceQueueSize;
              ++ifAdvanceCount;
            }
            ifStatement->setInclusiveCarryCount(ifCarryCount);
            ifStatement->setInclusiveAdvanceCount(ifAdvanceCount);
        }
        else if (While * whileStatement = dyn_cast<While>(stmt)) {
            const auto preWhileCarryCount = mCarryQueueSize;
            const auto preWhileAdvanceCount = mAdvanceQueueSize;
            Examine(whileStatement->getCondition());
            mMaxNestingDepth = std::max(mMaxNestingDepth, ++mNestingDepth);
            Examine(whileStatement->getBody());
            --mNestingDepth;
            whileStatement->setInclusiveCarryCount(mCarryQueueSize - preWhileCarryCount);
            whileStatement->setInclusiveAdvanceCount(mAdvanceQueueSize - preWhileAdvanceCount);
        }
    }
}

void PabloCompiler::Examine(PabloAST *expr)
{
    if (Call * call = dyn_cast<Call>(expr)) {
        mCalleeMap.insert(std::make_pair(call->getCallee(), nullptr));
    }
    else if (And * pablo_and = dyn_cast<And>(expr)) {
        Examine(pablo_and->getExpr1());
        Examine(pablo_and->getExpr2());
    }
    else if (Or * pablo_or = dyn_cast<Or>(expr)) {
        Examine(pablo_or->getExpr1());
        Examine(pablo_or->getExpr2());
    }
    else if (Sel * pablo_sel = dyn_cast<Sel>(expr)) {
        Examine(pablo_sel->getCondition());
        Examine(pablo_sel->getTrueExpr());
        Examine(pablo_sel->getFalseExpr());
    }
    else if (Not * pablo_not = dyn_cast<Not>(expr)) {
        Examine(pablo_not->getExpr());
    }
    else if (Advance * adv = dyn_cast<Advance>(expr)) {
        ++mAdvanceQueueSize;
        Examine(adv->getExpr());
    }
    else if (MatchStar * mstar = dyn_cast<MatchStar>(expr)) {
        ++mCarryQueueSize;
        Examine(mstar->getMarker());
        Examine(mstar->getCharClass());
    }
    else if (ScanThru * sthru = dyn_cast<ScanThru>(expr)) {
        ++mCarryQueueSize;
        Examine(sthru->getScanFrom());
        Examine(sthru->getScanThru());
    }
}

void PabloCompiler::DeclareCallFunctions() {
    for (auto mapping : mCalleeMap) {
        const String * callee = mapping.first;
        void * callee_ptr = nullptr;
        #define CHECK_GENERAL_CODE_CATEGORY(SUFFIX) \
            if (callee->str() == #SUFFIX) { \
                callee_ptr = (void*)&__get_category_##SUFFIX; \
            } else
        CHECK_GENERAL_CODE_CATEGORY(Cc)
        CHECK_GENERAL_CODE_CATEGORY(Cf)
        CHECK_GENERAL_CODE_CATEGORY(Cn)
        CHECK_GENERAL_CODE_CATEGORY(Co)
        CHECK_GENERAL_CODE_CATEGORY(Cs)
        CHECK_GENERAL_CODE_CATEGORY(Ll)
        CHECK_GENERAL_CODE_CATEGORY(Lm)
        CHECK_GENERAL_CODE_CATEGORY(Lo)
        CHECK_GENERAL_CODE_CATEGORY(Lt)
        CHECK_GENERAL_CODE_CATEGORY(Lu)
        CHECK_GENERAL_CODE_CATEGORY(Mc)
        CHECK_GENERAL_CODE_CATEGORY(Me)
        CHECK_GENERAL_CODE_CATEGORY(Mn)
        CHECK_GENERAL_CODE_CATEGORY(Nd)
        CHECK_GENERAL_CODE_CATEGORY(Nl)
        CHECK_GENERAL_CODE_CATEGORY(No)
        CHECK_GENERAL_CODE_CATEGORY(Pc)
        CHECK_GENERAL_CODE_CATEGORY(Pd)
        CHECK_GENERAL_CODE_CATEGORY(Pe)
        CHECK_GENERAL_CODE_CATEGORY(Pf)
        CHECK_GENERAL_CODE_CATEGORY(Pi)
        CHECK_GENERAL_CODE_CATEGORY(Po)
        CHECK_GENERAL_CODE_CATEGORY(Ps)
        CHECK_GENERAL_CODE_CATEGORY(Sc)
        CHECK_GENERAL_CODE_CATEGORY(Sk)
        CHECK_GENERAL_CODE_CATEGORY(Sm)
        CHECK_GENERAL_CODE_CATEGORY(So)
        CHECK_GENERAL_CODE_CATEGORY(Zl)
        CHECK_GENERAL_CODE_CATEGORY(Zp)
        CHECK_GENERAL_CODE_CATEGORY(Zs)
        // OTHERWISE ...
        throw std::runtime_error("Unknown unicode category \"" + callee->str() + "\"");
        #undef CHECK_GENERAL_CODE_CATEGORY
        Value * unicodeCategory = mMod->getOrInsertFunction("__get_category_" + callee->str(), mBitBlockType, mBasisBitsInputPtr, NULL);
        if (LLVM_UNLIKELY(unicodeCategory == nullptr)) {
            throw std::runtime_error("Could not create static method call for unicode category \"" + callee->str() + "\"");
        }
        mExecutionEngine->addGlobalMapping(cast<GlobalValue>(unicodeCategory), callee_ptr);
        mCalleeMap[callee] = unicodeCategory;
    }
}

void PabloCompiler::compileStatements(const StatementList & stmts) {
    for (const PabloAST * statement : stmts) {
        compileStatement(statement);
    }
}

void PabloCompiler::compileStatement(const PabloAST * stmt)
{
    if (const Assign * assign = dyn_cast<const Assign>(stmt))
    {
        Value* expr = compileExpression(assign->getExpr());
        mMarkerMap[assign->getName()] = expr;
        if (LLVM_UNLIKELY(assign->isOutputAssignment())) {
            SetOutputValue(expr, assign->getOutputIndex());
        }
    }
    if (const Next * next = dyn_cast<const Next>(stmt))
    {
        Value* expr = compileExpression(next->getExpr());
        mMarkerMap[next->getName()] = expr;
    }
    else if (const If * ifStatement = dyn_cast<const If>(stmt))
    //
    //  The If-ElseZero stmt:
    //  if <predicate:expr> then <body:stmt>* elsezero <defined:var>* endif
    //  If the value of the predicate is nonzero, then determine the values of variables
    //  <var>* by executing the given statements.  Otherwise, the value of the
    //  variables are all zero.  Requirements: (a) no variable that is defined within 
    //  the body of the if may be accessed outside unless it is explicitly  
    //  listed in the variable list, (b) every variable in the defined list receives
    //  a value within the body, and (c) the logical consequence of executing
    //  the statements in the event that the predicate is zero is that the
    //  values of all defined variables indeed work out to be 0.
    //
    //  Simple Implementation with Phi nodes:  a phi node in the if exit block
    //  is inserted for each variable in the defined variable list.  It receives
    //  a zero value from the ifentry block and the defined value from the if
    //  body.
    //
    {
        BasicBlock * ifEntryBlock = mBasicBlock;  // The block we are in.
        BasicBlock * ifBodyBlock = BasicBlock::Create(mMod->getContext(), "if.body", mFunction, 0);
        BasicBlock * ifEndBlock = BasicBlock::Create(mMod->getContext(), "if.end", mFunction, 0);
        
        const auto baseCarryQueueIdx = mCarryQueueIdx;
        const auto baseAdvanceQueueIdx = mAdvanceQueueIdx;
        
        int ifCarryCount = ifStatement->getInclusiveCarryCount();
        int ifAdvanceCount = ifStatement->getInclusiveAdvanceCount();
        //  Carry/Advance queue strategy.   
        //  If there are any carries or advances at any nesting level within the
        //  if statement, then the statement must be executed.   A "summary" 
        //  carryover variable is determined for this purpose, consisting of the
        //  or of all of the carry and advance variables within the if.
        //  This variable is determined as follows.
        //  (a)  If the CarryCount and AdvanceCount are both 0, there is no summary variable.
        //  (b)  If the CarryCount is 1 and the AdvanceCount is 0, then the summary
        //       carryover variable is just the single carry queue entry.
        //  (c)  If the CarryCount is 0 and the AdvanceCount is 1, then the summary
        //       carryover variable is just the advance carry queue entry.
        //  (d)  Otherwise, an additional advance queue entry is created for the
        //       summary variable.
        //  Note that the test for cases (c) and (d) may be combined: the summary carryover 
        //  variable is just last advance queue entry.
        //
        
        IRBuilder<> b_entry(ifEntryBlock);
        mBasicBlock = ifEntryBlock;
        Value* if_test_value = compileExpression(ifStatement->getCondition());
        
        if ((ifCarryCount == 1) && (ifAdvanceCount == 0)) {
            Value* last_if_pending_carries = genCarryInLoad(baseCarryQueueIdx);
            if_test_value = b_entry.CreateOr(if_test_value, last_if_pending_carries);
        }
        else if ((ifCarryCount > 0) || (ifAdvanceCount > 0)) {
            Value* last_if_pending_advances = genAdvanceInLoad(baseAdvanceQueueIdx + ifAdvanceCount - 1);
            if_test_value = b_entry.CreateOr(if_test_value, last_if_pending_advances);
        }
        b_entry.CreateCondBr(genBitBlockAny(if_test_value), ifEndBlock, ifBodyBlock);

        // Entry processing is complete, now handle the body of the if.
        
        IRBuilder<> bIfBody(ifBodyBlock);
        mBasicBlock = ifBodyBlock;
        
        compileStatements(ifStatement->getBody());
        
        // After the recursive compile, now insert the code to compute the summary
        // carry over variable.
        
        if ((ifCarryCount + ifAdvanceCount) > 1) {
            // A summary variable is needed.

            Value * carry_summary = mZeroInitializer;
            for (int c = baseCarryQueueIdx; c < baseCarryQueueIdx + ifCarryCount; c++)
            {
                Value* carryq_value = genCarryInLoad(c);
                carry_summary = bIfBody.CreateOr(carry_summary, carryq_value);
            }
            // Note that the limit in the following uses -1, because
            // last entry of the advance queue is for the summary variable.
            for (int c = baseAdvanceQueueIdx; c < baseAdvanceQueueIdx + ifAdvanceCount - 1; c++)
            {
                Value* advance_q_value = genAdvanceInLoad(c);
                carry_summary = bIfBody.CreateOr(advance_q_value, carry_summary);
            }
            genAdvanceOutStore(carry_summary, mAdvanceQueueIdx++); //baseAdvanceQueueIdx + ifAdvanceCount - 1);
        }
        bIfBody.CreateBr(ifEndBlock);

        //End Block
        IRBuilder<> bEnd(ifEndBlock);
        mBasicBlock = ifEndBlock;
        
        for (const Assign * a : ifStatement->getDefined()) {
            PHINode * phi = bEnd.CreatePHI(mBitBlockType, 2, a->getName()->str());
            auto f = mMarkerMap.find(a->getName());
            assert (f != mMarkerMap.end());
            phi->addIncoming(mZeroInitializer, ifEntryBlock);
            phi->addIncoming(f->second, ifBodyBlock);
            mMarkerMap[a->getName()] = phi;
        }
    }
    else if (const While * whileStatement = dyn_cast<const While>(stmt))
    {
        const auto baseCarryQueueIdx = mCarryQueueIdx;
        const auto baseAdvanceQueueIdx = mAdvanceQueueIdx;
        if (mNestingDepth == 0) {
            for (auto i = 0; i != whileStatement->getInclusiveCarryCount(); ++i) {
                genCarryInLoad(baseCarryQueueIdx + i);
            }
            for (auto i = 0; i != whileStatement->getInclusiveAdvanceCount(); ++i) {
                genAdvanceInLoad(baseAdvanceQueueIdx + i);
            }
        }

        SmallVector<const Next*, 4> nextNodes;
        for (const PabloAST * node : whileStatement->getBody()) {
            if (isa<Next>(node)) {
                nextNodes.push_back(cast<Next>(node));
            }
        }

        // Compile the initial iteration statements; the calls to genCarryOutStore will update the
        // mCarryQueueVector with the appropriate values. Although we're not actually entering a new basic
        // block yet, increment the nesting depth so that any calls to genCarryInLoad or genCarryOutStore
        // will refer to the previous value.

        ++mNestingDepth;

        compileStatements(whileStatement->getBody());

        // Reset the carry queue index. Note: this ought to be changed in the future. Currently this assumes
        // that compiling the while body twice will generate the equivalent IR. This is not necessarily true
        // but works for now.
        mCarryQueueIdx = baseCarryQueueIdx;
        mAdvanceQueueIdx = baseAdvanceQueueIdx;

        BasicBlock* whileCondBlock = BasicBlock::Create(mMod->getContext(), "while.cond", mFunction, 0);
        BasicBlock* whileBodyBlock = BasicBlock::Create(mMod->getContext(), "while.body", mFunction, 0);
        BasicBlock* whileEndBlock = BasicBlock::Create(mMod->getContext(), "while.end", mFunction, 0);

        // Note: compileStatements may update the mBasicBlock pointer if the body contains nested loops. It
        // may not be same one that we entered the function with.
        IRBuilder<> bEntry(mBasicBlock);
        bEntry.CreateBr(whileCondBlock);

        // CONDITION BLOCK
        IRBuilder<> bCond(whileCondBlock);
        // generate phi nodes for any carry propogating instruction
        int whileCarryCount = whileStatement->getInclusiveCarryCount();
        int whileAdvanceCount = whileStatement->getInclusiveAdvanceCount();
        std::vector<PHINode*> phiNodes(whileCarryCount + whileAdvanceCount + nextNodes.size());
        unsigned index = 0;
        for (index = 0; index != whileCarryCount; ++index) {
            PHINode * phi = bCond.CreatePHI(mBitBlockType, 2);
            phi->addIncoming(mCarryQueueVector[baseCarryQueueIdx + index], mBasicBlock);
            mCarryQueueVector[baseCarryQueueIdx + index] = mZeroInitializer; // (use phi for multi-carry mode.)
            phiNodes[index] = phi;
        }
        for (int i = 0; i != whileAdvanceCount; ++i) {
            PHINode * phi = bCond.CreatePHI(mBitBlockType, 2);
            phi->addIncoming(mAdvanceQueueVector[baseAdvanceQueueIdx + i], mBasicBlock);
            mAdvanceQueueVector[baseAdvanceQueueIdx + i] = mZeroInitializer; // (use phi for multi-carry mode.)
            phiNodes[index++] = phi;
        }
        // and for any Next nodes in the loop body
        for (const Next * n : nextNodes) {
            PHINode * phi = bCond.CreatePHI(mBitBlockType, 2, n->getName()->str());
            auto f = mMarkerMap.find(n->getName());
            assert (f != mMarkerMap.end());
            phi->addIncoming(f->second, mBasicBlock);
            mMarkerMap[n->getName()] = phi;
            phiNodes[index++] = phi;
        }

        mBasicBlock = whileCondBlock;
        bCond.CreateCondBr(genBitBlockAny(compileExpression(whileStatement->getCondition())), whileEndBlock, whileBodyBlock);

        // BODY BLOCK
        mBasicBlock = whileBodyBlock;
        compileStatements(whileStatement->getBody());
        // update phi nodes for any carry propogating instruction
        IRBuilder<> bWhileBody(mBasicBlock);
        for (index = 0; index != whileStatement->getInclusiveCarryCount(); ++index) {
            Value * carryOut = bWhileBody.CreateOr(phiNodes[index], mCarryQueueVector[baseCarryQueueIdx + index]);
            PHINode * phi = phiNodes[index];
            phi->addIncoming(carryOut, mBasicBlock);
            mCarryQueueVector[baseCarryQueueIdx + index] = phi;
        }
        for (int i = 0; i != whileAdvanceCount; ++i) {
            Value * advOut = bWhileBody.CreateOr(phiNodes[index], mAdvanceQueueVector[baseAdvanceQueueIdx + i]);
            PHINode * phi = phiNodes[index++];
            phi->addIncoming(advOut, mBasicBlock);
            mAdvanceQueueVector[baseAdvanceQueueIdx + i] = phi;
        }
        // and for any Next nodes in the loop body
        for (const Next * n : nextNodes) {
            auto f = mMarkerMap.find(n->getName());
            assert (f != mMarkerMap.end());
            PHINode * phi = phiNodes[index++];
            phi->addIncoming(f->second, mBasicBlock);
            mMarkerMap[n->getName()] = phi;
        }

        bWhileBody.CreateBr(whileCondBlock);

        // EXIT BLOCK
        mBasicBlock = whileEndBlock;
        if (--mNestingDepth == 0) {
            for (index = 0; index != whileCarryCount; ++index) {
                genCarryOutStore(phiNodes[index], baseCarryQueueIdx + index);
            }
            for (index = 0; index != whileAdvanceCount; ++index) {
                genAdvanceOutStore(phiNodes[whileCarryCount + index], baseAdvanceQueueIdx + index);
            }
        }
    }
}

Value * PabloCompiler::compileExpression(const PabloAST * expr)
{
    Value * retVal = nullptr;
    IRBuilder<> b(mBasicBlock);
    if (isa<Ones>(expr)) {
        retVal = mOneInitializer;
    }
    else if (isa<Zeroes>(expr)) {
        retVal = mZeroInitializer;
    }
    else if (const Call* call = dyn_cast<Call>(expr)) {
        //Call the callee once and store the result in the marker map.
        auto mi = mMarkerMap.find(call->getCallee());
        if (mi == mMarkerMap.end()) {
            auto ci = mCalleeMap.find(call->getCallee());
            if (LLVM_UNLIKELY(ci == mCalleeMap.end())) {
                throw std::runtime_error("Unexpected error locating static function for \"" + call->getCallee()->str() + "\"");
            }
            mi = mMarkerMap.insert(std::make_pair(call->getCallee(), b.CreateCall(ci->second, mBasisBitsAddr))).first;
        }
        retVal = mi->second;
    }
    else if (const Var * var = dyn_cast<Var>(expr))
    {
        auto f = mMarkerMap.find(var->getName());
        if (LLVM_UNLIKELY(f == mMarkerMap.end())) {
            throw std::runtime_error(PabloPrinter::print(var) + " used before creation.");
        }
        retVal = f->second;
    }
    else if (const And * pablo_and = dyn_cast<And>(expr))
    {
        retVal = b.CreateAnd(compileExpression(pablo_and->getExpr1()), compileExpression(pablo_and->getExpr2()), "and");
    }
    else if (const Or * pablo_or = dyn_cast<Or>(expr))
    {
        retVal = b.CreateOr(compileExpression(pablo_or->getExpr1()), compileExpression(pablo_or->getExpr2()), "or");
    }
    else if (const Sel * sel = dyn_cast<Sel>(expr))
    {
        Value* ifMask = compileExpression(sel->getCondition());
        Value* ifTrue = b.CreateAnd(ifMask, compileExpression(sel->getTrueExpr()));
        Value* ifFalse = b.CreateAnd(genNot(ifMask), compileExpression(sel->getFalseExpr()));
        retVal = b.CreateOr(ifTrue, ifFalse);
    }
    else if (const Not * pablo_not = dyn_cast<Not>(expr))
    {
        retVal = genNot(compileExpression(pablo_not->getExpr()));
    }
    else if (const Advance * adv = dyn_cast<Advance>(expr))
    {
        Value* strm_value = compileExpression(adv->getExpr());
        int shift = adv->getAdvanceAmount();
        retVal = genAdvanceWithCarry(strm_value, shift);
    }
    else if (const MatchStar * mstar = dyn_cast<MatchStar>(expr))
    {
        Value* marker = compileExpression(mstar->getMarker());
        Value* cc = compileExpression(mstar->getCharClass());
        Value* marker_and_cc = b.CreateAnd(marker, cc);
        retVal = b.CreateOr(b.CreateXor(genAddWithCarry(marker_and_cc, cc), cc), marker, "matchstar");
    }
    else if (const ScanThru * sthru = dyn_cast<ScanThru>(expr))
    {
        Value* marker_expr = compileExpression(sthru->getScanFrom());
        Value* cc_expr = compileExpression(sthru->getScanThru());
        retVal = b.CreateAnd(genAddWithCarry(marker_expr, cc_expr), genNot(cc_expr), "scanthru");
    }
    return retVal;
}

#ifdef USE_UADD_OVERFLOW
#ifdef USE_TWO_UADD_OVERFLOW
PabloCompiler::SumWithOverflowPack PabloCompiler::callUaddOverflow(Value* int128_e1, Value* int128_e2) {
    std::vector<Value*> struct_res_params;
    struct_res_params.push_back(int128_e1);
    struct_res_params.push_back(int128_e2);
    CallInst* struct_res = CallInst::Create(mFunctionUaddOverflow, struct_res_params, "uadd_overflow_res", mBasicBlock);
    struct_res->setCallingConv(CallingConv::C);
    struct_res->setTailCall(false);
    AttributeSet struct_res_PAL;
    struct_res->setAttributes(struct_res_PAL);

    SumWithOverflowPack ret;

    std::vector<unsigned> int128_sum_indices;
    int128_sum_indices.push_back(0);
    ret.sum = ExtractValueInst::Create(struct_res, int128_sum_indices, "sum", mBasicBlock);

    std::vector<unsigned> int1_obit_indices;
    int1_obit_indices.push_back(1);
    ret.obit = ExtractValueInst::Create(struct_res, int1_obit_indices, "obit", mBasicBlock);

    return ret;
}
#else
PabloCompiler::SumWithOverflowPack PabloCompiler::callUaddOverflow(Value* int128_e1, Value* int128_e2, Value* int1_cin) {
    std::vector<Value*> struct_res_params;
    struct_res_params.push_back(int128_e1);
    struct_res_params.push_back(int128_e2);
    struct_res_params.push_back(int1_cin);
    CallInst* struct_res = CallInst::Create(mFunctionUaddOverflowCarryin, struct_res_params, "uadd_overflow_res", mBasicBlock);
    struct_res->setCallingConv(CallingConv::C);
    struct_res->setTailCall(false);
    AttributeSet struct_res_PAL;
    struct_res->setAttributes(struct_res_PAL);

    SumWithOverflowPack ret;

    std::vector<unsigned> int128_sum_indices;
    int128_sum_indices.push_back(0);
    ret.sum = ExtractValueInst::Create(struct_res, int128_sum_indices, "sum", mBasicBlock);

    std::vector<unsigned> int1_obit_indices;
    int1_obit_indices.push_back(1);
    ret.obit = ExtractValueInst::Create(struct_res, int1_obit_indices, "obit", mBasicBlock);

    return ret;
}
#endif
#endif

Value* PabloCompiler::genAddWithCarry(Value* e1, Value* e2) {
    IRBuilder<> b(mBasicBlock);

    //CarryQ - carry in.
    const int carryIdx = mCarryQueueIdx++;
    Value* carryq_value = genCarryInLoad(carryIdx);
#ifdef USE_TWO_UADD_OVERFLOW
    //This is the ideal implementation, which uses two uadd.with.overflow
    //The back end should be able to recognize this pattern and combine it into uadd.with.overflow.carryin
    CastInst* int128_e1 = new BitCastInst(e1, IntegerType::get(mMod->getContext(), BLOCK_SIZE), "e1_128", mBasicBlock);
    CastInst* int128_e2 = new BitCastInst(e2, IntegerType::get(mMod->getContext(), BLOCK_SIZE), "e2_128", mBasicBlock);
    CastInst* int128_carryq_value = new BitCastInst(carryq_value, IntegerType::get(mMod->getContext(), BLOCK_SIZE), "carryq_128", mBasicBlock);

    SumWithOverflowPack sumpack0, sumpack1;

    sumpack0 = callUaddOverflow(int128_e1, int128_e2);
    sumpack1 = callUaddOverflow(sumpack0.sum, int128_carryq_value);

    Value* obit = b.CreateOr(sumpack0.obit, sumpack1.obit, "carry_bit");
    Value* sum = b.CreateBitCast(sumpack1.sum, mBitBlockType, "ret_sum");

    /*obit is the i1 carryout, zero extend and insert it into a v2i64 or v4i64 vector.*/
    ConstantAggregateZero* const_packed_5 = ConstantAggregateZero::get(mBitBlockType);
    ConstantInt* const_int32_6 = ConstantInt::get(mMod->getContext(), APInt(32, StringRef("0"), 10));
    CastInst* int64_o0 = new ZExtInst(obit, IntegerType::get(mMod->getContext(), 64), "o0", mBasicBlock);
    InsertElementInst* carry_out = InsertElementInst::Create(const_packed_5, int64_o0, const_int32_6, "carry_out", mBasicBlock);

#elif defined USE_UADD_OVERFLOW
    //use llvm.uadd.with.overflow.i128 or i256
    CastInst* int128_e1 = new BitCastInst(e1, IntegerType::get(mMod->getContext(), BLOCK_SIZE), "e1_128", mBasicBlock);
    CastInst* int128_e2 = new BitCastInst(e2, IntegerType::get(mMod->getContext(), BLOCK_SIZE), "e2_128", mBasicBlock);

    //get i1 carryin from iBLOCK_SIZE
    ConstantInt* const_int32_6 = ConstantInt::get(mMod->getContext(), APInt(32, StringRef("0"), 10));
    ExtractElementInst * int64_carryq_value = ExtractElementInst::Create(carryq_value, const_int32_6, "carryq_64", mBasicBlock);
    CastInst* int1_carryq_value = new TruncInst(int64_carryq_value, IntegerType::get(mMod->getContext(), 1), "carryq_1", mBasicBlock);

    SumWithOverflowPack sumpack0;
    sumpack0 = callUaddOverflow(int128_e1, int128_e2, int1_carryq_value);
    Value* obit = sumpack0.obit;
    Value* sum = b.CreateBitCast(sumpack0.sum, mBitBlockType, "sum");

    /*obit is the i1 carryout, zero extend and insert it into a v2i64 or v4i64 vector.*/
    ConstantAggregateZero* const_packed_5 = ConstantAggregateZero::get(mBitBlockType);
    CastInst* int64_o0 = new ZExtInst(obit, IntegerType::get(mMod->getContext(), 64), "o0", mBasicBlock);
    InsertElementInst* carry_out = InsertElementInst::Create(const_packed_5, int64_o0, const_int32_6, "carry_out", mBasicBlock);
#elif (BLOCK_SIZE == 128)
    //calculate carry through logical ops
    Value* carrygen = b.CreateAnd(e1, e2, "carrygen");
    Value* carryprop = b.CreateOr(e1, e2, "carryprop");
    Value* digitsum = b.CreateAdd(e1, e2, "digitsum");
    Value* partial = b.CreateAdd(digitsum, carryq_value, "partial");
    Value* digitcarry = b.CreateOr(carrygen, b.CreateAnd(carryprop, genNot(partial)));
    Value* mid_carry_in = genShiftLeft64(b.CreateLShr(digitcarry, 63), "mid_carry_in");

    Value* sum = b.CreateAdd(partial, mid_carry_in, "sum");
    Value* carry_out = genShiftHighbitToLow(b.CreateOr(carrygen, b.CreateAnd(carryprop, genNot(sum))), "carry_out");
#else
    //BLOCK_SIZE == 256, there is no other implementation
    static_assert(false, "Add with carry for 256-bit bitblock requires USE_UADD_OVERFLOW");
#endif //USE_TWO_UADD_OVERFLOW

    genCarryOutStore(carry_out, carryIdx);
    return sum;
}

Value* PabloCompiler::genCarryInLoad(const unsigned index) {
    assert (index < mCarryQueueVector.size());
    if (mNestingDepth == 0) {
        IRBuilder<> b(mBasicBlock);
        mCarryQueueVector[index] = b.CreateAlignedLoad(b.CreateGEP(mCarryQueuePtr, b.getInt64(index)), BLOCK_SIZE/8, false);
    }
    return mCarryQueueVector[index];
}

void PabloCompiler::genCarryOutStore(Value* carryOut, const unsigned index ) {
    assert (carryOut);
    assert (index < mCarryQueueVector.size());
    if (mNestingDepth == 0) {
        IRBuilder<> b(mBasicBlock);
        b.CreateAlignedStore(carryOut, b.CreateGEP(mCarryQueuePtr, b.getInt64(index)), BLOCK_SIZE/8, false);
    }
    mCarryQueueVector[index] = carryOut;
}

Value* PabloCompiler::genAdvanceInLoad(const unsigned index) {
    assert (index < mAdvanceQueueVector.size());
    if (mNestingDepth == 0) {
        IRBuilder<> b(mBasicBlock);
        mAdvanceQueueVector[index] = b.CreateAlignedLoad(b.CreateGEP(mAdvanceQueuePtr, b.getInt64(index)), BLOCK_SIZE/8, false);
    }
    return mAdvanceQueueVector[index];
}

void PabloCompiler::genAdvanceOutStore(Value* advanceOut, const unsigned index ) {
    assert (advanceOut);
    assert (index < mAdvanceQueueVector.size());
    if (mNestingDepth == 0) {
        IRBuilder<> b(mBasicBlock);
        b.CreateAlignedStore(advanceOut, b.CreateGEP(mAdvanceQueuePtr, b.getInt64(index)), BLOCK_SIZE/8, false);
    }
    mAdvanceQueueVector[index] = advanceOut;
}

inline Value* PabloCompiler::genBitBlockAny(Value* test) {
    IRBuilder<> b(mBasicBlock);
    Value* cast_marker_value_1 = b.CreateBitCast(test, IntegerType::get(mMod->getContext(), BLOCK_SIZE));
    return b.CreateICmpEQ(cast_marker_value_1, ConstantInt::get(IntegerType::get(mMod->getContext(), BLOCK_SIZE), 0));
}

Value* PabloCompiler::genShiftHighbitToLow(Value* e, const Twine &namehint) {
    IRBuilder<> b(mBasicBlock);
    Value* i128_val = b.CreateBitCast(e, IntegerType::get(mMod->getContext(), BLOCK_SIZE));
    return b.CreateBitCast(b.CreateLShr(i128_val, BLOCK_SIZE - 1, namehint), mBitBlockType);
}

Value* PabloCompiler::genShiftLeft64(Value* e, const Twine &namehint) {
    IRBuilder<> b(mBasicBlock);
    Value* i128_val = b.CreateBitCast(e, IntegerType::get(mMod->getContext(), BLOCK_SIZE));
    return b.CreateBitCast(b.CreateShl(i128_val, 64, namehint), mBitBlockType);
}

inline Value* PabloCompiler::genNot(Value* expr) {
    IRBuilder<> b(mBasicBlock);
    return b.CreateXor(expr, mOneInitializer, "not");
}

Value* PabloCompiler::genAdvanceWithCarry(Value* strm_value, int shift_amount) {

    IRBuilder<> b(mBasicBlock);

    const auto advanceIdx = mAdvanceQueueIdx++;
#ifdef USE_LONG_INTEGER_SHIFT
    Value* advanceq_longint = b.CreateBitCast(genAdvanceInLoad(advanceIdx), IntegerType::get(mMod->getContext(), BLOCK_SIZE));
    Value* strm_longint = b.CreateBitCast(strm_value, IntegerType::get(mMod->getContext(), BLOCK_SIZE));
    Value* adv_longint = b.CreateOr(b.CreateShl(strm_longint, shift_amount), b.CreateLShr(advanceq_longint, BLOCK_SIZE - shift_amount), "advance");
    Value* result_value = b.CreateBitCast(adv_longint, mBitBlockType);
    genAdvanceOutStore(strm_value, advanceIdx);

    return result_value;
#elif (BLOCK_SIZE == 128)
    if (shift_amount == 1) {
        Value* advanceq_value = genShiftHighbitToLow(genAdvanceInLoad(advanceIdx));
        Value* srli_1_value = b.CreateLShr(strm_value, 63);
        Value* packed_shuffle;
        Constant* const_packed_1_elems [] = {b.getInt32(0), b.getInt32(2)};
        Constant* const_packed_1 = ConstantVector::get(const_packed_1_elems);
        packed_shuffle = b.CreateShuffleVector(advanceq_value, srli_1_value, const_packed_1);

        Constant* const_packed_2_elems[] = {b.getInt64(1), b.getInt64(1)};
        Constant* const_packed_2 = ConstantVector::get(const_packed_2_elems);

        Value* shl_value = b.CreateShl(strm_value, const_packed_2);
        Value* result_value = b.CreateOr(shl_value, packed_shuffle, "advance");

        //CarryQ - carry out:
        genAdvanceOutStore(strm_value, advanceIdx);

        return result_value;
    }
    else if (shift_amount < 64) {
        // This is the preferred logic, but is too slow for the general case.
        // We need to speed up our custom LLVM for this code.
        Value* advanceq_longint = b.CreateBitCast(genAdvanceInLoad(advanceIdx), IntegerType::get(mMod->getContext(), BLOCK_SIZE));
        Value* strm_longint = b.CreateBitCast(strm_value, IntegerType::get(mMod->getContext(), BLOCK_SIZE));
        Value* adv_longint = b.CreateOr(b.CreateShl(strm_longint, shift_amount), b.CreateLShr(advanceq_longint, BLOCK_SIZE - shift_amount), "advance");
        Value* result_value = b.CreateBitCast(adv_longint, mBitBlockType);
        genAdvanceOutStore(strm_value, advanceIdx);

        return result_value;
    }
    else {//if (shift_amount >= 64) {
        throw std::runtime_error("Shift amount >= 64 in Advance is currently unsupported.");
    }
#else 
    //BLOCK_SIZE == 256
    static_assert(false, "Advance with carry on 256-bit bitblock requires long integer shifts (USE_LONG_INTEGER_SHIFT).");
#endif //USE_LONG_INTEGER_SHIFT
}

void PabloCompiler::SetOutputValue(Value * marker, const unsigned index) {
    IRBuilder<> b(mBasicBlock);
    if (marker->getType()->isPointerTy()) {
        marker = b.CreateAlignedLoad(marker, BLOCK_SIZE/8, false);
    }
    Value* indices[] = {b.getInt64(0), b.getInt32(index)};
    Value* gep = b.CreateGEP(mOutputAddrPtr, indices);
    b.CreateAlignedStore(marker, gep, BLOCK_SIZE/8, false);
}

}
