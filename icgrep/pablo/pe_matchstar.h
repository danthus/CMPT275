/*
 *  Copyright (c) 2014 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#ifndef PE_MATCHSTAR_H
#define PE_MATCHSTAR_H

#include <pablo/pabloAST.h>

namespace pablo {

class MatchStar : public Statement {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::MatchStar;
    }
    static inline bool classof(const void *) {
        return false;
    }
    inline PabloAST * getMarker() const {
        return getOperand(0);
    }
    inline PabloAST * getCharClass() const  {
        return getOperand(1);
    }
    virtual ~MatchStar() {}
protected:
    MatchStar(PabloAST * marker, PabloAST * cc, String * name, PabloBlock * parent)
    : Statement(ClassTypeId::MatchStar, {marker, cc}, name, parent)
    {

    }
};

}

#endif // PE_MATCHSTAR_H



