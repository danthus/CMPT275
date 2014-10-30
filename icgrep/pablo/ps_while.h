/*
 *  Copyright (c) 2014 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#ifndef PS_WHILE_H
#define PS_WHILE_H

#include <pablo/pabloAST.h>

namespace pablo {

class While : public Statement {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::While;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~While() {
    }
    inline PabloAST * getCondition() const {
        return mExpr;
    }
    inline StatementList & getBody() {
        return mBody;
    }
    inline const StatementList & getBody() const {
        return mBody;
    }
    inline void setInclusiveCarryCount(const unsigned count) {
        mCarryCount = count;
    }
    inline unsigned getInclusiveCarryCount() const {
        return mCarryCount;
    }
protected:
    void* operator new (std::size_t size) noexcept {
        return mAllocator.allocate(size);
    }
    While(PabloAST * expr, StatementList && body)
    : Statement(ClassTypeId::While)
    , mExpr(expr)
    , mBody(std::move(body))
    , mCarryCount(0)
    {

    }
private:
    PabloAST * const    mExpr;
    StatementList       mBody;
    unsigned            mCarryCount;
};

}

#endif // PS_WHILE_H


