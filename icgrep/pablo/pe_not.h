/*
 *  Copyright (c) 2014 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#ifndef PE_NOT_H
#define PE_NOT_H

#include <pablo/pabloAST.h>

namespace pablo {

class PabloBlock;

class Not : public Statement {
    friend struct OptimizeNot;
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::Not;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~Not() {
    }
    PabloAST * getExpr() const {
        return mOperand[0];
    }
protected:
    Not(PabloAST * expr, PabloBlock * parent);
};

struct OptimizeNot {
    PabloAST * operator()(PabloAST * expr, PabloBlock * pb);
};

}

#endif // PE_NOT_H


