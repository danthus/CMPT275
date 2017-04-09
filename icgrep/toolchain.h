/*
 *  Copyright (c) 2017 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#ifndef TOOLCHAIN_H
#define TOOLCHAIN_H
#include <string>
#include <IR_Gen/idisa_builder.h>
#include <llvm/IR/TypeBuilder.h>
#include <boost/container/flat_map.hpp>
#include <object_cache.h>

namespace llvm { class ExecutionEngine; }
namespace llvm { class Module; }
namespace llvm { class TargetMachine; }
namespace llvm { namespace cl { class OptionCategory; } }
namespace IDISA { class IDISA_Builder; }
namespace kernel { class KernelBuilder; }
namespace parabix { class StreamSetBuffer; }

namespace codegen {
const llvm::cl::OptionCategory * codegen_flags();

// Command Parameters
enum DebugFlags {
    ShowIR,
#if LLVM_VERSION_MINOR > 6
    ShowASM,
#endif
    SerializeThreads
};

bool DebugOptionIsSet(DebugFlags flag);


extern char OptLevel;  // set from command line
extern int BlockSize;  // set from command line
extern int SegmentSize;  // set from command line
extern int BufferSegments;
extern int ThreadNum;
extern bool EnableAsserts;
#ifdef CUDA_ENABLED
extern bool NVPTX;
extern int GroupNum;
#endif
}

#ifdef CUDA_ENABLED
void setNVPTXOption();
void Compile2PTX (llvm::Module * m, std::string IRFilename, std::string PTXFilename);
#endif

void AddParabixVersionPrinter();

bool AVX2_available();

llvm::ExecutionEngine * JIT_to_ExecutionEngine (llvm::Module * m);

void ApplyObjectCache(llvm::ExecutionEngine * e);

void generatePipeline(IDISA::IDISA_Builder * iBuilder, const std::vector<kernel::KernelBuilder *> & kernels);


class ParabixDriver {

    using ModuleMap = boost::container::flat_map<kernel::KernelBuilder *, std::unique_ptr<llvm::Module>>;

public:
    ParabixDriver(IDISA::IDISA_Builder * iBuilder);
    
    IDISA::IDISA_Builder * getIDISA_Builder() {return iBuilder;}
    
    void JITcompileMain ();

    void addKernelCall(kernel::KernelBuilder & kb, const std::vector<parabix::StreamSetBuffer *> & inputs, const std::vector<parabix::StreamSetBuffer *> & outputs);
    
    void generatePipelineIR();
    
    template <typename ExternalFunctionType>
    void addExternalLink(kernel::KernelBuilder & kb, llvm::StringRef name, ExternalFunctionType * functionPtr) const;

    void addExternalLink(kernel::KernelBuilder & kb, llvm::StringRef name, llvm::FunctionType * type, void * functionPtr) const;

    void linkAndFinalize();
    
    void * getPointerToMain();

private:
    IDISA::IDISA_Builder * const            iBuilder;
    llvm::Module * const                    mMainModule;
    llvm::TargetMachine *                   mTarget;
    llvm::ExecutionEngine *                 mEngine;
    std::unique_ptr<ParabixObjectCache>     mCache;
    std::vector<kernel::KernelBuilder *>    mKernelList;
    ModuleMap                               mModuleMap;
};

namespace {

// NOTE: Currently, LLVM TypeBuilder can deduce FuntionTypes for only up to 5 arguments. The following
// templates have no limit but should be deprecated if the TypeBuilder ever supports n-ary functions.

template<unsigned i, typename... Args>
struct ParameterTypeBuilder;

template<unsigned i, typename A1, typename... An>
struct ParameterTypeBuilder<i, A1, An...> {
    static void get(llvm::LLVMContext & C, llvm::Type ** params) {
        ParameterTypeBuilder<i, A1>::get(C, params);
        ParameterTypeBuilder<i + 1, An...>::get(C, params);
    }
};

template<unsigned i, typename A>
struct ParameterTypeBuilder<i, A> {
    static void get(llvm::LLVMContext & C, llvm::Type ** params) {
        params[i] = llvm::TypeBuilder<A, false>::get(C);
    }
};

template<typename T>
struct FunctionTypeBuilder;

template<typename R, typename... Args>
struct FunctionTypeBuilder<R(Args...)> {
    static llvm::FunctionType * get(llvm::LLVMContext & C) {
        llvm::Type * params[sizeof...(Args)];
        ParameterTypeBuilder<0, Args...>::get(C, params);
        return llvm::FunctionType::get(llvm::TypeBuilder<R, false>::get(C), params, false);
    }
};

template<typename R>
struct FunctionTypeBuilder<R()> {
    static llvm::FunctionType * get(llvm::LLVMContext & C) {
        return llvm::FunctionType::get(llvm::TypeBuilder<R, false>::get(C), false);
    }
};

}

template <typename ExternalFunctionType>
void ParabixDriver::addExternalLink(kernel::KernelBuilder & kb, llvm::StringRef name, ExternalFunctionType * functionPtr) const {
    llvm::FunctionType * const type = FunctionTypeBuilder<ExternalFunctionType>::get(iBuilder->getContext());
    assert ("FunctionTypeBuilder did not resolve a function type." && type);
    addExternalLink(kb, name, type, reinterpret_cast<void *>(functionPtr));
}

#endif
