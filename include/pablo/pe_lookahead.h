/*
 *  Copyright (c) 2014 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#ifndef PE_LOOKAHEAD_H
#define PE_LOOKAHEAD_H

#include <pablo/pabloAST.h>
#include <pablo/pe_integer.h>
#include <pablo/pe_var.h>

namespace pablo {

class Lookahead final : public Statement {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::Lookahead;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~Lookahead() {
    }
    inline PabloAST * getExpression() const {
        return getOperand(0);
    }
    inline unsigned getAmount() const {
        return llvm::cast<Integer>(getOperand(1))->value();
    }
protected:
    Lookahead(PabloAST * expr, PabloAST * shiftAmount, const String * name, Allocator & allocator)
    : Statement(ClassTypeId::Lookahead, expr->getType(), {expr, shiftAmount}, name, allocator) {
        assert(llvm::isa<Integer>(shiftAmount) && llvm::cast<Integer>(shiftAmount)->value() >= 0);
    }
};

}

#endif // PE_LOOKAHEAD_H



