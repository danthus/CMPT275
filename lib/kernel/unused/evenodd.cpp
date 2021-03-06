/*
 *  Copyright (c) 2017 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 */

#include <kernel/util/evenodd.h>
#include <kernel/core/kernel_builder.h>

using namespace llvm;

namespace kernel {

void EvenOddKernel::generateDoBlockMethod(BuilderRef iBuilder) {
    Value * even = iBuilder->simd_fill(64, iBuilder->getInt64(0x5555555555555555));
    Value * odd = iBuilder->bitCast(iBuilder->simd_fill(8, iBuilder->getInt8(0xAA)));
    iBuilder->storeOutputStreamBlock("even_odd", iBuilder->getInt32(0), even);
    iBuilder->storeOutputStreamBlock("even_odd", iBuilder->getInt32(1), odd);
}

EvenOddKernel::EvenOddKernel(BuilderRef b)
: BlockOrientedKernel(b, "EvenOdd", {Binding{b->getStreamSetTy(8, 1), "BasisBits"}}, {Binding{b->getStreamSetTy(2, 1), "even_odd"}}, {}, {}, {}) {

}

}
