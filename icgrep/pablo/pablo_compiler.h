/*
 *  Copyright (c) 2016 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#ifndef PABLO_COMPILER_H
#define PABLO_COMPILER_H


#include <string>
#include <list>
#include <vector>
#include <map>
#include <algorithm>
#include <unordered_map>
#include <pablo/pe_string.h>
#include <pablo/carry_manager.h>
#include <llvm/ADT/Twine.h>
#include <llvm/IR/IRBuilder.h>
#include <IDISA/idisa_builder.h>
#include <kernels/kernel.h>

namespace llvm {
    class Value;
    class Module;
    class ExecutionEngine;
    class VectorType;
    class PointerType;
    class ConstantAggregateZero;
    class Constant;
    class FunctionType;
    class Function;
    class BasicBlock;
}

namespace pablo {

using namespace llvm;

class PabloAST;
class PabloBlock;
class PabloFunction;
class String;
class Var;
class Statement;
class StatementList;
class If;
class While;

class PabloCompiler {

    typedef std::unordered_map<const pablo::PabloAST *, Value *>   ASTToValueMap;

public:
    PabloCompiler(Module * m, IDISA::IDISA_Builder * b);
    ~PabloCompiler();
    Function * compile(pablo::PabloFunction * function);   
    void setKernel(KernelBuilder * kBuilder);
    
private:
    void GenerateFunction(PabloFunction & function);
    void Examine(PabloFunction & function);
    void Examine(PabloBlock * block);

    void SetOutputValue(Value * marker, const unsigned index);

    void compileBlock(PabloBlock * block);
    void compileStatement(const Statement * stmt);
    void compileIf(const If * ifStmt);
    void compileWhile(const While * whileStmt);
    Value* compileExpression(const PabloAST * expr);
    void GenerateKernel(PabloBlock * block, PabloFunction * function);

    ASTToValueMap                       mMarkerMap;

    Module *                            mMod;
    IDISA::IDISA_Builder *              iBuilder;
    Type* const                         mBitBlockType;

    CarryManager *                      mCarryManager;

    PointerType*                        mInputType;

    PabloBlock *                        mPabloBlock;

    KernelBuilder *                     mKBuilder;
    
    unsigned                            mWhileDepth;
    unsigned                            mIfDepth;

    Function *                          mFunction;
    Value *                             mInputAddressPtr;
    Value *                             mOutputAddressPtr;

    unsigned                            mMaxWhileDepth;
    int                                 mFilePosIdx;

};

}

#endif // PABLO_COMPILER_H
