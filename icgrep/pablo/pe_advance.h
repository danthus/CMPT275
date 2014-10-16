/*
 *  Copyright (c) 2014 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#ifndef PE_ADVANCE_H
#define PE_ADVANCE_H

#include "pabloAST.h"

namespace pablo {

class Advance : public PabloAST {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::Advance;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~Advance() {
    }
    inline PabloAST * getExpr() const {
        return mExpr;
    }
protected:
    Advance(PabloAST * expr)
    : PabloAST(ClassTypeId::Advance)
    , mExpr(expr) {

    }
private:
    PabloAST * const mExpr;
};

}

#endif // PE_ADVANCE_H



