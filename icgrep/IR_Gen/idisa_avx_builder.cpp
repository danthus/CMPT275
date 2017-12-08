/*
 *  Copyright (c) 2015 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#include "idisa_avx_builder.h"

using namespace llvm;

namespace IDISA {
    
std::string IDISA_AVX_Builder::getBuilderUniqueName() {
    return mBitBlockWidth != 256 ? "AVX_" + std::to_string(mBitBlockWidth) : "AVX";
}

Value * IDISA_AVX_Builder::hsimd_signmask(unsigned fw, Value * a) {
    // AVX2 special cases
    if (mBitBlockWidth == 256) {
        if (fw == 64) {
            Value * signmask_f64func = Intrinsic::getDeclaration(getModule(), Intrinsic::x86_avx_movmsk_pd_256);
            Type * bitBlock_f64type = VectorType::get(getDoubleTy(), mBitBlockWidth/64);
            Value * a_as_pd = CreateBitCast(a, bitBlock_f64type);
            return CreateCall(signmask_f64func, a_as_pd);
        } else if (fw == 32) {
            Value * signmask_f32func = Intrinsic::getDeclaration(getModule(), Intrinsic::x86_avx_movmsk_ps_256);
            Type * bitBlock_f32type = VectorType::get(getFloatTy(), mBitBlockWidth/32);
            Value * a_as_ps = CreateBitCast(a, bitBlock_f32type);
            return CreateCall(signmask_f32func, a_as_ps);
        }
    } else if (mBitBlockWidth == 512) {
        if (fw == 64) {
            Type * bitBlock_f32type = VectorType::get(getFloatTy(), mBitBlockWidth / 32);
            Value * a_as_ps = CreateBitCast(a, bitBlock_f32type);
            Constant * indicies[8];
            for (unsigned i = 0; i < 8; i++) {
                indicies[i] = getInt32(2 * i + 1);
            }
            Value * packh = CreateShuffleVector(a_as_ps, UndefValue::get(bitBlock_f32type), ConstantVector::get({indicies, 8}));
            Type * halfBlock_f32type = VectorType::get(getFloatTy(), mBitBlockWidth/64);
            Value * pack_as_ps = CreateBitCast(packh, halfBlock_f32type);
            Value * signmask_f32func = Intrinsic::getDeclaration(getModule(), Intrinsic::x86_avx_movmsk_ps_256);
            return CreateCall(signmask_f32func, pack_as_ps);
        }
    }
    // Otherwise use default SSE logic.
    return IDISA_SSE_Builder::hsimd_signmask(fw, a);
}

std::string IDISA_AVX2_Builder::getBuilderUniqueName() {
    return mBitBlockWidth != 256 ? "AVX2_" + std::to_string(mBitBlockWidth) : "AVX2";
}
    
Value * IDISA_AVX2_Builder::hsimd_packh(unsigned fw, Value * a, Value * b) {
    if (fw <= 64) {        
        Value * aVec = fwCast(fw / 2, a);
        Value * bVec = fwCast(fw / 2, b);
        const auto field_count = 2 * mBitBlockWidth / fw;
        Constant * Idxs[field_count];
        const auto H = (field_count / 2);
        const auto Q = (field_count / 4);
        for (unsigned i = 0; i < Q; i++) {
            Idxs[i] = getInt32(2 * i);
            Idxs[i + Q] = getInt32((2 * i) + 1);
            Idxs[i + H] = getInt32((2 * i) + H);
            Idxs[i + H + Q] = getInt32((2 * i) + 1 + H);
        }
        Value * shufa = CreateShuffleVector(aVec, aVec, ConstantVector::get({Idxs, field_count}));
        Value * shufb = CreateShuffleVector(bVec, bVec, ConstantVector::get({Idxs, field_count}));
        return hsimd_packh(mBitBlockWidth / 2, shufa, shufb);
    }
    // Otherwise use default SSE logic.
    return IDISA_SSE_Builder::hsimd_packh(fw, a, b);
}

Value * IDISA_AVX2_Builder::hsimd_packl(unsigned fw, Value * a, Value * b) {
    if (fw <= 64) {
        Value * aVec = fwCast(fw / 2, a);
        Value * bVec = fwCast(fw / 2, b);
        const auto field_count = 2 * mBitBlockWidth / fw;
        Constant * Idxs[field_count];
        const auto H = (field_count / 2);
        const auto Q = (field_count / 4);
        for (unsigned i = 0; i < Q; i++) {
            Idxs[i] = getInt32(2 * i);
            Idxs[i + Q] = getInt32((2 * i) + 1);
            Idxs[i + H] = getInt32((2 * i) + H);
            Idxs[i + H + Q] = getInt32((2 * i) + H + 1);
        }
        Value * shufa = CreateShuffleVector(aVec, aVec, ConstantVector::get({Idxs, field_count}));
        Value * shufb = CreateShuffleVector(bVec, bVec, ConstantVector::get({Idxs, field_count}));
        return hsimd_packl(mBitBlockWidth / 2, shufa, shufb);
    }
    // Otherwise use default SSE logic.
    return IDISA_SSE_Builder::hsimd_packl(fw, a, b);
}
    
Value * IDISA_AVX2_Builder::esimd_mergeh(unsigned fw, Value * a, Value * b) {
#if LLVM_VERSION_INTEGER < LLVM_6_0_0
    if ((fw == 128) && (mBitBlockWidth == 256)) {
        Value * vperm2i128func = Intrinsic::getDeclaration(getModule(), Intrinsic::x86_avx2_vperm2i128);
        return CreateCall(vperm2i128func, {fwCast(64, a), fwCast(64, b), getInt8(0x31)});
    }
#endif
    // Otherwise use default SSE logic.
    return IDISA_SSE_Builder::esimd_mergeh(fw, a, b);
}

Value * IDISA_AVX2_Builder::esimd_mergel(unsigned fw, Value * a, Value * b) {
#if LLVM_VERSION_INTEGER < LLVM_6_0_0
    if ((fw == 128) && (mBitBlockWidth == 256)) {
        Value * vperm2i128func = Intrinsic::getDeclaration(getModule(), Intrinsic::x86_avx2_vperm2i128);
        return CreateCall(vperm2i128func, {fwCast(64, a), fwCast(64, b), getInt8(0x20)});
    }
#endif
    // Otherwise use default SSE logic.
    return IDISA_SSE_Builder::esimd_mergel(fw, a, b);
}

Value * IDISA_AVX2_Builder::hsimd_packl_in_lanes(unsigned lanes, unsigned fw, Value * a, Value * b) {
    if ((fw == 16)  && (lanes == 2)) {
        Value * vpackuswbfunc = Intrinsic::getDeclaration(getModule(), Intrinsic::x86_avx2_packuswb);
        Value * a_low = fwCast(16, simd_and(a, simd_lomask(fw)));
        Value * b_low = fwCast(16, simd_and(b, simd_lomask(fw)));
        return CreateCall(vpackuswbfunc, {a_low, b_low});
    }
    // Otherwise use default SSE logic.
    return IDISA_SSE_Builder::hsimd_packl_in_lanes(lanes, fw, a, b);
}

Value * IDISA_AVX2_Builder::hsimd_packh_in_lanes(unsigned lanes, unsigned fw, Value * a, Value * b) {
    if ((fw == 16)  && (lanes == 2)) {
        Value * vpackuswbfunc = Intrinsic::getDeclaration(getModule(), Intrinsic::x86_avx2_packuswb);
        Value * a_low = simd_srli(fw, a, fw/2);
        Value * b_low = simd_srli(fw, b, fw/2);
        return CreateCall(vpackuswbfunc, {a_low, b_low});
    }
    // Otherwise use default SSE logic.
    return IDISA_SSE_Builder::hsimd_packh_in_lanes(lanes, fw, a, b);
}
    
std::pair<Value *, Value *> IDISA_AVX2_Builder::bitblock_add_with_carry(Value * e1, Value * e2, Value * carryin) {
    // using LONG_ADD
    Type * carryTy = carryin->getType();
    if (carryTy == mBitBlockType) {
        carryin = mvmd_extract(32, carryin, 0);
    }
    Value * carrygen = simd_and(e1, e2);
    Value * carryprop = simd_or(e1, e2);
    Value * digitsum = simd_add(64, e1, e2);
    Value * digitcarry = simd_or(carrygen, simd_and(carryprop, CreateNot(digitsum)));
    Value * carryMask = hsimd_signmask(64, digitcarry);
    Value * carryMask2 = CreateOr(CreateAdd(carryMask, carryMask), carryin);
    Value * bubble = simd_eq(64, digitsum, allOnes());
    Value * bubbleMask = hsimd_signmask(64, bubble);
    Value * incrementMask = CreateXor(CreateAdd(bubbleMask, carryMask2), bubbleMask);
    Value * increments = esimd_bitspread(64,incrementMask);
    Value * sum = simd_add(64, digitsum, increments);
    Value * carry_out = CreateLShr(incrementMask, mBitBlockWidth / 64);
    if (carryTy == mBitBlockType) {
        carry_out = bitCast(CreateZExt(carry_out, getIntNTy(mBitBlockWidth)));
    }
    return std::pair<Value *, Value *>{carry_out, bitCast(sum)};
}
    
std::pair<Value *, Value *> IDISA_AVX2_Builder::bitblock_indexed_advance(Value * strm, Value * index_strm, Value * shiftIn, unsigned shiftAmount) {
    Value * popcount_f = Intrinsic::getDeclaration(getModule(), Intrinsic::ctpop, getSizeTy());
    Value * PEXT_f = nullptr;
    Value * PDEP_f = nullptr;
    unsigned bitWidth = sizeof(size_t) * 8;
    if (bitWidth == 64) {
        PEXT_f = Intrinsic::getDeclaration(getModule(), Intrinsic::x86_bmi_pext_64);
        PDEP_f = Intrinsic::getDeclaration(getModule(), Intrinsic::x86_bmi_pdep_64);
    }
    else if ((bitWidth == 32)  && (shiftAmount < 32)) {
        PEXT_f = Intrinsic::getDeclaration(getModule(), Intrinsic::x86_bmi_pext_32);
        PDEP_f = Intrinsic::getDeclaration(getModule(), Intrinsic::x86_bmi_pdep_32);
    }
    else {
        llvm::report_fatal_error("indexed_advance unsupported bit width");
    }
    Type * iBitBlock = getIntNTy(getBitBlockWidth());
    Value * shiftVal = getSize(shiftAmount);
    if (LLVM_LIKELY(shiftAmount < bitWidth)) {
        Value * carry = mvmd_extract(bitWidth, shiftIn, 0);
        Value * result = allZeroes();
        for (unsigned i = 0; i < getBitBlockWidth()/bitWidth; i++) {
            Value * s = mvmd_extract(bitWidth, strm, i);
            Value * ix = mvmd_extract(bitWidth, index_strm, i);
            Value * ix_popcnt = CreateCall(popcount_f, {ix});
            Value * bits = CreateCall(PEXT_f, {s, ix});
            Value * adv = CreateOr(CreateShl(bits, shiftAmount), carry);
            // We have two cases depending on whether the popcount of the index pack is < shiftAmount or not.
            Value * popcount_small = CreateICmpULT(ix_popcnt, shiftVal);
            Value * carry_if_popcount_small = 
                CreateOr(CreateShl(bits, CreateSub(shiftVal, ix_popcnt)),
                            CreateLShr(carry, ix_popcnt));
            Value * carry_if_popcount_large = CreateLShr(bits, CreateSub(ix_popcnt, shiftVal));
            carry = CreateSelect(popcount_small, carry_if_popcount_small, carry_if_popcount_large);
            result = mvmd_insert(bitWidth, result, CreateCall(PDEP_f, {adv, ix}), i);
        }
        Value * carryOut = mvmd_insert(bitWidth, allZeroes(), carry, 0);
        return std::pair<Value *, Value *>{bitCast(carryOut), bitCast(result)};
    }
    else if (shiftAmount <= mBitBlockWidth) {
        // The shift amount is always greater than the popcount of the individual
        // elements that we deal with.   This simplifies some of the logic.
        Value * carry = CreateBitCast(shiftIn, iBitBlock);
        Value * result = allZeroes();
        for (unsigned i = 0; i < getBitBlockWidth()/bitWidth; i++) {
            Value * s = mvmd_extract(bitWidth, strm, i);
            Value * ix = mvmd_extract(bitWidth, index_strm, i);
            Value * ix_popcnt = CreateCall(popcount_f, {ix});
            Value * bits = CreateCall(PEXT_f, {s, ix});  // All these bits are shifted out (appended to carry).
            result = mvmd_insert(bitWidth, result, CreateCall(PDEP_f, {mvmd_extract(bitWidth, carry, 0), ix}), i);
            carry = CreateLShr(carry, CreateZExt(ix_popcnt, iBitBlock)); // Remove the carry bits consumed, make room for new bits.
            carry = CreateOr(carry, CreateShl(CreateZExt(bits, iBitBlock), CreateZExt(CreateSub(shiftVal, ix_popcnt), iBitBlock)));
        }
        return std::pair<Value *, Value *>{bitCast(carry), bitCast(result)};
    }
    else {
        // The shift amount is greater than the total popcount.   We will consume popcount
        // bits from the shiftIn value only, and produce a carry out value of the selected bits.
        // elements that we deal with.   This simplifies some of the logic.
        Value * carry = CreateBitCast(shiftIn, iBitBlock);
        Value * result = allZeroes();
        Value * carryOut = CreateBitCast(allZeroes(), iBitBlock);
        Value * generated = getSize(0);
        for (unsigned i = 0; i < getBitBlockWidth()/bitWidth; i++) {
            Value * s = mvmd_extract(bitWidth, strm, i);
            Value * ix = mvmd_extract(bitWidth, index_strm, i);
            Value * ix_popcnt = CreateCall(popcount_f, {ix});
            Value * bits = CreateCall(PEXT_f, {s, ix});  // All these bits are shifted out (appended to carry).
            result = mvmd_insert(bitWidth, result, CreateCall(PDEP_f, {mvmd_extract(bitWidth, carry, 0), ix}), i);
            carry = CreateLShr(carry, CreateZExt(ix_popcnt, iBitBlock)); // Remove the carry bits consumed.
            carryOut = CreateOr(carryOut, CreateShl(CreateZExt(bits, iBitBlock), CreateZExt(generated, iBitBlock)));
            generated = CreateAdd(generated, ix_popcnt);
        }
        return std::pair<Value *, Value *>{bitCast(carryOut), bitCast(result)};
    }
}

}
