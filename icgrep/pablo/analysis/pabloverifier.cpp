#include "pabloverifier.hpp"
#include <pablo/function.h>
#include <pablo/codegenstate.h>
#include <pablo/printer_pablos.h>
#include <iostream>
#include <unordered_set>

namespace pablo {

struct OrderingVerifier {
    OrderingVerifier() : mParent(nullptr) {}
    OrderingVerifier(const OrderingVerifier & parent) : mParent(&parent) {}
    bool count(const PabloAST * expr) const {
        if (mSet.count(expr)) {
            return true;
        } else if (mParent) {
            return mParent->count(expr);
        }
        return false;
    }
    void insert(const PabloAST * expr) {
        mSet.insert(expr);
    }
private:
    const OrderingVerifier * const mParent;
    std::unordered_set<const PabloAST *> mSet;
};

void isTopologicallyOrdered(const PabloBlock & block, const OrderingVerifier & parent, const bool ignoreUnusedStatements) {
    OrderingVerifier ov(parent);
    for (const Statement * stmt : block) {
        if (stmt->getNumUses() == 0 && ignoreUnusedStatements) {
            continue;
        }
        if (LLVM_UNLIKELY(isa<While>(stmt))) {
            isTopologicallyOrdered(cast<While>(stmt)->getBody(), ov, ignoreUnusedStatements);
            for (const Next * var : cast<While>(stmt)->getVariants()) {
                ov.insert(var);
            }
        }
        for (unsigned i = 0; i != stmt->getNumOperands(); ++i) {
            const PabloAST * op = stmt->getOperand(i);
            if ((isa<Statement>(op) || isa<Var>(op)) && ov.count(op) == false) {
                // TODO: make this actually test whether the operand is ever defined,
                // or if it was defined in a scope that cannot be reached?
                std::string tmp;
                raw_string_ostream str(tmp);
                str << "PabloVerifier: function is not topologically ordered! ";
                PabloPrinter::print(stmt->getOperand(i), str);
                str << " was used before definition!";
                throw std::runtime_error(str.str());
            }
        }
        ov.insert(stmt);
        if (LLVM_UNLIKELY(isa<If>(stmt))) {
            isTopologicallyOrdered(cast<If>(stmt)->getBody(), ov, ignoreUnusedStatements);
            for (const Assign * def : cast<If>(stmt)->getDefined()) {
                ov.insert(def);
            }
        }
    }
}

void isTopologicallyOrdered(const PabloFunction & function, const bool ignoreUnusedStatements) {
    OrderingVerifier ov;
    for (unsigned i = 0; i != function.getNumOfParameters(); ++i) {
        ov.insert(function.getParameter(i));
    }
    isTopologicallyOrdered(function.getEntryBlock(), ov, ignoreUnusedStatements);
}

void PabloVerifier::verify(const PabloFunction & function, const bool ignoreUnusedStatements) {
    try {
        isTopologicallyOrdered(function, ignoreUnusedStatements);
    } catch(std::runtime_error err) {
        raw_os_ostream out(std::cerr);
        PabloPrinter::print(function.getEntryBlock().statements(), out);
        out.flush();
        throw err;
    }
}

}
