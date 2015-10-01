/*
 *  Copyright (c) 2015 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#include <string>
#include <iostream>
#include <fstream>

#include "basis_bits.h"
#include "utf_encoding.h"
#include "pablo/pablo_compiler.h"
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Host.h>

#ifndef DISABLE_PREGENERATED_UCD_FUNCTIONS
#include <UCD/precompiled_properties.h>
#endif
#include <re/re_cc.h>
#include <re/re_nullable.h>
#include <re/re_simplifier.h>
#include <re/re_alt.h>
#include <re/parsefailure.h>
#include <re/re_parser.h>
#include <re/re_compiler.h>
#include <utf8_encoder.h>
#include <cc/cc_compiler.h>
#include <cc/cc_namemap.hpp>
#include <pablo/pablo_compiler.h>
#include <pablo/optimizers/pablo_simplifier.hpp>
#include <pablo/optimizers/pablo_codesinking.hpp>
#ifdef ENABLE_MULTIPLEXING
#include <pablo/optimizers/pablo_automultiplexing.hpp>
#include <pablo/optimizers/pablo_bddminimization.h>
#include <pablo/optimizers/booleanreassociationpass.h>
#endif
#include <pablo/function.h>
#include <pablo/analysis/pabloverifier.hpp>
#include <re/printer_re.h>
#include <pablo/printer_pablos.h>

#include "do_grep.h"

static cl::OptionCategory cRegexOutputOptions("Regex Dump Options",
                                              "These options control printing of intermediate regular expression structures.");
static cl::opt<bool> PrintAllREs("print-REs", cl::init(false), cl::desc("print regular expression passes"), cl::cat(cRegexOutputOptions));
static cl::opt<bool> PrintParsedREs("print-parsed-REs", cl::init(false), cl::desc("print out parsed regular expressions"), cl::cat(cRegexOutputOptions));
static cl::opt<bool> PrintStrippedREs("print-stripped-REs", cl::init(false), cl::desc("print out REs with nullable prefixes/suffixes removed"), cl::cat(cRegexOutputOptions));
static cl::opt<bool> PrintNamedREs("print-named-REs", cl::init(false), cl::desc("print out named REs"), cl::cat(cRegexOutputOptions));
static cl::opt<bool> PrintUTF8REs("print-utf8-REs", cl::init(false), cl::desc("print out UTF-8 REs"), cl::cat(cRegexOutputOptions));
static cl::opt<bool> PrintSimplifiedREs("print-simplified-REs", cl::init(false), cl::desc("print out final simplified REs"), cl::cat(cRegexOutputOptions));
static cl::OptionCategory dPabloDumpOptions("Pablo Dump Options",
                                            "These options control printing of intermediate Pablo code.");

static cl::opt<bool> PrintOptimizedREcode("print-pablo", cl::init(false), cl::desc("print final optimized Pablo code"), cl::cat(dPabloDumpOptions));
static cl::opt<bool> PrintCompiledCCcode("print-CC-pablo", cl::init(false), cl::desc("print Pablo output from character class compiler"), cl::cat(dPabloDumpOptions));
static cl::opt<bool> PrintCompiledREcode("print-RE-pablo", cl::init(false), cl::desc("print Pablo output from the regular expression compiler"), cl::cat(dPabloDumpOptions));

static cl::OptionCategory cPabloOptimizationsOptions("Pablo Optimizations", "These options control Pablo optimization passes.");

static cl::opt<bool> DisablePabloCSE("disable-CSE", cl::init(false),
                                     cl::desc("Disable Pablo common subexpression elimination/dead code elimination"),
                                     cl::cat(cPabloOptimizationsOptions));
static cl::opt<bool> PabloSinkingPass("sinking", cl::init(false),
                                      cl::desc("Moves all instructions into the innermost legal If-scope so that they are only executed when needed."),
                                      cl::cat(cPabloOptimizationsOptions));

#ifdef ENABLE_MULTIPLEXING
static cl::opt<bool> EnableMultiplexing("multiplexing", cl::init(false),
                                        cl::desc("combine Advances whose inputs are mutual exclusive into the fewest number of advances possible (expensive)."),
                                        cl::cat(cPabloOptimizationsOptions));

static cl::opt<bool> EnableReassociation("reassoc", cl::init(false),
                                         cl::desc("perform reassocation and distribution law optimization."),
                                         cl::cat(cPabloOptimizationsOptions));
#endif

static cl::opt<bool> UseAVX2("use-AVX2", cl::init(false), cl::desc("execute with AVX2 instruction set."), cl::cat(cPabloOptimizationsOptions));

re::RE * regular_expression_passes(const Encoding encoding, re::RE * re_ast)  {
    if (PrintAllREs || PrintParsedREs) {
        std::cerr << "Parser:" << std::endl << Printer_RE::PrintRE(re_ast) << std::endl;
    }

    //Optimization passes to simplify the AST.
    re_ast = re::RE_Nullable::removeNullablePrefix(re_ast);
    if (PrintAllREs || PrintStrippedREs) {
        std::cerr << "RemoveNullablePrefix:" << std::endl << Printer_RE::PrintRE(re_ast) << std::endl;
    }
    re_ast = re::RE_Nullable::removeNullableSuffix(re_ast);
    if (PrintAllREs || PrintStrippedREs) {
        std::cerr << "RemoveNullableSuffix:" << std::endl << Printer_RE::PrintRE(re_ast) << std::endl;
    }
    
    cc::CC_NameMap nameMap;
    re_ast = nameMap.process(re_ast, re::UnicodeClass);
    
    // std::cerr << "-----------------------------" << std::endl;
    
    if (PrintAllREs || PrintNamedREs) {
        std::cerr << "Namer:" << std::endl << Printer_RE::PrintRE(re_ast) << std::endl;
        std::cerr << "NameMap:\n" << nameMap.printMap() << std::endl;
    }
    
    //Add the UTF encoding.
    if (encoding.getType() == Encoding::Type::UTF_8) {
        re_ast = cc::UTF8_Encoder::toUTF8(nameMap, re_ast);
        if (PrintAllREs || PrintUTF8REs) {
            //Print to the terminal the AST that was generated by the utf8 encoder.
            std::cerr << "UTF8-encoder:" << std::endl << Printer_RE::PrintRE(re_ast) << std::endl;
            std::cerr << "NameMap:\n" << nameMap.printMap() << std::endl;
        }
    }
    
    re_ast = re::RE_Simplifier::simplify(re_ast);
    if (PrintAllREs || PrintSimplifiedREs) {
        //Print to the terminal the AST that was generated by the simplifier.
        std::cerr << "Simplifier:" << std::endl << Printer_RE::PrintRE(re_ast) << std::endl;
    }
    return re_ast;
}
    
pablo::PabloFunction * re2pablo_compiler(const Encoding encoding, re::RE * re_ast) {
    
    pablo::PabloFunction * function = pablo::PabloFunction::Create("process_block", 8, 2);
    
    cc::CC_Compiler cc_compiler(*function, encoding);
    
    cc_compiler.compileByteClasses(re_ast);
    
    if (PrintCompiledCCcode) {
        //Print to the terminal the AST that was generated by the character class compiler.
        llvm::raw_os_ostream cerr(std::cerr);
        cerr << "CC AST:" << "\n";
        PabloPrinter::print(function->getEntryBlock().statements(), cerr);
    }

    re::RE_Compiler re_compiler(*function, cc_compiler);
    re_compiler.initializeRequiredStreams();
    re_compiler.compileUnicodeNames(re_ast);
    re_compiler.finalizeMatchResult(re_compiler.compile(re_ast));

    if (PrintCompiledREcode) {
        //Print to the terminal the AST that was generated by the pararallel bit-stream compiler.
        llvm::raw_os_ostream cerr(std::cerr);
        cerr << "Initial Pablo AST:\n";
        PabloPrinter::print(function->getEntryBlock().statements(), cerr);
    }
#ifndef NDEBUG
    pablo::PabloVerifier::verify(*function, "creation");
#endif
    return function;
}

void pablo_function_passes(pablo::PabloFunction * function) {
    // Scan through the pablo code and perform DCE and CSE
    if (!DisablePabloCSE) {
        pablo::Simplifier::optimize(*function);
    }
    if (PabloSinkingPass) {
        pablo::CodeSinking::optimize(*function);
    }
#ifdef ENABLE_MULTIPLEXING    
    if (EnableMultiplexing) {
        pablo::BDDMinimizationPass::optimize(*function);
        pablo::AutoMultiplexing::optimize(*function);        
    }    
    if (EnableReassociation) {
        pablo::BooleanReassociationPass::optimize(*function);
    }
#endif
    if (PrintOptimizedREcode) {
        //Print to the terminal the AST that was generated by the pararallel bit-stream compiler.
        llvm::raw_os_ostream cerr(std::cerr);
        cerr << "Final Pablo AST:\n";
        PabloPrinter::print(function->getEntryBlock().statements(), cerr);
    }
}



ExecutionEngine * JIT_to_ExecutionEngine (llvm::Function * f) {

    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    std::string errMessage;
    EngineBuilder builder(std::move(std::unique_ptr<Module>(f->getParent())));
    builder.setErrorStr(&errMessage);
    builder.setMCPU(sys::getHostCPUName());
    builder.setOptLevel(CodeGenOpt::Level::None);
#if (BLOCK_SIZE == 256)
    if(UseAVX2){
            std::vector<std::string> attrs;
            attrs.push_back("avx2");
            builder.setMAttrs(attrs);
    }
#endif
    //builder.setOptLevel(mMaxWhileDepth ? CodeGenOpt::Level::Less : CodeGenOpt::Level::None);
    ExecutionEngine * engine = builder.create();
    if (engine == nullptr) {
        throw std::runtime_error("Could not create ExecutionEngine: " + errMessage);
    }
    //engine->addGlobalMapping(cast<GlobalValue>(mPrintRegisterFunction), (void *)&wrapped_print_register);
    // engine->addGlobalMapping(externalFunction, proto->getFunctionPtr());

    return engine;
}


extern "C" {
  void wrapped_print_register(char * regName, BitBlock bit_block) {
      print_register<BitBlock>(regName, bit_block);
  }
}

void icgrep_Linking(Module * m, ExecutionEngine * e) {
    Module::FunctionListType & fns = m->getFunctionList();
    for (Module::FunctionListType::iterator it = fns.begin(), it_end = fns.end(); it != it_end; ++it) {
        std::string fnName = it->getName().str();
        if (fnName == "process_block") continue;
        if (fnName == "wrapped_print_register") {
            e->addGlobalMapping(cast<GlobalValue>(it), (void *)&wrapped_print_register);
        }
#ifndef DISABLE_PREGENERATED_UCD_FUNCTIONS
        else {
            const UCD::ExternalProperty & ep = UCD::resolveExternalProperty(fnName);
            e->addGlobalMapping(cast<GlobalValue>(it), std::get<0>(ep));
        }
#endif
    }
}
