/*
 *  Copyright (c) 2017 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 */

#include "evenodd.h"
#include <IR_Gen/idisa_builder.h>  // for IDISA_Builder

using namespace llvm;

namespace kernel {

void EvenOddKernel::generateDoBlockMethod() {
    Value * even = iBuilder->simd_fill(64, iBuilder->getInt64(0x5555555555555555));
    Value * odd = iBuilder->bitCast(iBuilder->simd_fill(8, iBuilder->getInt8(0xAA)));
    storeOutputStreamBlock("even_odd", iBuilder->getInt32(0), even);
    storeOutputStreamBlock("even_odd", iBuilder->getInt32(1), odd);
}

EvenOddKernel::EvenOddKernel(IDISA::IDISA_Builder * builder)
: BlockOrientedKernel(builder, "EvenOdd", {Binding{builder->getStreamSetTy(8, 1), "BasisBits"}}, {Binding{builder->getStreamSetTy(2, 1), "even_odd"}}, {}, {}, {}) {
    setNoTerminateAttribute(true);
    setDoBlockUpdatesProducedItemCountsAttribute(false);

}

}