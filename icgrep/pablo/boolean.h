#ifndef BOOLEAN_H
#define BOOLEAN_H

#include <pablo/pabloAST.h>

namespace pablo {

class And : public Variadic {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::And;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~And() { }
protected:
    And(Type * const type, PabloAST * expr1, PabloAST * expr2, const String * name)
    : Variadic(ClassTypeId::And, type, {expr1, expr2}, name)
    {

    }
    And(Type * const type, const unsigned reserved, const String * name)
    : Variadic(ClassTypeId::And, type, reserved, name)
    {

    }
    template<typename iterator>
    And(Type * const type, iterator begin, iterator end, const String * name)
    : Variadic(ClassTypeId::And, type, begin, end, name) {

    }
};

class Or : public Variadic {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::Or;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~Or() { }
protected:
    Or(Type * const type, PabloAST * expr1, PabloAST * expr2, const String * name)
    : Variadic(ClassTypeId::Or, type, {expr1, expr2}, name)
    {

    }
    Or(Type * const type, const unsigned reserved, const String * name)
    : Variadic(ClassTypeId::Or, type, reserved, name)
    {

    }
    template<typename iterator>
    Or(Type * const type, iterator begin, iterator end, const String * name)
    : Variadic(ClassTypeId::Or, type, begin, end, name) {

    }
};

class Xor : public Variadic {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::Xor;
    }
    static inline bool classof(const void *) {
        return false;
    }
protected:
    Xor(Type * const type, PabloAST * expr1, PabloAST * expr2, const String * name)
    : Variadic(ClassTypeId::Xor, type, {expr1, expr2}, name)
    {

    }
    Xor(Type * const type, const unsigned reserved, const String * name)
    : Variadic(ClassTypeId::Xor, type, reserved, name)
    {

    }
    template<typename iterator>
    Xor(Type * const type, iterator begin, iterator end, const String * name)
    : Variadic(ClassTypeId::Xor, type, begin, end, name) {

    }
};

class Not : public Statement {
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
        return getOperand(0);
    }
protected:
    Not(PabloAST * expr, const String * name)
    : Statement(ClassTypeId::Not, expr->getType(), {expr}, name)
    {

    }
};

class Sel : public Statement {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::Sel;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~Sel() {
    }
    inline PabloAST * getCondition() const {
        return getOperand(0);
    }
    inline PabloAST * getTrueExpr() const {
        return getOperand(1);
    }
    inline PabloAST * getFalseExpr() const {
        return getOperand(2);
    }
protected:
    Sel(PabloAST * condExpr, PabloAST * trueExpr, PabloAST * falseExpr, const String * name)
    : Statement(ClassTypeId::Sel, trueExpr->getType(), {condExpr, trueExpr, falseExpr}, name) {

    }
};

}

#endif // BOOLEAN_H