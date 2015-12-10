#ifndef FACTORIZEDFG_H
#define FACTORIZEDFG_H

#include <boost/container/flat_map.hpp>
#include <vector>

namespace pablo {

class PabloFunction;
class PabloBlock;
class Variadic;
class Not;
class Statement;
class PabloAST;

class FactorizeDFG {
    using ScopeDepth = boost::container::flat_map<const PabloBlock *, unsigned>;
    using VertexSet = std::vector<PabloAST *>;
public:
    static void transform(PabloFunction & function);
protected:    
    void initialize(const PabloFunction & function);
    void initialize(const PabloBlock * const block, const unsigned depth);
    void CSE(PabloBlock * const block);
    void CSE(Variadic * const var);
    PabloBlock * chooseInsertionScope(const VertexSet & users);
    void findInsertionPoint(const VertexSet & operands, PabloBlock * const scope);
    void finalize(PabloBlock * const block);
    Statement * finalize(Variadic * const var, PabloBlock * block);
    FactorizeDFG() = default;
private:
    ScopeDepth  mScopeDepth;
};

}

#endif // FACTORIZEDFG_H
