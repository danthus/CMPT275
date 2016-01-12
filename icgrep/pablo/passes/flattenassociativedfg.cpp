#include "flattenassociativedfg.h"
#include <pablo/codegenstate.h>
#include <pablo/optimizers/pablo_simplifier.hpp>
#include <pablo/analysis/pabloverifier.hpp>
#include <boost/container/flat_set.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <queue>

using namespace boost;
using namespace boost::container;

namespace pablo {

using TypeId = PabloAST::ClassTypeId;

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief coalesce
 ** ------------------------------------------------------------------------------------------------------------- */
inline void FlattenAssociativeDFG::coalesce(Variadic * const var) {
    const TypeId typeId = var->getClassTypeId();
    for (unsigned i = 0; i < var->getNumOperands(); ) {
        PabloAST * const op = var->getOperand(i);
        if (op->getClassTypeId() == typeId) {
            Variadic * removedVar = cast<Variadic>(var->removeOperand(i));
            for (unsigned j = 0; j != cast<Variadic>(op)->getNumOperands(); ++j) {
                var->addOperand(cast<Variadic>(op)->getOperand(j));
            }
            if (removedVar->getNumOperands() == 1) {
                removedVar->replaceWith(removedVar->getOperand(0));
            } else if (removedVar->getNumUses() == 0) {
                removedVar->eraseFromParent(true);
            }
            continue;
        }
        ++i;
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief coalesce
 ** ------------------------------------------------------------------------------------------------------------- */
void FlattenAssociativeDFG::coalesce(PabloBlock * const block, const bool traverse) {
    Statement * stmt = block->front();
    while (stmt) {
        Statement * next = stmt->getNextNode();
        if (LLVM_UNLIKELY((isa<If>(stmt) || isa<While>(stmt)) && traverse)) {
            coalesce(isa<If>(stmt) ? cast<If>(stmt)->getBody() : cast<While>(stmt)->getBody(), true);
        } else if (isa<And>(stmt) || isa<Or>(stmt) || isa<Xor>(stmt)) {
            coalesce(cast<Variadic>(stmt));
        } else if (isa<Not>(stmt)) {
            deMorgansExpansion(cast<Not>(stmt), block);
        }
        stmt = next;
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief deMorgansExpansion
 *
 * Apply the De Morgans' law to any negated And or Or statement with the intent of further coalescing its operands
 * thereby allowing the Simplifier to check for tautologies and contradictions.
 ** ------------------------------------------------------------------------------------------------------------- */
inline void FlattenAssociativeDFG::deMorgansExpansion(Not * const var, PabloBlock * const block) {
    PabloAST * const negatedVar = var->getOperand(0);
    if (isa<And>(negatedVar) || isa<Or>(negatedVar)) {
        Variadic * src = cast<Variadic>(negatedVar);
        const unsigned operands = src->getNumOperands();
        Variadic * replacement = nullptr;
        block->setInsertPoint(var->getPrevNode());
        if (isa<And>(negatedVar)) {
            replacement = block->createOr(operands);
        } else {
            replacement = block->createAnd(operands);
        }
        block->setInsertPoint(replacement->getPrevNode());
        for (unsigned i = 0; i != operands; ++i) {
            replacement->addOperand(block->createNot(src->getOperand(i)));
        }
        coalesce(replacement);
        var->replaceWith(replacement, true, true);
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief deMorgansReduction
 ** ------------------------------------------------------------------------------------------------------------- */
inline void FlattenAssociativeDFG::deMorgansReduction(Variadic * const var, PabloBlock * const block) {
    unsigned negations = 0;
    for (unsigned i = 0; i < var->getNumOperands(); ++i) {
        if (isa<Not>(var->getOperand(i))) {
            ++negations;
        }
    }
    if (negations > 1) {
        PabloAST * negated[negations];
        for (unsigned i = var->getNumOperands(), j = negations; i && j; ) {
            if (isa<Not>(var->getOperand(--i))) {
                negated[--j] = cast<Not>(var->removeOperand(i))->getOperand(0);
            }
        }
        block->setInsertPoint(var->getPrevNode());
        Variadic * extractedVar = nullptr;
        if (isa<And>(var)) {
            extractedVar = block->createOr(negations);
        } else { // if (isa<Or>(var)) {
            extractedVar = block->createAnd(negations);
        }
        for (unsigned i = 0; i != negations; ++i) {
            extractedVar->addOperand(negated[i]);
        }
        var->addOperand(block->createNot(extractedVar));
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief deMorgansReduction
 ** ------------------------------------------------------------------------------------------------------------- */
void FlattenAssociativeDFG::deMorgansReduction(PabloBlock * const block, const bool traverse) {
    for (Statement * stmt : *block) {
        if (LLVM_UNLIKELY((isa<If>(stmt) || isa<While>(stmt)) && traverse)) {
            deMorgansReduction(isa<If>(stmt) ? cast<If>(stmt)->getBody() : cast<While>(stmt)->getBody(), true);
        } else if (isa<And>(stmt) || isa<Or>(stmt)) {
            deMorgansReduction(cast<Variadic>(stmt), block);
        }
    }
}

union VertexData {
    Assign * def;
    TypeId   typeId;
    explicit VertexData() : def(nullptr) { }
    explicit VertexData(Assign * def) : def(def) { }
    explicit VertexData(TypeId typeId) : typeId(typeId) { }
};
using Graph = adjacency_list<vecS, vecS, bidirectionalS, VertexData, Variadic *>;
using Vertex = Graph::vertex_descriptor;
using SourceMap = flat_map<Assign *, Vertex>;
using SinkMap = flat_map<TypeId, Vertex>;
using VertexSet = std::vector<Vertex>;
using Biclique = std::pair<VertexSet, VertexSet>;
using BicliqueSet = std::vector<Biclique>;

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addToVariadicGraph
 ** ------------------------------------------------------------------------------------------------------------- */
static bool addToVariadicGraph(Assign * const def, Graph & G, SourceMap & A, SinkMap & B) {

    if (LLVM_LIKELY(A.count(def) == 0)) {
        // Test if its valid to transform this statement
        for (PabloAST * user : def->users()) {
            if (isa<Variadic>(user) == 0) {
                if (isa<If>(user)) {
                    if (LLVM_LIKELY(cast<If>(user)->getCondition() != def)) {
                        continue;
                    }
                }
                return false;
            }
        }

        // Add the statement and its users to G
        const Vertex u = add_vertex(VertexData(def), G);
        A.emplace(def, u);
        for (PabloAST * user : def->users()) {
            if (isa<Variadic>(user)) {
                auto f = B.find(user->getClassTypeId());
                if (f == B.end()) {
                    f = B.emplace(user->getClassTypeId(), add_vertex(VertexData(user->getClassTypeId()), G)).first;
                }
                assert (f != B.end());
                G[add_edge(u, f->second, G).first] = cast<Variadic>(user);
            }
        }
    }
    return true;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief matches
 ** ------------------------------------------------------------------------------------------------------------- */
inline static bool matches(const PabloAST * const a, const PabloAST * const b) {
    return (isa<Assign>(b)) && (cast<Assign>(a)->getParent() == cast<Assign>(b)->getParent());
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief enumerateBicliques
 *
 * Adaptation of the MICA algorithm as described in "Consensus algorithms for the generation of all maximal
 * bicliques" by Alexe et. al. (2003). Note: this implementation considers all verticies in set A to be in
 * bipartition A and their adjacencies to be in B.
  ** ------------------------------------------------------------------------------------------------------------- */
static BicliqueSet enumerateBicliques(const Graph & G, const VertexSet & A) {
    using IntersectionSets = std::set<VertexSet>;

    IntersectionSets B1;
    for (auto u : A) {
        flat_set<Vertex> adj;
        adj.reserve(out_degree(u, G));
        for (auto e : make_iterator_range(out_edges(u, G))) {
            adj.insert(target(e, G));
        }
        B1.emplace(adj.begin(), adj.end());
    }

    IntersectionSets B(B1);

    IntersectionSets Bi;

    VertexSet clique;
    for (auto i = B1.begin(); i != B1.end(); ++i) {
        for (auto j = i; ++j != B1.end(); ) {
            std::set_intersection(i->begin(), i->end(), j->begin(), j->end(), std::back_inserter(clique));
            if (clique.size() > 0) {
                if (B.count(clique) == 0) {
                    Bi.insert(clique);
                }
                clique.clear();
            }
        }
    }

    for (;;) {
        if (Bi.empty()) {
            break;
        }
        B.insert(Bi.begin(), Bi.end());
        IntersectionSets Bk;
        for (auto i = B1.begin(); i != B1.end(); ++i) {
            for (auto j = Bi.begin(); j != Bi.end(); ++j) {
                std::set_intersection(i->begin(), i->end(), j->begin(), j->end(), std::back_inserter(clique));
                if (clique.size() > 0) {
                    if (B.count(clique) == 0) {
                        Bk.insert(clique);
                    }
                    clique.clear();
                }
            }
        }
        Bi.swap(Bk);
    }

    BicliqueSet S;
    for (auto Bi = B.begin(); Bi != B.end(); ++Bi) {
        VertexSet Ai(A);
        for (const Vertex u : *Bi) {
            VertexSet Aj;
            Aj.reserve(in_degree(u, G));
            for (auto e : make_iterator_range(in_edges(u, G))) {
                Aj.push_back(source(e, G));
            }
            std::sort(Aj.begin(), Aj.end());
            VertexSet Ak;
            Ak.reserve(std::min(Ai.size(), Aj.size()));
            std::set_intersection(Ai.begin(), Ai.end(), Aj.begin(), Aj.end(), std::back_inserter(Ak));
            Ai.swap(Ak);
        }
        assert (Ai.size() > 0); // cannot happen if this algorithm is working correctly
        // If |Ai| > |Bi|, removing Ai from of the Variadic and sinking it into the nested scope will
        // reduce the number of values stored.
        if (Ai.size() > Bi->size()) {
            S.emplace_back(std::move(Ai), std::move(*Bi));
        }
    }
    return S;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief intersects
 ** ------------------------------------------------------------------------------------------------------------- */
template <class Type>
inline bool intersects(const Type & A, const Type & B) {
    auto first1 = A.begin(), last1 = A.end();
    auto first2 = B.begin(), last2 = B.end();
    while (first1 != last1 && first2 != last2) {
        if (*first1 < *first2) {
            ++first1;
        } else if (*first2 < *first1) {
            ++first2;
        } else {
            return true;
        }
    }
    return false;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief independentCliqueSets
 ** ------------------------------------------------------------------------------------------------------------- */
template <unsigned side>
inline static BicliqueSet independentCliqueSets(BicliqueSet && cliques, const unsigned minimum) {
    using IndependentSetGraph = adjacency_list<hash_setS, vecS, undirectedS, unsigned>;

    const auto l = cliques.size();
    IndependentSetGraph I(l);

    // Initialize our weights
    for (unsigned i = 0; i != l; ++i) {
        I[i] = std::pow(std::get<side>(cliques[i]).size(), 2);
    }

    // Determine our constraints
    for (unsigned i = 0; i != l; ++i) {
        for (unsigned j = i + 1; j != l; ++j) {
            if (intersects(std::get<side>(cliques[i]), std::get<side>(cliques[j]))) {
                add_edge(i, j, I);
            }
        }
    }

    // Use the greedy algorithm to choose our independent set
    VertexSet selected;
    for (;;) {
        unsigned w = 0;
        Vertex u = 0;
        for (unsigned i = 0; i != l; ++i) {
            if (I[i] > w) {
                w = I[i];
                u = i;
            }
        }
        if (w < minimum) break;
        selected.push_back(u);
        I[u] = 0;
        for (auto v : make_iterator_range(adjacent_vertices(u, I))) {
            I[v] = 0;
        }
    }

    // Sort the selected list and then remove the unselected cliques
    std::sort(selected.begin(), selected.end(), std::greater<Vertex>());
    auto end = cliques.end();
    for (const unsigned offset : selected) {
        end = cliques.erase(cliques.begin() + offset + 1, end) - 1;
    }
    cliques.erase(cliques.begin(), end);

    return std::move(cliques);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief tryToPartiallyExtractVariadic
 ** ------------------------------------------------------------------------------------------------------------- */
inline void FlattenAssociativeDFG::tryToPartiallyExtractVariadic(Variadic * const var) {

    for (unsigned i = 0; i < var->getNumOperands(); ++i) {
        PabloAST * const op = var->getOperand(i);
        if (isa<Assign>(op)) {

            // Have we found a variadic operation that can sunk into a nested scope?
            for (unsigned j = i + 1; j != var->getNumOperands(); ++j) {
                bool invalid = false;
                if (LLVM_UNLIKELY(matches(op, var->getOperand(j)))) {
                    Graph G;
                    SourceMap A;
                    SinkMap B;
                    if (addToVariadicGraph(cast<Assign>(op), G, A, B) == 0) {
                        invalid = true;
                        break;
                    }
                    addToVariadicGraph(cast<Assign>(var->getOperand(j)), G, A, B);
                    for (++j; j != var->getNumOperands(); ++j) {
                        if (LLVM_UNLIKELY(matches(op, var->getOperand(j)))) {
                            addToVariadicGraph(cast<Assign>(var->getOperand(j)), G, A, B);
                        }
                    }

                    if (A.size() > 1) {

                        VertexSet H;
                        H.reserve(A.size());
                        for (auto a : A) {
                            H.push_back(a.second);
                        }

                        const auto S = independentCliqueSets<0>(std::move(enumerateBicliques(G, H)), 2);
                        assert (S.size() > 0);
                        for (const Biclique & C : S) {
                            const VertexSet & sources = std::get<0>(C);
                            const VertexSet & variadics = std::get<1>(C);
                            assert (variadics.size() > 0);
                            assert (sources.size() > variadics.size());
                            PabloBlock * const block = cast<Assign>(op)->getParent();
                            block->setInsertPoint(block->back());
                            for (const auto v : variadics) {
                                Variadic * joiner = nullptr;
                                switch (G[v].typeId) {
                                    case TypeId::And:
                                        joiner = block->createAnd(sources.size());
                                        break;
                                    case TypeId::Or:
                                        joiner = block->createOr(sources.size());
                                        break;
                                    case TypeId::Xor:
                                        joiner = block->createXor(sources.size());
                                        break;
                                    default: llvm_unreachable("Unexpected!");
                                }
                                assert (joiner);
                                flat_set<Assign *> defs;
                                for (const auto u : sources) {
                                    defs.insert(G[u].def);
                                }
                                for (Assign * def : defs) {
                                    joiner->addOperand(def->getOperand(0));                                    
                                }

                                coalesce(joiner);
                                Assign * const joined = block->createAssign("m", joiner);

                                for (Assign * def : defs) {
                                    def->replaceWith(joined);
                                    assert (def->getNumUses() == 0);
                                }
                            }
                        }
                        --i;
                    }
                    break;
                }
                if (invalid) {
                    break;
                }
            }
        }
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief tryToPartiallyExtractVariadic
 ** ------------------------------------------------------------------------------------------------------------- */
void FlattenAssociativeDFG::tryToPartiallyExtractVariadic(PabloBlock * const block) {
    for (Statement * stmt = block->back(); stmt; stmt = stmt->getPrevNode()) {
        if (LLVM_UNLIKELY(isa<If>(stmt) || isa<While>(stmt))) {
            tryToPartiallyExtractVariadic(isa<If>(stmt) ? cast<If>(stmt)->getBody() : cast<While>(stmt)->getBody());
        } else if (isa<Variadic>(stmt)) {
            tryToPartiallyExtractVariadic(cast<Variadic>(stmt));
        }
    }
}

using ScopeDependencyGraph = adjacency_list<hash_setS, vecS, bidirectionalS, PabloAST *>;
using ScopeDependencyMap = flat_map<PabloAST *, ScopeDependencyGraph::vertex_descriptor>;

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief find
 ** ------------------------------------------------------------------------------------------------------------- */
inline ScopeDependencyGraph::vertex_descriptor find(PabloAST * expr, ScopeDependencyGraph & G, ScopeDependencyMap & M) {
    auto f = M.find(expr);
    if (f == M.end()) {
        f = M.emplace(expr, add_vertex(expr, G)).first;
    }
    return f->second;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief buildScopeDependencyGraph
 ** ------------------------------------------------------------------------------------------------------------- */
ScopeDependencyGraph::vertex_descriptor buildScopeDependencyGraph(Variadic * const var, ScopeDependencyGraph & G, ScopeDependencyMap & M) {
    auto f = M.find(var);
    if (f != M.end()) {
        return f->second;
    }
    auto u = add_vertex(var, G);
    M.emplace(var, u);
    for (unsigned i = 0; i != var->getNumOperands(); ++i) {
        PabloAST * expr = var->getOperand(i);
        PabloAST * value = var;
        while (isa<Assign>(expr)) {            
            value = expr;
            expr = cast<Assign>(expr)->getExpression();
        }
        if ((expr->getClassTypeId() == var->getClassTypeId()) && (expr->getNumUses() == 1)) {
            const auto v = find(value, G, M);
            add_edge(v, u, G);
            add_edge(buildScopeDependencyGraph(cast<Variadic>(expr), G, M), v, G);
        }
    }
    return u;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief analyzeScopeDependencies
 ** ------------------------------------------------------------------------------------------------------------- */
inline void analyzeScopeDependencies(Assign * const def, ScopeDependencyGraph & G, ScopeDependencyMap & M) {
    if (LLVM_LIKELY(isa<Variadic>(def->getExpression()))) {
        buildScopeDependencyGraph(cast<Variadic>(def->getExpression()), G, M);
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief analyzeScopeDependencies
 ** ------------------------------------------------------------------------------------------------------------- */
void analyzeScopeDependencies(PabloBlock * const block, ScopeDependencyGraph & G, ScopeDependencyMap & M) {
    for (Statement * stmt : *block) {
        if (LLVM_UNLIKELY(isa<If>(stmt) || isa<While>(stmt))) {
            analyzeScopeDependencies(isa<If>(stmt) ? cast<If>(stmt)->getBody() : cast<While>(stmt)->getBody(), G, M);
        } else if (LLVM_UNLIKELY(isa<Assign>(stmt))) {
            analyzeScopeDependencies(cast<Assign>(stmt), G, M);
        }
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief removeDependenciesWithUnresolvedUses
 ** ------------------------------------------------------------------------------------------------------------- */
void removeDependenciesWithUnresolvedUses(ScopeDependencyGraph & G) {
    for (auto u : make_iterator_range(vertices(G))) {
        const PabloAST * const expr = G[u];
        unsigned uses = 0;
        if (isa<Assign>(expr)) {
            for (const PabloAST * user : cast<Assign>(expr)->users()) {
                if (!isa<If>(user) || cast<If>(user)->getCondition() == expr) {
                    ++uses;
                }
            }
        } else {
            uses = expr->getNumUses();
        }
        if (uses != out_degree(u, G)) {
            clear_out_edges(u, G);
        }
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief eliminateUnecessaryDependencies
 ** ------------------------------------------------------------------------------------------------------------- */
void eliminateUnecessaryDependencies(ScopeDependencyGraph & G) {
    using Vertex = ScopeDependencyGraph::vertex_descriptor;
    std::vector<bool> visited(num_vertices(G), false);
    std::queue<Vertex> Q;
    for (auto u : make_iterator_range(vertices(G))) {
        if (out_degree(u, G) == 0 && in_degree(u, G) != 0) {
            Q.push(u);
        }
    }
    while (Q.size() > 0) {
        const auto u = Q.front(); Q.pop();
        visited[u] = true;
        for (auto e : make_iterator_range(in_edges(u, G))) {
            bool usersHaveBeenVisited = true;
            const auto v = source(e, G);
            for (auto e : make_iterator_range(out_edges(v, G))) {
                if (visited[target(e, G)] == 0) {
                    usersHaveBeenVisited = false;
                    break;
                }
            }
            if (usersHaveBeenVisited) {
                Q.push(v);
                for (auto e : make_iterator_range(in_edges(u, G))) {
                    const auto w = source(e, G);
                    if (w != v) {
                        auto f = add_edge(w, v, G);
                        if (f.second == 0) {
                            cast<Variadic>(G[v])->deleteOperand(G[w]);
                        }
                    }
                }
            }
        }
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief removeFalseScopeDependencies
 *
 * After coalescing the AST, we may find that a result of some If statement is added to a result of a subsequent
 * If statement. Unless necessary for correctness, eliminate it as we can potentially schedule the If nodes
 * better without the sequential dependency.
 *
 * TODO: make this only iterate over the scope blocks and test the scope branch.
 ** ------------------------------------------------------------------------------------------------------------- */
inline void FlattenAssociativeDFG::removeFalseScopeDependencies(PabloFunction & function) {
    ScopeDependencyGraph G;
    {
        ScopeDependencyMap M;
        analyzeScopeDependencies(function.getEntryBlock(), G, M);
    }
    removeDependenciesWithUnresolvedUses(G);
    eliminateUnecessaryDependencies(G);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief transform
 ** ------------------------------------------------------------------------------------------------------------- */
void FlattenAssociativeDFG::transform(PabloFunction & function) {

    FlattenAssociativeDFG::coalesce(function.getEntryBlock(), true);
    #ifndef NDEBUG
    PabloVerifier::verify(function, "post-coalescence");
    #endif

    Simplifier::optimize(function);

    FlattenAssociativeDFG::deMorgansReduction(function.getEntryBlock(), true);
    #ifndef NDEBUG
    PabloVerifier::verify(function, "post-demorgans-reduction");
    #endif

    Simplifier::optimize(function);

    FlattenAssociativeDFG::removeFalseScopeDependencies(function);
    #ifndef NDEBUG
    PabloVerifier::verify(function, "post-remove-false-scope-dependencies");
    #endif

    FlattenAssociativeDFG::tryToPartiallyExtractVariadic(function.getEntryBlock());
    #ifndef NDEBUG
    PabloVerifier::verify(function, "post-partial-variadic-extraction");
    #endif

    Simplifier::optimize(function);


}

}