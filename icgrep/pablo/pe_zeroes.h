/*
 *  Copyright (c) 2014 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#ifndef PE_ZEROES_H
#define PE_ZEROES_H

#include "pabloAST.h"

namespace pablo {

class Zeroes : public PabloAST {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::Zeroes;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~Zeroes() {

    }
    virtual PabloAST * getOperand(const unsigned) const {
        assert (false);
        return nullptr;
    }
    virtual unsigned getNumOperands() const {
        return 0;
    }
    virtual void setOperand(const unsigned, PabloAST *) {
        assert (false);
    }
    inline bool operator==(const Zeroes & other) const {
        return true;
    }
    virtual bool operator==(const PabloAST & other) const {
        return isa<Zeroes>(other);
    }
protected:
    void* operator new (std::size_t size) noexcept {
        return mAllocator.allocate(size);
    }
    Zeroes() : PabloAST(ClassTypeId::Zeroes) { }
};

}

#endif // PE_ZEROES_H


