/*
 *  Copyright (c) 2017 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 */
#ifndef CHARCLASSES_H
#define CHARCLASSES_H

#include <pablo/pablo_kernel.h>  // for PabloKernel
#include <re/alphabet/alphabet.h>

namespace kernel { class KernelBuilder; }
namespace IDISA { class IDISA_Builder; }
namespace re { class RE; class CC; }
namespace kernel {

struct CharClassesSignature {
    CharClassesSignature(const std::vector<re::CC *> & ccs, bool useDirectCC);
protected:
    const bool mUseDirectCC;
    const std::string mSignature;
};

class CharClassesKernel : public CharClassesSignature, public pablo::PabloKernel {
public:
    CharClassesKernel(BuilderRef b, std::vector<re::CC *> && ccs, StreamSet * BasisBits, StreamSet * CharClasses);
    bool hasSignature() const override { return true; }
    std::string makeSignature(BuilderRef) const override;
    bool isCachable() const override { return true; }
protected:
    void generatePabloMethod() override;
protected:
    std::vector<re::CC *> mCCs;
};


class ByteClassesKernel: public CharClassesSignature, public pablo::PabloKernel {
public:
    ByteClassesKernel(BuilderRef b, std::vector<re::CC *> && ccs, StreamSet * inputStream, StreamSet * CharClasses);
    bool hasSignature() const override { return true; }
    std::string makeSignature(BuilderRef) const override;
    bool isCachable() const override { return true; }
protected:
    void generatePabloMethod() override;
protected:
    std::vector<re::CC *> mCCs;
};


}
#endif
