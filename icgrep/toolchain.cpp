/*
 *  Copyright (c) 2015 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#include <string>
#include <iostream>
#include <fstream>
#include <sstream>

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/CodeGen/CommandFlags.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/raw_ostream.h>

// Dynamic processor detection
#define ISPC_LLVM_VERSION ISPC_LLVM_3_6
#include <util/ispc.cpp>

#include <re/re_cc.h>
#include <object_cache.h>

static cl::OptionCategory bGrepOutputOptions("Output Options",
                                      "These options control the output.");

static cl::opt<bool> CountOnly("c", cl::desc("Count and display the matching lines per file only."), cl::cat(bGrepOutputOptions));
static cl::alias CountOnlyLong("count", cl::desc("Alias for -c"), cl::aliasopt(CountOnly));
static cl::opt<bool> NormalizeLineBreaks("normalize-line-breaks", cl::desc("Normalize line breaks to std::endl."), cl::init(false),  cl::cat(bGrepOutputOptions));

static cl::opt<bool> ShowFileNames("H", cl::desc("Show the file name with each matching line."), cl::cat(bGrepOutputOptions));
static cl::alias ShowFileNamesLong("with-filename", cl::desc("Alias for -H"), cl::aliasopt(ShowFileNames));

static cl::opt<bool> ShowLineNumbers("n", cl::desc("Show the line number with each matching line."), cl::cat(bGrepOutputOptions));
static cl::alias ShowLineNumbersLong("line-number", cl::desc("Alias for -n"), cl::aliasopt(ShowLineNumbers));



static cl::OptionCategory cMachineCodeOptimization("Machine Code Optimizations", "These options control back-end compilier optimization levels.");


static cl::opt<char> OptLevel("O", cl::desc("Optimization level. [-O0, -O1, -O2, or -O3] (default = '-O0')"),
                              cl::cat(cMachineCodeOptimization), cl::Prefix, cl::ZeroOrMore, cl::init('0'));


static cl::OptionCategory cObjectCache("Object Caching", "These options control back-end object caching behaviours.");

static cl::opt<bool> EnableObjectCache("enable-object-cache", cl::init(false), cl::desc("Enable object caching"), cl::cat(cObjectCache));

static cl::opt<std::string> ObjectCacheDir("object-cache-dir", cl::init(""), cl::desc("Path to the object cache diretory"), cl::cat(cObjectCache));



ExecutionEngine * JIT_to_ExecutionEngine (Module * m) {

    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    PassRegistry * Registry = PassRegistry::getPassRegistry();
    initializeCore(*Registry);
    initializeCodeGen(*Registry);
    initializeLowerIntrinsicsPass(*Registry);

    std::string errMessage;
    EngineBuilder builder(std::move(std::unique_ptr<Module>(m)));
    builder.setErrorStr(&errMessage);
    builder.setMCPU(sys::getHostCPUName());
    CodeGenOpt::Level optLevel = CodeGenOpt::Level::None;
    switch (OptLevel) {
        case '0': optLevel = CodeGenOpt::None; break;
        case '1': optLevel = CodeGenOpt::Less; break;
        case '2': optLevel = CodeGenOpt::Default; break;
        case '3': optLevel = CodeGenOpt::Aggressive; break;
        default: errs() << OptLevel << " is an invalid optimization level.\n";
    }
    builder.setOptLevel(optLevel);

    if ((strncmp(lGetSystemISA(), "avx2", 4) == 0)) {
            std::vector<std::string> attrs;
            attrs.push_back("avx2");
            builder.setMAttrs(attrs);
    }

    // builder.selectTarget();

    //builder.setOptLevel(mMaxWhileDepth ? CodeGenOpt::Level::Less : CodeGenOpt::Level::None);
    ExecutionEngine * engine = builder.create();
    ICGrepObjectCache * cache = nullptr;
    if (engine == nullptr) {
        throw std::runtime_error("Could not create ExecutionEngine: " + errMessage);
    }
    if (EnableObjectCache) {
        if (ObjectCacheDir.empty())
            // Default is $HOME/.cache/icgrep
            cache = new ICGrepObjectCache();
        else
            cache = new ICGrepObjectCache(ObjectCacheDir);
        engine->setObjectCache(cache);
    }
    return engine;
}



static int * total_count;
static std::stringstream * resultStrs = nullptr;
static std::vector<std::string> inputFiles;

void initResult(std::vector<std::string> filenames, const int n){

    inputFiles = filenames;
    resultStrs = new std::stringstream[n];
    total_count = new int[n];
    for (int i=1; i<inputFiles.size(); i++){
        total_count[i-1] = 0;
    }

}

extern "C" {
    void wrapped_report_match(uint64_t lineNum, uint64_t line_start, uint64_t line_end, const char * buffer, uint64_t filesize, char * filename) {

        int idx = 0;
        for (int i=1; i<inputFiles.size(); i++){
            if (inputFiles[i] == filename){
                idx = i-1;
                break;
            }
        }

        if(CountOnly){
            total_count[idx]++;
            return;
        }

        if (ShowFileNames) {
            resultStrs[idx] << filename << ':';
        }
        if (ShowLineNumbers) {
            resultStrs[idx] << lineNum << ":";
        }

        if ((buffer[line_start] == 0xA) && (line_start != line_end)) {
            // The line "starts" on the LF of a CRLF.  Really the end of the last line.
            line_start++;
        }
        if (line_end == filesize) {
            // The match position is at end-of-file.   We have a final unterminated line.
            resultStrs[idx].write(&buffer[line_start], line_end - line_start);
            if (NormalizeLineBreaks) {
                resultStrs[idx] << '\n';  // terminate it
            }
            return;
        }
        unsigned char end_byte = (unsigned char)buffer[line_end]; 
        if (NormalizeLineBreaks) {
            if (end_byte == 0x85) {
                // Line terminated with NEL, on the second byte.  Back up 1.
                line_end--;
            } else if (end_byte > 0xD) {
                // Line terminated with PS or LS, on the third byte.  Back up 2.
                line_end -= 2;
            }
            resultStrs[idx].write(&buffer[line_start], line_end - line_start);
            resultStrs[idx] << '\n';
        }
        else{   
            if (end_byte == 0x0D) {
                // Check for line_end on first byte of CRLF;  note that we don't
                // want to access past the end of buffer.
                if ((line_end + 1 < filesize) && (buffer[line_end + 1] == 0x0A)) {
                    // Found CRLF; preserve both bytes.
                    line_end++;
                }
            }
            resultStrs[idx].write(&buffer[line_start], line_end - line_start + 1);
        }
    }
}

void PrintResult(){
    if(CountOnly){
        for (int i=1; i<inputFiles.size(); i++){
            std::cout << total_count[i-1] << std::endl;
        }
        return;
    }

    std::string out;
    for (int i=1; i<inputFiles.size(); i++){
        std::cout << resultStrs[i-1].str();
    }
}

re::CC * parsedCodePointSet;

extern "C" {
    void insert_codepoints(uint64_t lineNum, uint64_t line_start, uint64_t line_end, const char * buffer) {
       re::codepoint_t c = 0;
        ssize_t line_pos = line_start;
        while (isxdigit(buffer[line_pos])) {
            if (isdigit(buffer[line_pos])) {
                c = (c << 4) | (buffer[line_pos] - '0');
            }
            else {
                c = (c << 4) | (tolower(buffer[line_pos]) - 'a' + 10);
            }
            line_pos++;
        }
        assert(((line_pos - line_start) >= 4) && ((line_pos - line_start) <= 6)); // UCD format 4 to 6 hex digits.       
        parsedCodePointSet->insert(c);
    }
}

void setParsedCodePointSet(){
    parsedCodePointSet = re::makeCC();
}

re::CC * getParsedCodePointSet(){
    return parsedCodePointSet;
}

// extern "C" {
//   void wrapped_print_register(char * regName, BitBlock bit_block) {
//       print_register<BitBlock>(regName, bit_block);
//   }
// }

void icgrep_Linking(Module * m, ExecutionEngine * e) {
    Module::FunctionListType & fns = m->getFunctionList();
    for (Module::FunctionListType::iterator it = fns.begin(), it_end = fns.end(); it != it_end; ++it) {
        std::string fnName = it->getName().str();
        if (fnName == "s2p_block") continue;
        if (fnName == "process_block") continue;
        if (fnName == "process_block_initialize_carries") continue;
        
        // if (fnName == "wrapped_print_register") {
        //     e->addGlobalMapping(cast<GlobalValue>(it), (void *)&wrapped_print_register);
        // }
        if (fnName == "wrapped_report_match") {
            e->addGlobalMapping(cast<GlobalValue>(it), (void *)&wrapped_report_match);
        }
        if (fnName == "insert_codepoints") {
            e->addGlobalMapping(cast<GlobalValue>(it), (void *)&insert_codepoints);
        }
#ifndef DISABLE_PREGENERATED_UCD_FUNCTIONS
        else {
            const UCD::ExternalProperty & ep = UCD::resolveExternalProperty(fnName);
            e->addGlobalMapping(cast<GlobalValue>(it), std::get<0>(ep));
        }
#endif
    }
}
