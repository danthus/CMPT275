﻿#ifndef SCHEDULING_ANALYSIS_HPP
#define SCHEDULING_ANALYSIS_HPP

#include "pipeline_analysis.hpp"
#include <algorithm>
#include <random>

#include <chrono>
#include <llvm/Support/Format.h>

namespace kernel {

#ifdef EXPERIMENTAL_SCHEDULING

#define INITIAL_TOPOLOGICAL_POPULATION_SIZE (30)

#define MAX_POPULATION_SIZE (30)

#define MAX_EVOLUTIONARY_ROUNDS (30)

#define MUTATION_RATE (0.20)

#define MAX_CUT_ACO_ROUNDS (50)

#define BIPARTITE_GRAPH_UNPLACED (0U)

#define BIPARTITE_GRAPH_LEFT_HAND (1U)

#define BIPARTITE_GRAPH_RIGHT_HAND (2U)

#define INITIAL_SCHEDULING_POPULATION_ATTEMPTS (20)

#define INITIAL_SCHEDULING_POPULATION_SIZE (10)

// #define ALLOW_ILLEGAL_PROGRAM_SCHEDULES_IN_EA_SET

#define SCHEDULING_FITNESS_COST_ACO_RHO (0.1)

#define SCHEDULING_FITNESS_COST_ACO_ROUNDS (100)

#define HAMILTONIAN_PATH_STRATEGY (1)

#define HAMILTONIAN_PATH_DEFAULT_WEIGHT (1.0)

#define HAMILTONIAN_PATH_PARTITION_EXIT_WEIGHT (0.001)

#define HAMILTONIAN_PATH_MINIMUM_WEIGHT (0.001)


static size_t init_time = 0;
static size_t fitness_time = 0;
static size_t repair_time = 0;
static size_t evolutionary_time = 0;


/** ------------------------------------------------------------------------------------------------------------- *
 * @brief analyzeDataflowWithinPartitions
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineAnalysis::schedulePartitionedProgram(PartitionGraph & P) {

    // Once we analyze the dataflow within the partitions, P contains DAWG that is either
    // edgeless if any permutation of its kernels is valid or contains all of its optimal
    // orderings for the kernels within each partition.

    analyzeDataflowWithinPartitions(P);

    // The graph itself has edges indicating a dependency between the partitions, annotated by the kernels
    // that are a producer of one of the streamsets that traverses the partitions. Ideally we'll use the
    // trie to score each of the possible orderings based on how close a kernel is to its cross-partition
    // consumers but first we need to determine the order of our partitions.

    const auto D = analyzeDataflowBetweenPartitions(P);

    auto S = makeInterPartitionSchedulingGraph(P, D);

    const auto C = scheduleProgramGraph(P, S, D);

    addSchedulingConstraints(P, C);

}

void printDAWG(const OrderingDAWG & G, const std::vector<unsigned> & K, raw_ostream & out, const StringRef name = "G") {

    out << "digraph \"" << name << "\" {\n";
    for (auto v : make_iterator_range(vertices(G))) {
        out << "v" << v << " [label=\"\"];\n";
    }
    for (auto e : make_iterator_range(edges(G))) {
        const auto s = source(e, G);
        const auto t = target(e, G);
        out << "v" << s << " -> v" << t <<
               " [label=\"" << K[G[e]] << "\"];\n";
    }

    out << "}\n\n";
    out.flush();
}

void printDAWG(const OrderingDAWG & G, raw_ostream & out, const StringRef name = "G") {

    out << "digraph \"" << name << "\" {\n";
    for (auto v : make_iterator_range(vertices(G))) {
        out << "v" << v << " [label=\"\"];\n";
    }
    for (auto e : make_iterator_range(edges(G))) {
        const auto s = source(e, G);
        const auto t = target(e, G);
        out << "v" << s << " -> v" << t <<
               " [label=\"" << G[e] << "\"];\n";
    }

    out << "}\n\n";
    out.flush();
}

namespace { // start of anonymous namespace

using Vertex = unsigned;

using Candidate = std::vector<Vertex>;

using Candidates = std::map<Candidate, size_t>;

using Individual = Candidates::const_iterator;

struct FitnessComparator {
    bool operator()(const Individual & a,const Individual & b) const{
        return a->second < b->second;
    }
};

using Population = std::vector<Individual>;

using random_engine = std::default_random_engine; // TODO: look into xorshift for this

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief postorder_minimize
 ** ------------------------------------------------------------------------------------------------------------- */
void postorder_minimize(OrderingDAWG & O) {

    // Adapted from "Comparison of construction algorithms for minimal acyclic
    // deterministic finite-state automata from a set of strings." 2003

    // Since final/non-final states are indicated by whether we're at the last
    // level or not, we ignore such comparisons; thus any state whose (outgoing)
    // transitions match are considered equal. Additionally, all strings are of
    // equal length but were not lexographically inserted. However, since the
    // level of each state in the DAWG cannot change w.r.t. the trie, we simplify
    // the original algorithm to avoid using a hash table.

    using Vertex = OrderingDAWG::vertex_descriptor;

    std::vector<Vertex> L;

    const auto n = num_vertices(O);

    BitVector P(n);

    BEGIN_SCOPED_REGION

    unsigned sink = 0;

    for (unsigned i = 1; i < n; ++i) {
        if (out_degree(i, O) == 0) {
            assert (in_degree(i, O) > 0);
            const auto e = in_edge(i, O);
            const auto p = source(e, O);
            P.set(p);
            if (sink == 0) {
                sink = i;
            } else {
                const auto ch = O[e];
                clear_in_edges(i, O);
                add_edge(p, sink, ch, O);
            }
        }
    }

    END_SCOPED_REGION

    using SV = SmallVector<std::pair<unsigned, unsigned>, 8>;

    std::vector<SV> T;

    for (;;) {

        L.clear();
        for (const auto u : P.set_bits()) {
            if (LLVM_UNLIKELY(u == 0)) {
                assert (P.count() == 1);
                return;
            }
            assert (in_degree(u, O) == 1);
            L.push_back(u);
        }
        P.reset();

        const auto m = L.size();

        if (T.size() < m) {
            T.resize(m);
        }

        for (unsigned i = 0; i < m; ++i) {

            // lexographically sort our outgoing transitions for every state in L
            const auto u = L[i];
            auto & A = T[i];
            A.clear();
            for (const auto e : make_iterator_range(out_edges(u, O))) {
                A.emplace_back(O[e], target(e, O));
            }
            std::sort(A.begin(), A.end());

            // check whether the i-th node is a duplicate of another within the
            // same level L
            const auto e = in_edge(u, O);
            const auto p = source(e, O);

            P.set(p);

            for (unsigned j = 0; j < i; ++j) {
                const auto v = L[j];
                const auto & B = T[j];
                if (A == B) {
                    const auto ch = O[e];
                    clear_vertex(u, O);
                    add_edge(p, v, ch, O);
                    break;
                }

            }
        }
    }

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief MemoryAnalysis
 ** ------------------------------------------------------------------------------------------------------------- */
class MemoryAnalysis {

    struct ACO {
        double Weight;
        double Pheromone;
    };

    using IntervalGraph = adjacency_list<vecS, vecS, undirectedS, no_property, ACO>;

    using IntervalEdge = typename IntervalGraph::edge_descriptor;

    enum class Orientation {
        Forwards = 0
        , Backwards = 1
        , Unknown = 2
    };

    struct EdgeOrientation {
        unsigned Component;
        Orientation Direction;

        EdgeOrientation(Orientation dir = Orientation::Forwards, unsigned component = 0)
        : Component(component)
        , Direction(dir) {

        }

    };

    using random_engine = std::default_random_engine;

    using TransitiveGraph = adjacency_list<vecS, vecS, undirectedS, no_property, EdgeOrientation>;

public:

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief analyze
     *
     * The analyze function is an offline DSA algorithm that returns an upperbound on the required memory.
     *
     * It is based on the algorithm presented in "Comparability graph coloring for optimizing utilization of
     * software-managed stream register files for stream processors" (2012) but instead of generating spanning
     * forests to mark "long-lived" streamsets, it generates a bipartite graph, potentially taking a max-cut
     * of a non-bipartite graph to preserve as many important relationships as possible. Because the algorithm
     * has a running time of O(2^(N + 1)), where N is the number of connected components in the bipartite graph,
     * is based on Prim's spanning tree algorithm. Rather than taking a random spanning tree, however, it
     * combines an ant colony heuristic with it to locate the heaviest cut that does not increase the number of
     * connected components.
     *
     * In cases where a max-cut is necessary, this analyze function will return the chromatic number of the
     * optimal colouring based on the original algorithm PLUS a greedy colouring of the "uncut" edges as a
     * worst-case proxy for the true chromatic number.
     ** ------------------------------------------------------------------------------------------------------------- */
    size_t analyze(const Candidate & candidate) {

        assert (candidate.size() == numOfKernels);

        if (LLVM_LIKELY(numOfStreamSets == 0)) {
            return 0;
        }

        assert (numOfKernels > 1);

        const auto fitness_start = std::chrono::high_resolution_clock::now();

        // Each node value in the interval graph marks the position of the candidate schedule
        // that produced the streamset.

        IntervalGraph I(numOfStreamSets);

        std::fill_n(live.begin(), numOfStreamSets, 0);

        BEGIN_SCOPED_REGION

        unsigned streamSetId = 0;
        unsigned position = 0;
        for (const auto kernel : candidate) {
            assert (kernel < numOfKernels);
            assert (S[kernel].Type == SchedulingNode::IsKernel);
            for (const auto output : make_iterator_range(out_edges(kernel, S))) {
                const auto streamSet = target(output, S);
                const SchedulingNode & node = S[streamSet];
                assert (node.Type == SchedulingNode::IsStreamSet);
                // assert (node.Size > 0);
                assert (streamSetId < numOfStreamSets);
                const auto i = streamSetId++;
                component[i] = position;
                for (unsigned j = 0; j != i; ++j) {
                    if (live[j] != 0) {
                        add_edge(j, i, I);
                        live[j]--;
                    }
                }
                live[i] = out_degree(streamSet, S);
            }
            ++position;
        }

        assert (position == numOfKernels);
        assert (streamSetId == numOfStreamSets);

        END_SCOPED_REGION

        const auto l = (2 * numOfKernels) + numOfStreamSets;

        TransitiveGraph G(l);

        std::fill_n(weight.begin(), l, 0);

        unsigned streamSetId = 0;
        unsigned priorProducerRank = 0;

        for (const auto kernel : candidate) {
            for (const auto output : make_iterator_range(out_edges(kernel, S))) {
                const auto streamSet = target(output, S);
                const SchedulingNode & node = S[streamSet];
                assert (node.Type != SchedulingNode::IsKernel);
                assert (node.Type == SchedulingNode::IsStreamSet);
                assert (streamSetId < numOfStreamSets);
                const auto i = streamSetId++;

                // Each node value in I marks the schedule position.
                const auto producerRank = component[i];
                assert (priorProducerRank <= producerRank);
                priorProducerRank = producerRank;

                auto consumerRank = producerRank;
                for (const auto e : make_iterator_range(out_edges(i, I))) {
                    const auto j = target(e, I);
                    const auto rank = component[j];
                    consumerRank = std::max(consumerRank, rank);
                }

                const auto lifespan = consumerRank - producerRank;

                const auto W = ceiling(S[streamSet].Size);

                if (LLVM_LIKELY(lifespan <= 1)) {
                    const auto j = (2 * producerRank) | lifespan;
                    assert (j < (2 * numOfKernels));
                    weight[j] += W;

                    // If the lifespan of this streamset is at most one, we can place it into the
                    // comparability graph and do not need to reason about it within the forest.
                    clear_vertex(i, I);

                } else {

                    const auto j = (2 * numOfKernels) + i;
                    weight[j] = W;

                    // NOTE: we mark the direction of the edges between the "forest" and comparability
                    // graph nodes as Unknown since we do not know their orientation until we've built
                    // the spanning forest for the remaining interval graph.

                    const auto m = ((2 * consumerRank) | 1);

                    for (unsigned i = (2 * producerRank); i <= m; ++i) {
                        assert (i < j);
                        add_edge(i, j, EdgeOrientation{Orientation::Unknown}, G);
                    }

                }
            }
        }

        assert (streamSetId == numOfStreamSets);

        // fill in the comparability graph edges
        auto dir = Orientation::Forwards;
        for (unsigned i = 1; i < numOfKernels; ++i) {
            const auto s = (i - 1) * 2;
            add_edge(s, s + 1U, EdgeOrientation{dir}, G);
            add_edge(s + 1U, s + 2U, EdgeOrientation{dir}, G);
            add_edge(s, s + 2U, EdgeOrientation{dir}, G);
            assert ((s + 2U) < (2U * numOfKernels));
            dir = (dir == Orientation::Forwards) ? Orientation::Backwards : Orientation::Forwards;
        }

        // Wang et al.'s paper suggests that graph G_I (once we remove any edges
        // accounted for in G) will likely be a forest and suggest taking a
        // spanning forest otherwise. What they want is a prime comparability
        // graph. Any tree is a bipartite graph and bipartite graphs are trivally
        // comparability graphs but are not necessarily trees.

        // Check if G_I is a bipartite graph and if not, do a max-cut.

        size_t worstCaseUnderapproximation = 0;

redo_bipartite_check_after_max_cut:

        assert (placement.size() >= numOfStreamSets);

        for (unsigned i = 0; i < numOfStreamSets; ++i) {
            placement[i] = (out_degree(i, I) == 0) ? BIPARTITE_GRAPH_LEFT_HAND : BIPARTITE_GRAPH_UNPLACED;
        }

        unsigned N = 1;

        for (unsigned r = 0;;) {

            // select the first vertex to 0/1 colour.
            for (;;) {
                // if we've placed every vertex, we can ignore this phase.
                if (r == numOfStreamSets) {
                    goto is_bipartite_graph;
                }
                if (placement[r] == BIPARTITE_GRAPH_UNPLACED) {
                    break;
                }
                assert (r < numOfStreamSets);
                ++r;
            }
            assert (r < numOfStreamSets);
            placement[r] = BIPARTITE_GRAPH_LEFT_HAND;
            assert (stack.empty());

            for (auto u = r;;) {

                assert (placement[u] != BIPARTITE_GRAPH_UNPLACED);
                const auto OTHER_HAND = (placement[u] ^ (BIPARTITE_GRAPH_LEFT_HAND | BIPARTITE_GRAPH_RIGHT_HAND));
                component[u] = N;
                for (const auto e : make_iterator_range(out_edges(u, I))) {
                    const auto v = target(e, I);
                    if (placement[v] == BIPARTITE_GRAPH_UNPLACED) {
                        placement[v] = OTHER_HAND;
                        stack.push_back(v);
                    } else if (placement[v] != OTHER_HAND) {
                        stack.clear();
                        assert ("second bipartite check failed?" && (worstCaseUnderapproximation == 0));
                        worstCaseUnderapproximation = compute_max_cut(I);
                        goto redo_bipartite_check_after_max_cut;
                    }
                }
                if (stack.empty()) {
                    break;
                }
                u = stack.back();
                stack.pop_back();
            }

            ++N;
        }

is_bipartite_graph:

        const auto firstStreamSet = (2 * numOfKernels);

        // orient the bridging edges according to the left/right hand sidedness.
        for (unsigned i = 0; i != numOfStreamSets; ++i) {
            const auto u = firstStreamSet + i;
            const auto inA = placement[i] == BIPARTITE_GRAPH_LEFT_HAND;
            const auto componentId = component[i];

            for (const auto e : make_iterator_range(out_edges(u, G))) {
                assert (target(e, G) < firstStreamSet);
                EdgeOrientation & O = G[e];
                O.Component = componentId;
                assert (O.Direction == Orientation::Unknown);
                // these are flipped w.r.t. the src < target ordering
                O.Direction = inA ? Orientation::Backwards : Orientation::Forwards;
            }
        }

        // then add our (bipartite) interval graph edges
        for (unsigned i = 0; i != numOfStreamSets; ++i) {
            const auto u = firstStreamSet + i;
            const auto inA = placement[i] == BIPARTITE_GRAPH_LEFT_HAND;
            const auto componentId = component[i];
            for (const auto e : make_iterator_range(out_edges(i, I))) { // ST
                const auto j = target(e, I); // ST
                assert (i != j);
                const bool flipped = (j < i);
                const auto dir = (inA ^ flipped) ? Orientation::Forwards : Orientation::Backwards;
                const auto v = firstStreamSet + j;
                add_edge(u, v, EdgeOrientation{dir, componentId}, G);
            }
        }

        // Our goal now is to find a minimal maximum-weight path through every
        // acyclic orientation of G; to do so we consider the permutations of the
        // component direction flags.

        auto chromaticNumber = std::numeric_limits<size_t>::max();

        // Based on the assumption N is relatively small, we can use a single counter
        // from 0 to pow(2,N) - 1 to represent our current premutation. If N > 6,
        // we'll need another method to converge on a solution.

        assert (N <= 10);

        for (unsigned i = 0; i < (1U << N); ++i) {
            const auto weight = calculate_orientation_clique_weight(i, G);
            chromaticNumber = std::min<size_t>(chromaticNumber, weight);
        }

        const auto result = chromaticNumber + worstCaseUnderapproximation;

        const auto fitness_end = std::chrono::high_resolution_clock::now();
        fitness_time += (fitness_end - fitness_start).count();

        return result;
    }

private:

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief calculate_orientation_clique_weight
     ** ------------------------------------------------------------------------------------------------------------- */
    unsigned calculate_orientation_clique_weight(const std::bitset<64> permutation, const TransitiveGraph & G) {

        // Ideally, we want to construct a topological ordering of our vertices then
        // recursively add the sum of the heaviest path into each vertex with the
        // current node weight starting from every source.

        // Unfortunetly, the way we faked the orientation of the edges makes it
        // impossible to use a boost method for this so the following implements
        // Kahn's algorithm. Since we're not actually interested in the ordering
        // itself and only the max weight of any path, we track that instead.

        // TODO we can more intelligently determine our source/sinks for this
        // permutation by using what we know about the graph structure.

        const auto l = num_vertices(G);

        std::fill_n(live.begin(), l, 0);

        auto is_oriented_forwards = [&](const unsigned u, const unsigned v, const TransitiveGraph::edge_descriptor e) {
            // We assume that given an edge (u, v), the direction is correct
            // if and only if index(u) < index(v). Flip the direction otherwise.
            assert (u == source(e, G) && v == target(e, G));
            const EdgeOrientation & O = G[e];
            const auto flipped = (v < u) ^ permutation.test(O.Component);
            return (O.Direction == Orientation::Forwards) ^ flipped;
        };

        for (const auto e : make_iterator_range(edges(G))) {
            const auto u = source(e, G);
            const auto v = target(e, G);
            const auto w = is_oriented_forwards(u, v, e) ? v : u;
            live[w]++;
        }

        assert (stack.empty());

        for (unsigned u = 0; u < l; ++u) {
            accum[u] = weight[u];
            // find all of our sinks
            if (live[u] == 0) {
                stack.push_back(u);
            }
        }

        assert (stack.size() > 0);

        unsigned visited = 0;

        size_t maxWeight = 0;
        for (;;) {

            const auto u = stack.back();
            stack.pop_back();
            ++visited;

            assert (in_degree(u, G) == out_degree(u, G));

            size_t weight = 0;
            for (const auto e : make_iterator_range(out_edges(u, G))) {
                const auto v = target(e, G);
                if (is_oriented_forwards(u, v, e)) {
                    assert ("G cannot be an cyclic graph" && live[v] > 0);
                    if (--live[v] == 0) {
                        stack.push_back(v);
                    }

                } else {
                    assert (live[v] == 0);
                    weight = std::max(weight, accum[v]);
                }
            }
            accum[u] += weight;
            maxWeight = std::max(maxWeight, accum[u]);
            if (stack.empty()) break;
        }

        assert (visited == l);
        return maxWeight;
    };


    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief compute_max_cut
     ** ------------------------------------------------------------------------------------------------------------- */
    size_t compute_max_cut(IntervalGraph & I) {

        // If G_I is not a bipartite graph, intuitively, we want to keep as many
        // interval relationships adjacent to heavy nodes as possible. So we're going
        // to apply a weighted max-cut to it to transform G_I into one by discarding
        // any "uncut" edges.

        constexpr double T_Init = 1.0;
        constexpr double T_Min = 0.001;

        constexpr double MAX_CUT_RHO = 0.005;
        constexpr double MAX_CUT_BETA = 1.0;

        // The following algorithm was originally based on the paper "An ant colony
        // algorithm for solving Max-cut problems" (2008). However, solutions found
        // by that approach typically resulted in many connected components (CCs).
        // Since the run-time of the interval colouring is O(2^(N + 1)), where N is
        // the number of CCs, general max-cut algorithms had a catastrophic impact
        // on the useability of the comparability graph approach.

        // To preserve the actual number of CCs, the algorithm was rewritten to use
        // Prims's spanning tree algorithm to define each cut set. The greedy local
        // search phase was also discarded.

        // Through empirical analysis, I modified the pheromone calculation function
        // to more quickly converge on a "good enough" solution but kept the same
        // +/- flavour of the paper's original deposit function.

        // TODO: I'm currently applying the Pythagorean theorem to the endpoint
        // weights to make the edge weight; hopefully this will prioritize placing
        // pairs of heavy nodes into differing sets over a heavy and light or two
        // light nodes. Investigate an alternate metric.

        for (const auto e : make_iterator_range(edges(I))) {
            const auto u = source(e, I);
            const auto v = target(e, I);
            const size_t Wu = weight[u + (2 * numOfKernels)];
            const size_t Wv = weight[v + (2 * numOfKernels)];
            const auto weight = std::log10(std::pow(std::sqrt(Wu * Wu + Wv * Wv), MAX_CUT_BETA));

            ACO & M = I[e];
            M.Weight = weight;
            assert (M.Weight > 0.0);
            M.Pheromone = T_Init;
        }

        const auto numOfComponents = collect_connected_components(I);

        compute_spanning_tree_to_determine_placement(I, numOfComponents);

        std::vector<unsigned> solution(placement);

        auto bestWeight = calculate_cut_weight(I);

        for (const auto e : make_iterator_range(edges(I))) {
            const auto u = source(e, I);
            const auto v = target(e, I);
            if (placement[u] != placement[v]) {
                ACO & M = I[e];
                M.Pheromone += 1.0;
            }
        }

        for (unsigned r = 0; r < MAX_CUT_ACO_ROUNDS; ++r) {

            compute_spanning_tree_to_determine_placement(I, numOfComponents);

            // check effect of this change
            const auto currentWeight = calculate_cut_weight(I);

            // update the pheromone matrix
            double deposit = 0.0;
            if (currentWeight > bestWeight) {
                const auto r = std::sqrt(currentWeight - bestWeight);
                deposit = r / (0.2 + r);
            } else if (currentWeight < bestWeight) {
                const auto r = std::sqrt(bestWeight - currentWeight);
                deposit = -r / (0.2 + r);
            }

            for (const auto e : make_iterator_range(edges(I))) {
                ACO & M = I[e];
                M.Pheromone *= (1.0 - MAX_CUT_RHO);
                const auto u = source(e, I);
                const auto v = target(e, I);
                if (placement[u] != placement[v]) {
                    M.Pheromone += deposit;
                }
                M.Pheromone = std::max(M.Pheromone, T_Min);
            }

            if (bestWeight < currentWeight) {
                bestWeight = currentWeight;
                solution.swap(placement);
            }

        }

        IntervalGraph B(numOfStreamSets);

        remove_edge_if([&](const IntervalGraph::edge_descriptor e){
            const auto u = source(e, I);
            const auto v = target(e, I);
            if (solution[u] == solution[v]) {
                return true;
            } else {
                add_edge(u, v, B);
                return false;
            }
        }, I);

        // For every node we place in which we have deleted an edge to transform
        // G_I into a bipartite graph, our colouring could be at most wrong by the
        // the total colours needed to colour the graph G_I' constructed from its
        // deleted edges. G_I is an interval graph but G_I' is not necessarily one
        // since we're generating it by removing edges not vertices. However,
        // LexBFS may still return a good ordering for a greedy colouring.

        // TODO: either prove the vertex index ordering of G_I is a reverse PEO
        // or implement LexBFS here. I suspect it is a reverse PEO since we can
        // orient any edges of a clique from left to right.

        I.swap(B);

        // Note: B here is originally I due to the prior swap
        return greedy_colouring(B);
    }

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief determine_component
     ** ------------------------------------------------------------------------------------------------------------- */
    unsigned collect_connected_components(const IntervalGraph & I) {

        assert (num_edges(I) > 0);

        std::fill_n(placement.begin(), numOfStreamSets, 0);

        std::fill_n(accum.begin(), numOfStreamSets, 0);

        unsigned k = 1;
        unsigned pos = 0;

        accum[0] = 0;

        for (unsigned i = 0; i < numOfStreamSets; ++i) {
            assert (stack.empty());
            if (placement[i] == 0 && out_degree(i, I) > 0) {
                auto u = i;
                for (;;) {
                    component[pos++] = u;
                    for (const auto e : make_iterator_range(out_edges(u, I))) {
                        const auto v = target(e, I);
                        if (placement[v] == 0) {
                            placement[v] = k;
                            stack.push_back(v);
                        }
                        assert (placement[v] == k);
                    }
                    if (stack.empty()) {
                        break;
                    }
                    u = stack.back();
                    stack.pop_back();
                }
                accum[k++] = pos; // store the first position of the next component
            }
        }
        assert (k > 1);
        return k - 1;
    }

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief compute_spanning_tree_to_determine_placement
     *
     * Uses a modified version of Prim's algorithm to find a maximal spanning forest; makes edge inclusion choices
     * using weights an ant colony heuristic.
     ** ------------------------------------------------------------------------------------------------------------- */
    void compute_spanning_tree_to_determine_placement(const IntervalGraph & I, const unsigned k) {

        SmallVector<unsigned, 4> roots(k);
        for (unsigned i = 0; i < k; ++i) {
            assert (accum[i + 1] > accum[i]);
            const auto m = accum[i + 1] - accum[i];
            std::uniform_int_distribution<unsigned> initial(0, m - 1);
            const auto j = initial(rng) + accum[i];
            assert (j < numOfStreamSets);
            roots[i] = component[j];
        }

        std::fill_n(placement.begin(), numOfStreamSets, BIPARTITE_GRAPH_UNPLACED);

        BitVector in_tree(numOfStreamSets);
        for (const auto root : roots) {
            placement[root] = BIPARTITE_GRAPH_LEFT_HAND;
            in_tree.set(root);
        }

        SmallVector<IntervalEdge, 8> selected;
        SmallVector<double, 8> probability;

        for (;;) {

            #ifndef NDEBUG
            auto remaining = in_tree.count();
            #endif

            assert (probability.empty() && selected.empty());

            double sum = 0.0;
            for (const auto u : in_tree.set_bits()) {
                assert (placement[u] != BIPARTITE_GRAPH_UNPLACED);
                bool all_adjacencies_added = true;
                for (const auto e : make_iterator_range(out_edges(u, I))) {
                    const auto v = target(e, I);
                    assert (v != u);
                    if (placement[v] == BIPARTITE_GRAPH_UNPLACED) {
                        all_adjacencies_added = false;
                        const ACO & M = I[e];
                        assert (M.Pheromone > 0);
                        const auto w = std::pow(M.Pheromone, 2.0) * M.Weight;
                        selected.push_back(e);
                        probability.push_back(w);
                        sum += w;
                    }
                }
                if (all_adjacencies_added) {
                    in_tree.reset(u);
                }
                #ifndef NDEBUG
                --remaining;
                #endif
            }

            assert ("failed to visit every incomplete node?" && remaining == 0);

            if (selected.empty()) {
                break;
            }

            assert (sum > 0.0);

            std::uniform_real_distribution<double> distribution(0.0, sum);
            const auto c = distribution(rng);

            double d = std::numeric_limits<double>::epsilon();
            const auto m = probability.size();
            bool found = false;
            for (unsigned i = 0; i < m; ++i) {
                d += probability[i];
                if (d >= c) {
                    const auto e = selected[i];
                    const auto u = source(e, I);
                    const auto v = target(e, I);
                    assert (placement[u] != BIPARTITE_GRAPH_UNPLACED && placement[v] == BIPARTITE_GRAPH_UNPLACED);
                    assert (!in_tree.test(v));
                    in_tree.set(v);
                    placement[v] = placement[u] ^ (BIPARTITE_GRAPH_LEFT_HAND | BIPARTITE_GRAPH_RIGHT_HAND);
                    found = true;
                    break;
                }
            }
            assert (found);
            probability.clear();
            selected.clear();
        }

        #ifndef NDEBUG
        for (unsigned i = 0; i < numOfStreamSets; ++i) {
            assert ((out_degree(i, I) == 0) || (placement[i] != BIPARTITE_GRAPH_UNPLACED));
        }
        #endif
    }

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief calculate_cut_weight
     ** ------------------------------------------------------------------------------------------------------------- */
    double calculate_cut_weight(const IntervalGraph & I) const {
        double weight = 0;
        for (const auto e : make_iterator_range(edges(I))) {
            const auto u = source(e, I);
            const auto v = target(e, I);
            if (placement[u] != placement[v]) {
                weight += I[e].Weight;
            }
        }
        return weight;
    };

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief greedy_colouring
     ** ------------------------------------------------------------------------------------------------------------- */
    size_t greedy_colouring(const IntervalGraph & I) {

        using Interval = std::pair<unsigned, unsigned>;

        using ColourLine = flat_set<Interval>;

        std::vector<unsigned> remaining(numOfStreamSets, 0);

        std::vector<Interval> GC_Intervals(numOfStreamSets);

        ColourLine GC_CL;

        unsigned max_colours = 0;
        for (unsigned i = 0; i != numOfStreamSets; ++i) {
            const auto w = weight[i + (2 * numOfKernels)];

            if (w == 0) {
                remaining[i] = -1U;
            } else {
                remaining[i] = out_degree(i, I);
                unsigned first = 0;
                for (const auto & interval : GC_CL) {
                    const auto last = interval.first;
                    assert (first <= last);
                    if ((first + w) < last) {
                        break;
                    }
                    first = interval.second;
                }
                const auto last = first + w;
                assert (first <= last);
                if (last > max_colours) {
                    max_colours = last;
                }

                GC_Intervals[i] = std::make_pair(first, last);

                GC_CL.emplace(first, last);

                for (const auto e : make_iterator_range(out_edges(i, I))) {
                    const auto j = target(e, I);
                    if (j < i) {
                        assert (remaining[j] > 0 && remaining[j] < -1U);
                        remaining[j]--;
                    }
                }

                for (unsigned j = 0; j <= i; ++j) {
                    if (remaining[j] == 0) {
                        const auto f = GC_CL.find(GC_Intervals[j]);
                        assert (f != GC_CL.end());
                        GC_CL.erase(f);
                        remaining[j] = -1U;

                    }
                }
            }
        }
        return max_colours;
    };

public:

    MemoryAnalysis(const SchedulingGraph & S, const unsigned numOfKernels)
    : S(S)
    , numOfKernels(numOfKernels)
    , numOfStreamSets(num_vertices(S) - numOfKernels)
    , rng(std::random_device()())
    , live(2 * numOfKernels + numOfStreamSets)
    , weight(2 * numOfKernels + numOfStreamSets)
    , component(numOfStreamSets)
    , placement(numOfStreamSets)
    , accum(2 * numOfKernels + numOfStreamSets) {

    }

protected:

    const SchedulingGraph & S;

    const unsigned numOfKernels;
    const unsigned numOfStreamSets;

    random_engine rng;

private:

    std::vector<unsigned> live;
    std::vector<size_t>   weight;
    std::vector<unsigned> component;
    std::vector<unsigned> placement;
    std::vector<Vertex>   stack;
    std::vector<size_t>   accum;

};

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief SchedulingAnalysisWorker
 ** ------------------------------------------------------------------------------------------------------------- */
struct SchedulingAnalysisWorker {

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief repair
     ** ------------------------------------------------------------------------------------------------------------- */
    virtual void repair(Candidate & candidate) = 0;

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief fitness
     ** ------------------------------------------------------------------------------------------------------------- */
    virtual size_t fitness(const Candidate & candidate) {

        const auto fitness_start = std::chrono::high_resolution_clock::now();

        const auto result = analyzer.analyze(candidate);

        const auto fitness_end = std::chrono::high_resolution_clock::now();
        fitness_time += (fitness_end - fitness_start).count();

        return result;
    }

protected:

    SchedulingAnalysisWorker(const SchedulingGraph & S,
                       const unsigned numOfKernels,
                       const unsigned candidateLength)
    : numOfKernels(numOfKernels)
    , candidateLength(candidateLength)
    , analyzer(S, numOfKernels)
    , rng(std::random_device()()) {

    }

protected:

    const unsigned numOfKernels;
    const unsigned candidateLength;

    MemoryAnalysis analyzer;

    random_engine rng;

};

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief SchedulingAnalysis
 *
 * Both the partition scheduling algorithm and whole program scheduling algorithm rely on the following class.
 * Within it is a genetic algorithm designed to find a minimum memory schedule of a given SchedulingGraph.
 * However, the phenotype of of the partition algorithm is a topological ordering and the phenotype of the
 * whole program is a hamiltonian path. This consitutes a significant enough difference that it is difficult
 * to call with only one function. Instead both the "initGA" and "repair" functions are implemented within
 * the actual scheduling functions.
 ** ------------------------------------------------------------------------------------------------------------- */
class SchedulingAnalysis {

public:

    constexpr static size_t BITS_PER_SIZET = (CHAR_BIT * sizeof(size_t));

    struct permutation_bitset {

        permutation_bitset(const size_t N)
        : _value((N + BITS_PER_SIZET - 1) / BITS_PER_SIZET, 0) {

        }

        void randomize(random_engine & rng) {
            std::uniform_int_distribution<size_t> distribution(std::numeric_limits<size_t>::min(), std::numeric_limits<size_t>::max());
            for (auto & a : _value) {
                a = distribution(rng);
            }
        }

        bool test(unsigned i) const {
            return (_value[i / BITS_PER_SIZET] & (i & (BITS_PER_SIZET - 1))) != 0;
        }

    private:
        SmallVector<size_t, 4> _value;
    };

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief runGA
     ** ------------------------------------------------------------------------------------------------------------- */
    void runGA(OrderingDAWG & result) {

        Population P1;
        P1.reserve(MAX_POPULATION_SIZE);

        if (initGA(P1)) {
            std::sort(P1.begin(), P1.end(), FitnessComparator{});
            goto found_all_orderings;
        }

        BEGIN_SCOPED_REGION

        const auto evolutionary_start = std::chrono::high_resolution_clock::now();

        permutation_bitset bitString(candidateLength);

        BitVector V(candidateLength);

        std::uniform_real_distribution<double> zeroToOneReal(0.0, 1.0);


        std::make_heap(P1.begin(), P1.end(), FitnessComparator{});

        Population P2;
        P2.reserve(3 * MAX_POPULATION_SIZE);

        for (unsigned round = 0; round < MAX_EVOLUTIONARY_ROUNDS; ++round) {

            // errs() << "EA: round=" << round << "\n";

            const auto populationSize = P1.size();

            std::uniform_int_distribution<unsigned> upToN(0, populationSize - 1);

            auto tournament_select = [&]() -> const Candidate & {
                const auto & A = P1[upToN(rng)];
                const auto & B = P1[upToN(rng)];
                if (A->second < B->second) {
                    return A->first;
                } else {
                    return B->first;
                }
            };

//            for (unsigned i = 0; i < populationSize; ++i) {
//                errs() << "  population: ";
//                const auto & I = P1[i];
//                char joiner = '{';
//                for (const auto k : I->first) {
//                    errs() << joiner << k;
//                    joiner = ',';
//                }
//                errs() << "} := " << I->second << "\n";
//            }

            // CROSSOVER:

            assert (P2.empty());

            for (unsigned i = 0; i < populationSize; ++i) {

                const Candidate & A = tournament_select();
                const Candidate & B = tournament_select();

                // generate a random bit string
                bitString.randomize(rng);

                auto crossover = [&](const Candidate & A, const Candidate & B, const bool selector) {

                    Candidate C(candidateLength);

                    V.reset();

                    for (unsigned k = 0; k < candidateLength; ++k) {
                        const auto t = bitString.test(k);
                        if (t == selector) {
                            const auto v = A[k];
                            assert (v < candidateLength);
                            V.set(v);
                        } else {
                            C[k] = A[k];
                        }
                    }

                    for (unsigned k = 0U, p = -1U; k < candidateLength; ++k) {
                        const auto t = bitString.test(k);
                        if (t == selector) {
                            // V contains 1-bits for every entry we did not
                            // directly copy from A into C. We now insert them
                            // into C in the same order as they are in B.
                            for (;;){
                                ++p;
                                assert (p < candidateLength);
                                const auto v = B[p];
                                assert (v < candidateLength);
                                if (V.test(v)) break;
                            }
                            C[k] = B[p];
                        }
                    }

                    insertCandidate(C, P2);

                };

                crossover(A, B, true);

                crossover(B, A, false);

            }

            // MUTATION:

            // Since we generated our initial candidates by taking the first N
            // orderings (with the hope that we have a simple enough graph that
            // we visit all of them), our first set of candidates may be
            // relatively uniform. Try mutating all of them on the first round.

            for (unsigned i = 0; i < populationSize; ++i) {
                const auto j = upToN(rng);
                auto & A = P1[j];
                if (zeroToOneReal(rng) <= MUTATION_RATE) {
                    std::uniform_int_distribution<unsigned> randomPoint(0, candidateLength - 1);
                    auto a = randomPoint(rng);
                    for (;;) {
                        auto b = randomPoint(rng);
                        if (a == b) continue;
                        if (b < a) {
                            std::swap(a, b);
                        }
                        Candidate C{A->first};
                        std::shuffle(C.begin() + a, C.begin() + b, rng);

                        insertCandidate(C, P2);
                        break;
                    }
                }
            }

            // SELECTION:

            for (const auto & I : P2) {
                if (P1.size() == MAX_POPULATION_SIZE) {
                    if (I->second <= P1.front()->second) {
                        std::pop_heap(P1.begin(), P1.end(), FitnessComparator{});
                        P1.pop_back();
                    } else {
                        // New item exceeds the weight of the heaviest candiate
                        // in the population.
                        continue;
                    }
                }
                P1.emplace_back(I);
                std::push_heap(P1.begin(), P1.end(), FitnessComparator{});
            }

            P2.clear();
        }

        const auto evolutionary_end = std::chrono::high_resolution_clock::now();
        evolutionary_time += (evolutionary_end - evolutionary_start).count();

        END_SCOPED_REGION

        // Construct a trie of all possible best (lowest) orderings of this partition
        std::sort_heap(P1.begin(), P1.end(), FitnessComparator{});

found_all_orderings:

        if (LLVM_UNLIKELY(P1.empty())) return;

        assert (std::is_sorted(P1.begin(), P1.end(), FitnessComparator{}));

        auto i = P1.begin();
        const auto end = P1.end();

        const auto bestWeight = (*i)->second;
        do {
            make_trie((*i)->first, result);
        } while ((++i != end) && (bestWeight == (*i)->second));

    }

protected:

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief insertCandidate
     ** ------------------------------------------------------------------------------------------------------------- */
    bool insertCandidate(Candidate & C, Population & population) {
        repair(C);
        const auto f = candidates.emplace(C, 0);
        if (LLVM_LIKELY(f.second)) {
            f.first->second = fitness(f.first->first);
            population.emplace_back(f.first);
            return true;
        }
        return false;
    }

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief initGA
     ** ------------------------------------------------------------------------------------------------------------- */
    virtual bool initGA(Population & initialPopulation) = 0;

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief repair
     ** ------------------------------------------------------------------------------------------------------------- */
    virtual void repair(Candidate & candidate) = 0;

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief fitness
     ** ------------------------------------------------------------------------------------------------------------- */
    virtual size_t fitness(const Candidate & candidate) = 0;

private:

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief make_trie
     ** ------------------------------------------------------------------------------------------------------------- */
    void make_trie(const Candidate & C, OrderingDAWG & O) {
        assert (num_vertices(O) > 0);
        assert (C.size() == candidateLength);
        auto u = 0;

        for (unsigned i = 0; i != candidateLength; ++i) {
            const auto j = C[i];
            assert (j < candidateLength);
            for (const auto e : make_iterator_range(out_edges(u, O))) {
                if (O[e] == j) {
                    u = target(e, O);
                    goto in_trie;
                }
            }
            BEGIN_SCOPED_REGION
            const auto v = add_vertex(O);
            add_edge(u, v, j, O);
            u = v;
            END_SCOPED_REGION
in_trie:    continue;
        }
    };

protected:

    SchedulingAnalysis(const unsigned candidateLength)
    : candidateLength(candidateLength)
    , rng(std::random_device()()) {

    }

protected:

    const unsigned candidateLength;

    std::map<Candidate, size_t> candidates;

    random_engine rng;

};

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief PartitionSchedulingAnalysis
 ** ------------------------------------------------------------------------------------------------------------- */
struct PartitionSchedulingAnalysisWorker final : public SchedulingAnalysisWorker {

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief repair
     ** ------------------------------------------------------------------------------------------------------------- */
    void repair(Candidate & L) override {

        for (unsigned i = 0; i != numOfKernels; ++i) {
            remaining[i] = in_degree(i, D) + 1;
        }
        assert (replacement.size() == numOfKernels);

        for (unsigned i = 0; i < numOfKernels; ) {
            bool progress = false;
            for (unsigned j = 0; j != numOfKernels; ++j) {
                const auto k = L[j];
                if (remaining[k] == 1) {
                    assert (i < numOfKernels);
                    replacement[i++] = k;
                    remaining[k] = 0;
                    for (auto e : make_iterator_range(out_edges(k, D))) {
                        const auto v = target(e, D);
                        assert (remaining[v] > 1);
                        --remaining[v];
                    }
                    progress = true;
                }
            }
            assert (progress);
        }
        L.swap(replacement);

    }


public:

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief constructor
     ** ------------------------------------------------------------------------------------------------------------- */
    PartitionSchedulingAnalysisWorker(const SchedulingGraph & S, const PartitionDependencyGraph & D,
                                      const unsigned numOfKernels)
    : SchedulingAnalysisWorker(S, numOfKernels, numOfKernels)
    , D(D)
    , replacement(numOfKernels)
    , remaining(numOfKernels) {

    }

private:

    const PartitionDependencyGraph & D;

    Candidate replacement;

    std::vector<unsigned> remaining;

};

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief PartitionSchedulingAnalysis
 ** ------------------------------------------------------------------------------------------------------------- */
struct PartitionSchedulingAnalysis final : public SchedulingAnalysis {

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief initGA
     ** ------------------------------------------------------------------------------------------------------------- */
    bool initGA(Population & initialPopulation) override {

        // Any topological ordering of D can generate a valid schedule for our subgraph.
        // Begin by trying to generate N initial candidates. If we fail to enumerate all
        // of them, we'll use an evolutionary algorithm to try and explore the remaining
        // solution space.

        return enumerateUpToNTopologicalOrderings(D, INITIAL_TOPOLOGICAL_POPULATION_SIZE, [&](Candidate & L) {
            initialPopulation.emplace_back(candidates.emplace(L, fitness(L)).first);
        });

    }

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief repair
     ** ------------------------------------------------------------------------------------------------------------- */
    void repair(Candidate & candidate) override {
        worker.repair(candidate);
    }

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief fitness
     ** ------------------------------------------------------------------------------------------------------------- */
    size_t fitness(const Candidate & candidate) override {
        return worker.fitness(candidate);
    }

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief constructor
     ** ------------------------------------------------------------------------------------------------------------- */
    PartitionSchedulingAnalysis(const SchedulingGraph & S, const PartitionDependencyGraph & D,
                                const unsigned numOfKernels)
    : SchedulingAnalysis(numOfKernels)
    , D(D)
    , worker(S, D, numOfKernels) {

    }

private:

    const PartitionDependencyGraph & D;

    PartitionSchedulingAnalysisWorker worker;

};

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief ProgramSchedulingAnalysis
 ** ------------------------------------------------------------------------------------------------------------- */
struct ProgramSchedulingAnalysisWorker final : public SchedulingAnalysisWorker {

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief repair
     ** ------------------------------------------------------------------------------------------------------------- */
    void repair(Candidate & candidate) override {

        #ifndef ALLOW_ILLEGAL_PROGRAM_SCHEDULES_IN_EA_SET

        const auto repair_start = std::chrono::high_resolution_clock::now();

        nearest_valid_schedule(candidate);

        const auto repair_end = std::chrono::high_resolution_clock::now();
        repair_time += (repair_end - repair_start).count();

        #endif

    }

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief fitness
     ** ------------------------------------------------------------------------------------------------------------- */
    size_t fitness(const Candidate & candidate) override {

        const auto fitness_start = std::chrono::high_resolution_clock::now();
        #ifdef ALLOW_ILLEGAL_PROGRAM_SCHEDULES_IN_EA_SET
        auto result = std::numeric_limits<size_t>::max();
        if (is_valid_hamiltonian_path(candidate)) {
            result = analyzer.analyze(candidate);
        }
        #else
        const auto result = analyzer.analyze(candidate);
        #endif
        const auto fitness_end = std::chrono::high_resolution_clock::now();
        fitness_time += (fitness_end - fitness_start).count();

        return result;
    }

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief nearest_valid_schedule
     ** ------------------------------------------------------------------------------------------------------------- */
    void nearest_valid_schedule(Candidate & candidate) {

        assert (candidate.size() == numOfKernels);
        assert (index.size() == numOfKernels);

        // record the index position of each kernel in the candidate
        for (unsigned i = 0; i < numOfKernels; ++i) {
            const auto j = candidate[i];
            assert (j < numOfKernels);
            index[j] = i;
        }

        auto missing_element_cost = [&](const size_t k) -> double {
            if (k == 0) return 0;
            return ((double)(9 * k)) / ((double)numOfKernels) + 1.0;
        };

        std::vector<unsigned> aux(numOfKernels);
        std::vector<unsigned> offset(numOfKernels);

        auto tau_distance = [&](const unsigned m) {

            // normalized kendal tau distance algorithm [0,1]
            for (unsigned i = 0; i < m; ++i) {
                const auto k = toEval[i];
                assert (k < numOfKernels);
                offset[i] = index[k];
            }

            size_t inversions = 0;

            std::function<void(unsigned, unsigned)> inversion_count = [&](const unsigned lo, const unsigned hi) {
                if (lo < hi) {
                    const auto mid = (lo + hi) / 2;
                    inversion_count(lo, mid);
                    inversion_count(mid + 1, hi);

                    for (auto i = lo; i <= hi; ++i) {
                        aux[i] = offset[i];
                    }
                    auto i = lo;
                    auto j = mid + 1;
                    for (auto k = lo; k <= hi; ++k) {
                        if (i > mid) {
                            offset[k] = aux[j++];
                        } else if (j > hi) {
                            offset[k] = aux[i++];
                        } else if (aux[j] < aux[i]) {
                            offset[k] = aux[j++];
                            inversions += (mid - i + 1);
                        } else {
                            offset[k] = aux[i++];
                        }
                    }
                }
            };

            inversion_count(0, m - 1);
            const auto max = ((m * (m - 1)) / 2);
            assert (inversions <= max);
            return ((double)inversions) / ((double)max);
        };

restart_process:

        #if HAMILTONIAN_PATH_STRATEGY == 1

        // TODO: I'm not satisfied with the notion of "best path" as it violates the spirit
        // of an ACO. The goal is to make the best known path a little more resilient to
        // trail degradation. This improves the convergence rate to a good enough solution
        // but this makes it far more prone to a local minimum. Could number of times a path
        // is marked "best" be a form of weight?

        flat_set<std::pair<unsigned, unsigned>> bestPath;
        #endif

        double bestInversions = missing_element_cost(numOfKernels);

        unsigned converged = 0;

        for (auto & e : trail) {
            Trail & t = e.second;
            t.Pheromone = HAMILTONIAN_PATH_DEFAULT_WEIGHT;
            t.Permanence = 0.0;
        }

        replacement.clear();

        for (unsigned r = 0; r < SCHEDULING_FITNESS_COST_ACO_ROUNDS; ++r) {

            visited.reset();

            Vertex u = 0;

            path.clear();

            for (;;) {

                assert (u < num_vertices(O));

                assert (!visited.test(u));

                visited.set(u);

                path.push_back(u);

                targets.clear();

                for (const auto e : make_iterator_range(out_edges(u, O))) {
                    const auto v = target(e, O);
                    if (visited.test(v)) continue;
                    const auto f = trail.find(std::make_pair(u, v));
                    assert (f != trail.end());
                    const Trail & t = f->second;
                    targets.emplace_back(v, t.Pheromone);
                }

                const auto k = targets.size();

                if (k <= 1) {
                    if (LLVM_UNLIKELY(k == 0)) {
                        break;
                    } else {
                        u = targets.front().first;
                        continue;
                    }
                }

                double sum = 0.0;

                for (const auto & t : targets) {
                    sum += t.second;
                }

                std::uniform_real_distribution<double> distribution(0.0, sum);
                const auto c = distribution(rng);

                bool found = false;
                double d = std::numeric_limits<double>::epsilon();
                for (const auto & t : targets) {
                    d += t.second;
                    if (d >= c) {
                        u = t.first; // set our next target
                        found = true;
                        break;
                    }
                }
                assert (found);
            }

            // extract the sequence of kernel ids from the path
            toEval.clear();
            for (const auto i : path) {
                const auto & A = O[i];
                toEval.insert(toEval.end(), A.begin(), A.end());
            }
            const auto m = toEval.size();

            assert (m <= numOfKernels);

            // Count how many inversions occured in this path but since we may not
            // have acutally constructed a hamiltonian path, initialize the cost to
            // penalize such solutions.

            const auto inversions = tau_distance(m) + missing_element_cost(numOfKernels - m); // [0,10)

            #if HAMILTONIAN_PATH_STRATEGY == 0

            const double deposit = (10.0) / ((double)inversions);

            for (auto & p : pheromone) {
                double & trail = p.second;
                trail *= (1.0 - SCHEDULING_FITNESS_COST_ACO_RHO);
            }

            for (unsigned i = 1; i < m; ++i) {
                const auto e = std::make_pair(path[i - 1], path[i]);
                const auto f = pheromone.find(e);
                assert (f != pheromone.end());
                double & trail = f->second;
                trail += deposit;
            }

            for (auto & p : pheromone) {
                double & trail = p.second;
                trail = std::max(trail, HAMILTONIAN_PATH_MINIMUM_WEIGHT);
            }

            #endif

            #if HAMILTONIAN_PATH_STRATEGY == 1

            const auto l = path.size();

            double deposit = 0.0;

            if (inversions > bestInversions) {
                const auto d = inversions - bestInversions; // [0,10)
                const auto c = std::log(d + 1);
                deposit = c / (1.0 + c);

                for (unsigned i = 1; i < l; ++i) {
                    const auto e = std::make_pair(path[i - 1], path[i]); // [0,10)
                    const auto f = trail.find(e);
                    assert (f != trail.end());
                    Trail & t = f->second;
                    const auto scale = 1.0 - (0.90 * std::sqrt(t.Permanence));
                    const auto d = deposit * scale;
                    t.Pheromone = std::max(t.Pheromone - d, HAMILTONIAN_PATH_MINIMUM_WEIGHT);
                }


            } else if (inversions < bestInversions) {
                const auto d = bestInversions - inversions;
                deposit = std::sqrt(d) / (1.0 + std::log(d + 1));
                for (unsigned i = 1; i < l; ++i) {
                    const auto e = std::make_pair(path[i - 1], path[i]); // [0,10)
                    const auto f = trail.find(e);
                    assert (f != trail.end());
                    Trail & t = f->second;
                    t.Permanence = std::min(t.Permanence + 0.1, 1.0);
                    t.Pheromone += d;

                }

                // Store our path if its the best one
                if (m == numOfKernels) {
                    replacement.swap(toEval);
                }
                bestInversions = inversions;

                converged = 0;

            } else { // if (inversions == bestInversions) {
                ++converged;
            }

            #endif

//            errs() << " candidate:";
//            char joiner = ' ';
//            for (const auto c : path) {
//                errs() << joiner << c;
//                joiner= ',';
//            }
//            errs() << "  := " << format("%.4f", inversions) <<
//                         " (" << format("%.4f", bestInversions) << ")"
//                         " d=" << format("%.4f", deposit);

//            // Store our path if its the best one
//            if (inversions < bestInversions) {
//                converged = 0;
//                errs() << " *";
//            }

//            errs() << "\n";

//            auto & out = errs();
//            out << "digraph \"O" << r << "\" {\n";
//            for (auto v : make_iterator_range(vertices(O))) {
//                out << "v" << v << " [shape=record,label=\"" << v << "\"];\n";
//            }
//            for (auto e : make_iterator_range(edges(O))) {
//                const auto s = source(e, O);
//                const auto t = target(e, O);

//                const auto f = pheromone.find(std::make_pair(s, t));
//                assert (f != pheromone.end());
//                const auto w = std::log(f->second + 1.0) + 1.0;

//                out << "v" << s << " -> v" << t << " [penwidth=" << format("%.3f", w);
//                #if HAMILTONIAN_PATH_STRATEGY == 1
//                if (bestPath.count(std::make_pair(s, t)) == 1) {
//                    out << ",color=red";
//                }
//                #endif
//                out << "];\n";
//            }
//            out << "}\n\n";
//            out.flush();


            path.clear();

            if (converged == 3) {
                break;
            }

        }

        // If we converged to a solution but failed to find a valid hamiltonian path,
        // just restart the process. We're guaranteed to find one eventually.
        if (replacement.empty()) {

//            errs() << "!!! RESTART !!!\n";

            goto restart_process;
        }

//        errs() << "!!! CONVERGED !!!\n";

        assert (replacement.size() == numOfKernels);

        candidate.swap(replacement);

    }

public:

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief constructor
     ** ------------------------------------------------------------------------------------------------------------- */
    ProgramSchedulingAnalysisWorker(const SchedulingGraph & S,
                                    const PartitionOrderingGraph & O,
                                    const unsigned numOfKernels,
                                    const unsigned maxPathLength)
    : SchedulingAnalysisWorker(S, numOfKernels, numOfKernels)
    , O(O) {
        const auto n = num_vertices(O);
        visited.resize(n);
        index.resize(numOfKernels);
        path.reserve(maxPathLength);
        replacement.reserve(numOfKernels);
        toEval.reserve(numOfKernels);
        trail.reserve(num_edges(O));

        for (const auto e : make_iterator_range(edges(O))) {
            const unsigned u = source(e, O);
            const unsigned v = target(e, O);
            trail.emplace(std::make_pair(u, v), Trail{});
        }

    }

private:

    const PartitionOrderingGraph & O;

    BitVector visited;
    std::vector<std::pair<Vertex, double>> targets;

    struct Trail {
        double Pheromone = 0.0;
        double Permanence = 0.0;
    };

    flat_map<std::pair<Vertex, Vertex>, Trail> trail;
    std::vector<unsigned> index;
    Candidate path;
    Candidate replacement;
    Candidate toEval;

};

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief ProgramSchedulingAnalysis
 ** ------------------------------------------------------------------------------------------------------------- */
struct ProgramSchedulingAnalysis final : public SchedulingAnalysis {

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief initGA
     ** ------------------------------------------------------------------------------------------------------------- */
    bool initGA(Population & initialPopulation) override {

        bool r = true;

        const auto init_start = std::chrono::high_resolution_clock::now();

        Candidate candidate(candidateLength);
        std::iota(candidate.begin(), candidate.end(), 0);
        for (unsigned i = 0; i < INITIAL_SCHEDULING_POPULATION_ATTEMPTS; ++i) {
            std::shuffle(candidate.begin(), candidate.end(), rng);
            #ifdef ALLOW_ILLEGAL_PROGRAM_SCHEDULES_IN_EA_SET
            // we need to guarantee that our initial candidates are valid
            // hamiltonian paths or we become too dependent on the rng to
            // obtain one.
            worker.nearest_valid_schedule(candidate);
            #endif
            if (insertCandidate(candidate, initialPopulation)) {
                if (candidates.size() >= INITIAL_SCHEDULING_POPULATION_SIZE) {
                    r = false;
                    break;
                }
            }
        }

        const auto init_end = std::chrono::high_resolution_clock::now();
        init_time += (init_end - init_start).count();

        return r;
    }
    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief repair
     ** ------------------------------------------------------------------------------------------------------------- */
    void repair(Candidate & candidate) override {
        worker.repair(candidate);
    }

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief fitness
     ** ------------------------------------------------------------------------------------------------------------- */
    size_t fitness(const Candidate & candidate) override {
        return worker.fitness(candidate);
    }

    /** ------------------------------------------------------------------------------------------------------------- *
     * @brief constructor
     ** ------------------------------------------------------------------------------------------------------------- */
    ProgramSchedulingAnalysis(const SchedulingGraph & S,
                              const PartitionOrderingGraph & O,
                              const unsigned numOfKernels,
                              const unsigned maxPathLength)
    : SchedulingAnalysis(numOfKernels)
    , worker(S, O, numOfKernels, maxPathLength) {

    }

private:


    ProgramSchedulingAnalysisWorker worker;


};

} // end of anonymous namespace

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief analyzeDataflowWithinPartitions
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineAnalysis::analyzeDataflowWithinPartitions(PartitionGraph & P) const {

    using SchedulingMap = flat_map<Vertex, Vertex>;

    /// --------------------------------------------
    /// Construct our partition schedules
    /// --------------------------------------------

    const auto cfg = Z3_mk_config();
    Z3_set_param_value(cfg, "model", "true");
    Z3_set_param_value(cfg, "proof", "false");
    const auto ctx = Z3_mk_context(cfg);
    Z3_del_config(cfg);

    for (unsigned currentPartitionId = 1; currentPartitionId < PartitionCount; ++currentPartitionId) {

        // We begin by constructing a subgraph of this partition with just enough information to
        // form a bipartite graph of the kernel and streamset nodes.

        // The input streamset(s) to this partition are surpressed from the graph but any output
        // streamsets with an internal user will be recorded but flagged as External. Memory
        // colouring only wants to consider purely local streamsets but ought to consider
        // otherwise equivalent schedules that place kernels that produce outputs towards the
        // end of the schedule as superior.

        /// -----------------------------------------------------------------
        /// Identify the nodes / streamsets belonging to our i-th partition
        /// -----------------------------------------------------------------

        PartitionData & currentPartition = P[currentPartitionId];

        const auto & kernels = currentPartition.Kernels;
        const auto numOfKernels = kernels.size();

        auto S = makeIntraPartitionSchedulingGraph(P, currentPartitionId);

        BEGIN_SCOPED_REGION

        /// ------------------------------------------------------------------------
        /// Determine how many "invocations" are required by each kernel so that we
        /// can correctly scale the streamset sizes based on the dataflow rates
        /// ------------------------------------------------------------------------

        const auto solver = Z3_mk_solver(ctx);
        Z3_solver_inc_ref(ctx, solver);

        const auto varType = Z3_mk_real_sort(ctx);

        auto constant = [&](const Rational value) {
            return Z3_mk_real(ctx, value.numerator(), value.denominator());
        };

        const auto ONE = constant(1);

        auto free_variable = [&]() {
            auto v = Z3_mk_fresh_const(ctx, nullptr, varType);
            auto c1 = Z3_mk_ge(ctx, v, ONE);
            Z3_solver_assert(ctx, solver, c1);
            return v;
        };

        auto multiply =[&](Z3_ast X, Z3_ast Y) {
            assert (X);
            assert (Y);
            Z3_ast args[2] = { X, Y };
            return Z3_mk_mul(ctx, 2, args);
        };

        auto assert_equals =[&](Z3_ast X, Z3_ast Y) {
            assert (X);
            assert (Y);
            const auto Z = Z3_mk_eq(ctx, X, Y);
            Z3_solver_assert(ctx, solver, Z);
        };

        const auto n = num_vertices(S);

        std::vector<Z3_ast> VarList(n, nullptr);

        for (unsigned u = 0; u < numOfKernels; ++u) {
            assert (S[u].Type == SchedulingNode::IsKernel);
            VarList[u] = free_variable();
            for (const auto e : make_iterator_range(out_edges(u, S))) {
                const auto streamSet = target(e, S);
                const auto fixedRateVal = constant(S[e]);
                const auto producedRate = multiply(VarList[u], fixedRateVal);
                VarList[streamSet] = producedRate;
            }
        }
        for (unsigned u = 0; u < numOfKernels; ++u) {
            assert (S[u].Type == SchedulingNode::IsKernel);
            for (const auto e : make_iterator_range(in_edges(u, S))) {
                const auto streamSet = source(e, S);
                const auto producedRate = VarList[streamSet];
                const auto fixedRateVal = constant(S[e]);
                const auto consumedRate = multiply(VarList[u], fixedRateVal);
                assert_equals(producedRate, consumedRate);
            }
        }

        if (LLVM_UNLIKELY(Z3_solver_check(ctx, solver) == Z3_L_FALSE)) {
            report_fatal_error("Z3 failed to find a solution to synchronous dataflow graph");
        }

        const auto model = Z3_solver_get_model(ctx, solver);
        Z3_model_inc_ref(ctx, model);

        size_t lcm = 1;

        for (unsigned u = 0; u < numOfKernels; ++u) {
            assert (S[u].Type == SchedulingNode::IsKernel);
            assert(VarList[u]);
            Z3_ast value;
            if (LLVM_UNLIKELY(Z3_model_eval(ctx, model, VarList[u], Z3_L_TRUE, &value) != Z3_L_TRUE)) {
                report_fatal_error("Unexpected Z3 error when attempting to obtain value from model!");
            }

            __int64 num, denom;
            if (LLVM_UNLIKELY(Z3_get_numeral_rational_int64(ctx, value, &num, &denom) != Z3_L_TRUE)) {
                report_fatal_error("Unexpected Z3 error when attempting to convert model value to number!");
            }
            assert (num > 0);

            // scale each streamSet node size field by the replication vector
            const Rational replicationFactor{num, denom};

            auto & Su = S[u];
            Su.Size = replicationFactor;
            lcm = boost::lcm(lcm, Su.Size.denominator());
        }

        Z3_model_dec_ref(ctx, model);
        Z3_solver_dec_ref(ctx, solver);

        // NOTE: the LCM should always be 1 here but Z3 converges on a solution faster when rational numbers
        // are allowed so the following handles any degenerate solution. The only time I've seen this occur
        // is when a non-power of 2 segment size was given.

        if (LLVM_UNLIKELY(lcm != 1)) {
            for (unsigned u = 0; u < numOfKernels; ++u) {
                auto & replicationFactor = S[u].Size;
                replicationFactor *= lcm;
                assert (replicationFactor.denominator() == 1);
            }
        }

        currentPartition.Repetitions.resize(numOfKernels);

        for (unsigned u = 0; u < numOfKernels; ++u) {
            const auto & replicationFactor = S[u].Size;
            for (const auto e : make_iterator_range(out_edges(u, S))) {
                const auto v = target(e, S);
                assert (u != v);
                auto & Sv = S[v];
                assert (Sv.Type == SchedulingNode::IsStreamSet);
                const auto & itemsPerStride = S[e];
                Sv.Size *= (itemsPerStride * replicationFactor);
            }
            assert (replicationFactor.denominator() == 1);
            currentPartition.Repetitions[u] = replicationFactor.numerator();
        }

        END_SCOPED_REGION

        // We want to generate a subgraph of S consisting of only the kernel nodes
        // but whose edges initially represent the transitive closure of S. Once we
        // generate this graph, we remove the edges associated with the streamsets.
        // The final graph is the kernel dependency graph of S.

        // TODO: we ought to reason about paths in D independently since they are
        // subgraphs of S with a single topological ordering.

        const auto D = makePartitionDependencyGraph(currentPartition);

        // Now we begin the genetic algorithm phase; our overall goal is to find a schedule that
        // permits a minimum memory schedule.

        PartitionSchedulingAnalysis SA(S, D, numOfKernels);

        SA.runGA(currentPartition.Orderings);

    }

    Z3_reset_memory();
    Z3_del_context(ctx);
}


/** ------------------------------------------------------------------------------------------------------------- *
 * @brief makeIntraPartitionSchedulingGraph
 ** ------------------------------------------------------------------------------------------------------------- */
SchedulingGraph PipelineAnalysis::makeIntraPartitionSchedulingGraph(const PartitionGraph & P,
                                                                    const unsigned currentPartitionId) const {

    const PartitionData & currentPartition = P[currentPartitionId];

    const auto & kernels = currentPartition.Kernels;
    const auto numOfKernels = kernels.size();

    #warning add fake source/sink to each partition to reason about external streamsets?

    flat_set<Vertex> streamSets;

    for (const auto u : kernels) {
        const RelationshipNode & node = Relationships[u];
        assert (node.Type == RelationshipNode::IsKernel);
        for (const auto e : make_iterator_range(in_edges(u, Relationships))) {
            const auto binding = source(e, Relationships);
            if (Relationships[binding].Type == RelationshipNode::IsBinding) {
                const auto f = first_in_edge(binding, Relationships);
                assert (Relationships[f].Reason != ReasonType::Reference);
                const auto streamSet = source(f, Relationships);
                assert (Relationships[streamSet].Type == RelationshipNode::IsRelationship);
                assert (isa<StreamSet>(Relationships[streamSet].Relationship));
                streamSets.insert(streamSet);
            }
        }
//        for (const auto e : make_iterator_range(out_edges(u, Relationships))) {
//            const auto binding = target(e, Relationships);
//            if (Relationships[binding].Type == RelationshipNode::IsBinding) {
//                const auto f = first_out_edge(binding, Relationships);
//                assert (Relationships[f].Reason != ReasonType::Reference);
//                const auto streamSet = target(f, Relationships);
//                assert (Relationships[streamSet].Type == RelationshipNode::IsRelationship);
//                assert (isa<StreamSet>(Relationships[streamSet].Relationship));
//                streamSets.insert(streamSet);
//            }
//        }
    }

    flat_set<Vertex> externalStreamSet;
    for (const auto e : make_iterator_range(in_edges(currentPartitionId, P))) {
        externalStreamSet.insert(P[e]);
    }
//    for (const auto e : make_iterator_range(out_edges(currentPartitionId, P))) {
//        externalStreamSet.insert(P[e]);
//    }

    std::vector<Vertex> internalStreamSet;

    internalStreamSet.reserve(streamSets.size() - externalStreamSet.size());

    std::set_difference(streamSets.begin(), streamSets.end(),
                        externalStreamSet.begin(), externalStreamSet.end(),
                        std::back_inserter(internalStreamSet));

    const auto numOfStreamSets = internalStreamSet.size();

    SchedulingGraph G(numOfKernels + numOfStreamSets);

    for (unsigned i = 0; i < numOfKernels; ++i) {
        SchedulingNode & N = G[i];
        N.Type = SchedulingNode::IsKernel;
    }

    const auto firstStreamSet = numOfKernels;

    for (unsigned i = 0; i < numOfKernels; ++i) {
        const auto u = kernels[i];
        const RelationshipNode & node = Relationships[u];
        assert (node.Type == RelationshipNode::IsKernel);

        const auto strideSize = node.Kernel->getStride();

        for (const auto e : make_iterator_range(in_edges(u, Relationships))) {
            const auto binding = source(e, Relationships);
            if (Relationships[binding].Type == RelationshipNode::IsBinding) {
                const auto f = first_in_edge(binding, Relationships);
                assert (Relationships[f].Reason != ReasonType::Reference);
                const auto streamSet = source(f, Relationships);
                assert (Relationships[streamSet].Type == RelationshipNode::IsRelationship);
                assert (isa<StreamSet>(Relationships[streamSet].Relationship));
                const auto g = std::find(internalStreamSet.begin(), internalStreamSet.end(), streamSet);
                if (g != internalStreamSet.end()) {

                    const auto j = std::distance(internalStreamSet.begin(), g);

                    const RelationshipNode & rn = Relationships[binding];
                    const Binding & b = rn.Binding;
                    const ProcessingRate & rate = b.getRate();

                    // If we have a PopCount producer/consumer in the same partition,
                    // they're both perform an identical number of strides. So long
                    // as the producing/consuming strideRate match, the equation will
                    // work. Since the lower bound of PopCounts is 0, we always use the
                    // upper bound.
                    Rational itemsPerStride{rate.getUpperBound() * strideSize};
                    add_edge(firstStreamSet + j, i, itemsPerStride, G);
                }
            }
        }

        for (const auto e : make_iterator_range(out_edges(u, Relationships))) {
            const auto binding = target(e, Relationships);
            if (Relationships[binding].Type == RelationshipNode::IsBinding) {
                const auto f = first_out_edge(binding, Relationships);
                assert (Relationships[f].Reason != ReasonType::Reference);
                const auto streamSet = target(f, Relationships);
                assert (Relationships[streamSet].Type == RelationshipNode::IsRelationship);
                assert (isa<StreamSet>(Relationships[streamSet].Relationship));

                const auto g = std::find(internalStreamSet.begin(), internalStreamSet.end(), streamSet);
                if (g != internalStreamSet.end()) {
                    const auto j = std::distance(internalStreamSet.begin(), g);
                    const RelationshipNode & rn = Relationships[binding];
                    const Binding & b = rn.Binding;

                    const Rational bytesPerItem{b.getFieldWidth() * b.getNumElements(), 8};

                    SchedulingNode & SN = G[firstStreamSet + j];
                    SN.Type = SchedulingNode::IsStreamSet;
                    SN.Size = bytesPerItem;

                    const ProcessingRate & rate = b.getRate();
                    Rational itemsPerStride{rate.getUpperBound() * strideSize};

                    add_edge(i, firstStreamSet + j, itemsPerStride, G);
                }
            }
        }
    }

    return G;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief makePartitionDependencyGraph
 ** ------------------------------------------------------------------------------------------------------------- */
PartitionDependencyGraph PipelineAnalysis::makePartitionDependencyGraph(const PartitionData & currentPartition) const {

    const auto & K = currentPartition.Kernels;
    const auto numOfKernels = K.size();

    PartitionDependencyGraph G(numOfKernels);

    flat_set<Vertex> consumers;
    for (unsigned i = 0; i < numOfKernels; ++i) {
        const auto u = K[i];
        assert (Relationships[u].Type == RelationshipNode::IsKernel);
        for (const auto e : make_iterator_range(out_edges(u, Relationships))) {
            const auto output = target(e, Relationships);
            if (Relationships[output].Type == RelationshipNode::IsBinding) {
                const auto f = first_out_edge(output, Relationships);
                assert (Relationships[f].Reason != ReasonType::Reference);
                const auto streamSet = target(f, Relationships);
                assert (Relationships[streamSet].Type == RelationshipNode::IsRelationship);
                assert (isa<StreamSet>(Relationships[streamSet].Relationship));
                for (const auto e : make_iterator_range(out_edges(streamSet, Relationships))) {
                    const auto input = target(e, Relationships);
                    if (Relationships[input].Type == RelationshipNode::IsBinding) {
                        const auto g = first_out_edge(input, Relationships);
                        assert (Relationships[f].Reason != ReasonType::Reference);
                        const auto consumer = target(g, Relationships);
                        assert (Relationships[consumer].Type == RelationshipNode::IsKernel);
                        consumers.insert(consumer);
                    }
                }
            }
        }
        for (const auto v : consumers) {
            const auto f = std::find(K.begin(), K.end(), v);
            if (f != K.end()) {
                const auto j = std::distance(K.begin(), f);
                add_edge(i, j, G);
            }
        }
        consumers.clear();
    }

    reverse_traversal ordering{numOfKernels};
    assert (is_valid_topological_sorting(ordering, G));
    transitive_reduction_dag(ordering, G);

    return G;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief analyzeDataflowBetweenPartitions
 ** ------------------------------------------------------------------------------------------------------------- */
PartitionDataflowGraph PipelineAnalysis::analyzeDataflowBetweenPartitions(PartitionGraph & P) const {

    using DataflowMap = flat_map<Vertex, Vertex>;

    const auto activePartitions = (PartitionCount - 1);

    // create a bipartite graph consisting of partitions and cross-partition
    // streamset nodes and relationships

    flat_set<Vertex> streamSets;

    for (unsigned partitionId = 1; partitionId < PartitionCount; ++partitionId) {
        for (const auto e : make_iterator_range(out_edges(partitionId, P))) {
            const auto streamSet = P[e];
            assert (streamSet < num_vertices(Relationships));
            assert (Relationships[streamSet].Type == RelationshipNode::IsRelationship);
            assert (isa<StreamSet>(Relationships[streamSet].Relationship));
            streamSets.insert(streamSet);
        }
    }

    const auto numOfStreamSets = streamSets.size();

    PartitionDataflowGraph G(activePartitions + numOfStreamSets);

    for (unsigned streamSetNode = 0; streamSetNode < numOfStreamSets; ++streamSetNode) {
        const auto streamSet = *streamSets.nth(streamSetNode);
        for (const auto e : make_iterator_range(in_edges(streamSet, Relationships))) {
            const auto binding = source(e, Relationships);
            const RelationshipNode & output = Relationships[binding];
            if (LLVM_LIKELY(output.Type == RelationshipNode::IsBinding)) {
                const auto f = first_in_edge(binding, Relationships);
                assert (Relationships[f].Reason != ReasonType::Reference);
                const unsigned producer = source(f, Relationships);
                const RelationshipNode & node = Relationships[producer];
                assert (node.Type == RelationshipNode::IsKernel);

                const Binding & b = output.Binding;
                const Rational bytesPerItem{b.getFieldWidth() * b.getNumElements(), 8};

                const auto k = activePartitions + streamSetNode;

                G[k] = bytesPerItem;

                const auto g = PartitionIds.find(producer);
                assert (g != PartitionIds.end());
                const auto partitionId = g->second;

                const PartitionData & N = P[partitionId];
                const auto & K = N.Kernels;
                const auto h = std::find(K.begin(), K.end(), producer);
                assert (h != K.end());
                const auto i = std::distance(K.begin(), h);

                const auto strideSize = node.Kernel->getStride() * N.Repetitions[i];

                const ProcessingRate & rate = b.getRate();
                const auto sum = rate.getLowerBound() + rate.getUpperBound();
                const auto expected = sum * Rational{strideSize, 2};
                add_edge(partitionId - 1, k, PartitionDataflowEdge{producer, rate.getKind(), expected}, G);
            }
        }
    }

    for (unsigned partitionId = 1; partitionId < PartitionCount; ++partitionId) {
        const PartitionData & N = P[partitionId];
        const auto & K = N.Kernels;
        for (const auto e : make_iterator_range(in_edges(partitionId, P))) {
            const auto streamSet = P[e];
            assert (streamSet < num_vertices(Relationships));
            assert (Relationships[streamSet].Type == RelationshipNode::IsRelationship);
            assert (isa<StreamSet>(Relationships[streamSet].Relationship));
            const auto f = streamSets.find(streamSet);
            assert (f != streamSets.end());
            const auto streamSetNode = std::distance(streamSets.begin(), f);
            const auto k = activePartitions + streamSetNode;

            for (const auto e : make_iterator_range(out_edges(streamSet, Relationships))) {
                const auto binding = target(e, Relationships);
                const RelationshipNode & input = Relationships[binding];
                if (LLVM_LIKELY(input.Type == RelationshipNode::IsBinding)) {
                    const auto f = first_out_edge(binding, Relationships);
                    assert (Relationships[f].Reason != ReasonType::Reference);
                    const unsigned consumer = target(f, Relationships);
                    const RelationshipNode & node = Relationships[consumer];
                    assert (node.Type == RelationshipNode::IsKernel);
                    const auto g = std::find(K.begin(), K.end(), consumer);
                    if (g != K.end()) {
                        const auto i = std::distance(K.begin(), g);
                        const auto strideSize = node.Kernel->getStride() * N.Repetitions[i];
                        const Binding & b = input.Binding;
                        const ProcessingRate & rate = b.getRate();
                        const auto sum = rate.getLowerBound() + rate.getUpperBound();
                        const auto expected = sum * Rational{strideSize, 2};
                        add_edge(k, partitionId - 1, PartitionDataflowEdge{consumer, rate.getKind(), expected}, G);
                    }
                }
            }
        }

    }

    #warning incorporate length equality assertions

    const auto cfg = Z3_mk_config();
    Z3_set_param_value(cfg, "model", "true");
    Z3_set_param_value(cfg, "proof", "false");
    const auto ctx = Z3_mk_context(cfg);
    Z3_del_config(cfg);

    const auto varType = Z3_mk_real_sort(ctx);

    auto constant = [&](const Rational value) {
        return Z3_mk_real(ctx, value.numerator(), value.denominator());
    };

    const auto ONE = constant(1);

    std::vector<Rational> expectedStrides(activePartitions);

    std::vector<Z3_ast> assumptions;

    const auto n = num_vertices(G);

    std::vector<Z3_ast> VarList(activePartitions);

    const auto solver = Z3_mk_solver(ctx);
    Z3_solver_inc_ref(ctx, solver);

    auto free_variable = [&]() {
        auto v = Z3_mk_fresh_const(ctx, nullptr, varType);
        auto c1 = Z3_mk_ge(ctx, v, ONE);
        Z3_solver_assert(ctx, solver, c1);
        return v;
    };

    auto multiply =[&](Z3_ast X, Z3_ast Y) {
        Z3_ast args[2] = { X, Y };
        return Z3_mk_mul(ctx, 2, args);
    };

    for (unsigned i = 0; i < activePartitions; ++i) {
        VarList[i] = free_variable();
    }

    #warning this doesn't correctly handle greedy/unknown rates

    for (unsigned streamSet = activePartitions; streamSet < n; ++streamSet) {
        const auto output = in_edge(streamSet, G);
        const auto outputRate = constant(G[output].Expected);
        const auto producer = source(output, G);
        assert (producer < activePartitions);
        const auto outputRateVar = multiply(VarList[producer], outputRate);
        for (const auto input : make_iterator_range(out_edges(streamSet, G))) {
            const auto inputRate = constant(G[input].Expected);
            const auto consumer = target(input, G);
            assert (consumer < activePartitions);
            const auto inputRateVar = multiply(VarList[consumer], inputRate);
            assumptions.push_back(Z3_mk_eq(ctx, outputRateVar, inputRateVar));
        }
    }

    const auto m = Z3_maxsat(ctx, solver, assumptions);
    if (LLVM_UNLIKELY(m == 0)) {
        Z3_solver_pop(ctx, solver, 1);
        Z3_solver_check(ctx, solver);
    }

    const auto model = Z3_solver_get_model(ctx, solver);
    Z3_model_inc_ref(ctx, model);
    for (unsigned i = 0; i < activePartitions; ++i) {

        Z3_ast const stridesPerSegmentVar = VarList[i];
        Z3_ast value;
        if (LLVM_UNLIKELY(Z3_model_eval(ctx, model, stridesPerSegmentVar, Z3_L_TRUE, &value) != Z3_L_TRUE)) {
            report_fatal_error("Unexpected Z3 error when attempting to obtain value from model!");
        }

        __int64 num, denom;
        if (LLVM_UNLIKELY(Z3_get_numeral_rational_int64(ctx, value, &num, &denom) != Z3_L_TRUE)) {
            report_fatal_error("Unexpected Z3 error when attempting to convert model value to number!");
        }
        assert (num > 0);

        expectedStrides[i] = Rational{num, denom};
    }
    Z3_model_dec_ref(ctx, model);
    Z3_solver_dec_ref(ctx, solver);

    Z3_reset_memory();
    Z3_del_context(ctx);

    for (unsigned partitionId = 1; partitionId < PartitionCount; ++partitionId) {
        PartitionData & N = P[partitionId];
        N.ExpectedRepetitions = expectedStrides[partitionId - 1];
    }

    return G;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief makeInterPartitionSchedulingGraph
 ** ------------------------------------------------------------------------------------------------------------- */
PartitionOrdering PipelineAnalysis::makeInterPartitionSchedulingGraph(PartitionGraph & P,
                                                                      const PartitionDataflowGraph & D) const {

    // Our goal is to find a topological ordering of the partitions such that
    // (1) the distance each partition can "jump" (i.e. the number of subsequent
    // partitions it can safely skip given the observation that if this partition
    // produces no data, any partition that is strictly dominated by the output
    // of this partition cannot either) is maximal and (2) the expected memory
    // usage is minimal.

    // To satisfy (1), we know that every topological ordering that we could want
    // is a depth-first ordering of the transitive reduction of D.

    // We begin this algorithm by constructing an auxillary graph H in which
    // any *hamiltonian path* through H would be a valid topological ordering
    // of D. We then use H to construct a more complicated graph that contains
    // the kernel nodes that have cross-partition I/O and return it to the user.

    using BV = dynamic_bitset<>;
    using PathGraph = adjacency_list<hash_setS, vecS, bidirectionalS>;

    const auto activePartitions = (PartitionCount - 1);

    PathGraph H(activePartitions + 2);

    flat_set<unsigned> kernels;
    kernels.reserve(PartitionIds.size());

    // since we could have multiple source/sink nodes in P, we always
    // add two fake nodes to H for a common source/sink.

    const auto l = activePartitions + 2U;

    BV M(l);

    for (unsigned i = 0; i < activePartitions; ++i) {

        if (in_degree(i, D) == 0) {
            add_edge(0, i + 1U, H);
        } else {
            for (const auto e : make_iterator_range(in_edges(i, D))) {
                const PartitionDataflowEdge & E = D[e];
                kernels.insert(E.KernelId);
            }
        }

        if (out_degree(i, D) == 0) {
            add_edge(i + 1U, activePartitions + 1U, H);
        } else {
            assert (M.none());
            for (const auto e : make_iterator_range(out_edges(i, D))) {
                const PartitionDataflowEdge & E = D[e];
                assert (Relationships[E.KernelId].Type == RelationshipNode::IsKernel);
                kernels.insert(E.KernelId);
                const auto streamSet = target(e, D);
                assert (streamSet >= activePartitions);
                for (const auto f : make_iterator_range(out_edges(streamSet, D))) {
                    assert (Relationships[D[f].KernelId].Type == RelationshipNode::IsKernel);
                    const auto k = target(f, D);
                    assert (i < k && k < activePartitions);
                    M.set(k);
                }
            }
            assert (M.any());
            for (auto j = M.find_first(); j != BV::npos; j = M.find_next(j)) {
                add_edge(i + 1U, j + 1U, H);
            }
            M.reset();
        }
        assert (in_degree(i + 1, H) > 0);
        assert (out_degree(i + 1, H) > 0);
    }

    BEGIN_SCOPED_REGION
    const reverse_traversal ordering{l};
    assert (is_valid_topological_sorting(ordering, H));
    transitive_closure_dag(ordering, H);
    transitive_reduction_dag(ordering, H);
    END_SCOPED_REGION

    // To find our hamiltonian path later, we need a path from each join
    // in the graph to the other forked paths (including the implicit
    // "terminal" node.) Compute the post-dominator tree of H then insert
    // the appropriate edges from the immediate predecessor of each join
    // to the child of each dominating fork.

    BEGIN_SCOPED_REGION

    std::vector<BV> postdom(l);

    // We use Lengauer-Tarjan algorithm but since we know H is acyclic,
    // we know that we reach the fix-point after a single round.

    for (unsigned i = l; i--; ) { // reverse topological ordering
        BV & P = postdom[i];
        P.resize(activePartitions + 2);
        if (LLVM_LIKELY(out_degree(i, H) > 0)) {
            P.set(); // set all to 1
            for (const auto e : make_iterator_range(out_edges(i, H))) {
                const auto v = target(e, H);
                // assert (v > i);
                const BV & D = postdom[v];
                assert (D.size() == (activePartitions + 2));
                assert (D.test(v));
                P &= D;
            }
        }
        P.set(i);
    }

    std::vector<unsigned> rank(l);
    for (unsigned i = 0; i < l; ++i) { // forward topological ordering
        unsigned newRank = 0;
        for (const auto e : make_iterator_range(in_edges(i, H))) {
            newRank = std::max(newRank, rank[source(e, H)]);
        }
        rank[i] = newRank + 1;
    }

    std::vector<unsigned> occurences(l);
    std::vector<unsigned> singleton(l);

    std::vector<std::bitset<2>> ancestors(l);

    flat_set<std::pair<Vertex, Vertex>> toAdd;

    for (unsigned i = 0; i < l; ++i) {  // forward topological ordering
        const auto d = in_degree(i, H);
        if (d > 1) {

            PathGraph::in_edge_iterator begin, end;
            std::tie(begin, end) = in_edges(i, H);

            for (auto ei = begin; (++ei) != end; ) {
                const auto x = source(*ei, H);
                for (auto ej = begin; ej != ei; ++ej) {
                    const auto y = source(*ej, H);

                    // Determine the common ancestors of each input to node_i
                    for (unsigned j = 0; j < l; ++j) {
                        ancestors[j].reset();
                    }
                    ancestors[x].set(0);
                    ancestors[y].set(1);

                    std::fill_n(occurences.begin(), rank[i] - 1, 0);
                    for (auto j = l; j--; ) { // reverse topological ordering
                        for (const auto e : make_iterator_range(out_edges(j, H))) {
                            const auto v = target(e, H);
                            ancestors[j] |= ancestors[v];
                        }
                        if (ancestors[j].all()) {
                            const auto k = rank[j];
                            occurences[k]++;
                            singleton[k] = j;
                        }
                    }
                    // Now scan again through them to determine the single ancestor
                    // to the pair of inputs that is of highest rank.
                    auto lca = i;
                    for (auto j = rank[i] - 1; j--; ) {
                        if (occurences[j] == 1) {
                            lca = singleton[j];
                            break;
                        }
                    }

                    assert (lca < i);

                    const BV & Px = postdom[x];
                    const BV & Py = postdom[y];

                    for (const auto e : make_iterator_range(out_edges(lca, H))) {
                        const auto z = target(e, H);
                        const BV & Pz = postdom[z];

                        // Do not arc back to the start of a dominating path.

                        // NOTE: we delay adding the edges to H to prevent any changes
                        // to the in degree of a vertex we have not yet visited on a
                        // parallel path from being given an unintended edge.

                        if (!Px.is_subset_of(Pz)) {
                            toAdd.emplace(x, z);
                        }

                        if (!Py.is_subset_of(Pz)) {
                            toAdd.emplace(y, z);
                        }
                    }
                }
            }
        }
    }

    for (const auto & e : toAdd) {
        add_edge(e.first, e.second, H);
    }

    END_SCOPED_REGION

    // Each partition has one or more optimal orderings of kernel invocations.
    // We filter each ordering trie to only contain the kernels with cross-
    // partition I/O and then compute the minimal acyclic dfa. The edges
    // (i.e., line graph) of those subgraphs will map to nodes in the subsequent
    // ordering graph.

    std::vector<OrderingDAWG> partitionSubgraphs(activePartitions);

    unsigned numOfLineGraphNodes = 0;

    BEGIN_SCOPED_REGION

    for (unsigned i = 0; i < activePartitions; ++i) {

        PartitionData & D = P[i + 1U];

        const OrderingDAWG & O = D.Orderings;
        assert (num_vertices(O) > 0);
        const auto & K = D.Kernels;

        const auto n = K.size();

        assert (num_edges(O) >= (n - 1));

        OrderingDAWG & G = partitionSubgraphs[i];
        assert (num_vertices(G) == 0);
        add_vertex(G);
        std::function<void(unsigned, unsigned)> filter_trie = [&](const unsigned u, const unsigned i) {
            for (const auto e : make_iterator_range(out_edges(i, O))) {
                const auto t = O[e];
                assert (t < n);
                auto v = u;
                // Is kernel "t" a kernel with cross partition I/O?
                const auto f = kernels.find(K[t]);
                if (f != kernels.end()) {
                    const auto k = std::distance(kernels.begin(), f);
                    v = add_vertex(G);
                    add_edge(u, v, k, G);
                }
                filter_trie(v, target(e, O));
            }
        };
        filter_trie(0, 0);
        postorder_minimize(G);

        // printDAWG(G, errs(), "P_" + std::to_string(i));

        numOfLineGraphNodes += num_edges(G);
    }

    END_SCOPED_REGION

    // We now have our auxillary graph complete and are ready to construct our
    // scheduling graph G.

    const auto firstKernelNode = (2U * activePartitions) + 2U;

    const auto n = firstKernelNode + numOfLineGraphNodes;

    PartitionOrderingGraph G(n);

    // Split every node except for the common sink/source in H into two nodes in G;
    // this will form the backbone of the partition scheduling constraint graph

    for (unsigned i = 0; i <= activePartitions; ++i) {
        const auto partitionOut = (i * 2);
        for (const auto e : make_iterator_range(out_edges(i, H))) {
            const auto j = target(e, H);
            const auto outgoing = (j * 2) - 1;
            assert (outgoing < firstKernelNode);
            add_edge(partitionOut, outgoing, HAMILTONIAN_PATH_DEFAULT_WEIGHT, G);
        }
    }

    // Insert the line graph of each partition DAWG between the partition nodes.

    BEGIN_SCOPED_REGION

    flat_map<OrderingDAWG::edge_descriptor, Vertex> mapping;

    auto get = [&mapping](const OrderingDAWG::edge_descriptor & e) {
        const auto f = mapping.find(e);
        assert (f != mapping.end());
        return f->second;
    };

    for (unsigned i = 0, nextKernelNode = firstKernelNode; i < activePartitions; ++i) {

        const OrderingDAWG & H = partitionSubgraphs[i];

        const Vertex partitionIn = (i * 2) + 1;
        const Vertex partitionOut = (i * 2) + 2;

        mapping.reserve(num_edges(H));

        for (const auto e : make_iterator_range(edges(H))) {
            const auto u = nextKernelNode++;
            assert (u < n);
            auto & A = G[u];
            assert (A.empty());
            const auto k = H[e];
            assert (k < kernels.size());
            A.push_back(k);
            mapping.emplace(e, u);
        }

        for (const auto u : make_iterator_range(vertices(H))) {
            if (in_degree(u, H) == 0) {
                for (const auto e : make_iterator_range(out_edges(u, H))) {
                    add_edge(partitionIn, get(e), G);
                }
            } else if (out_degree(u, H) == 0) {
                for (const auto e : make_iterator_range(in_edges(u, H))) {
                    add_edge(get(e), partitionOut, G);
                }
            } else {
                for (const auto e : make_iterator_range(in_edges(u, H))) {
                    const auto v = get(e);
                    for (const auto f : make_iterator_range(out_edges(u, H))) {
                        const auto w = get(f);
                        add_edge(v, w, G);
                    }
                }
            }
        }

        mapping.clear();
    }

    END_SCOPED_REGION

    assert (out_degree(firstKernelNode - 1, G) == 0);

    auto printOrderingGraph = [&](const PartitionOrderingGraph & GP, raw_ostream & out, const StringRef name) {
        out << "digraph \"" << name << "\" {\n";
        for (auto v : make_iterator_range(vertices(GP))) {
            const auto & V = GP[v];
            out << "v" << v << " [shape=record,label=\"";
            bool addComma = false;
            for (const auto k : V) {
                if (addComma) out << ',';
                out << *kernels.nth(k);
                addComma = true;
            }
            out << "\"];\n";
        }
        for (auto e : make_iterator_range(edges(GP))) {
            const auto s = source(e, GP);
            const auto t = target(e, GP);
            out << "v" << s << " -> v" << t << ";\n";
        }
        out << "}\n\n";
        out.flush();

    };

    for (;;) {
        bool unchanged = true;
        // Even though G is likely cyclic, it was constructed from an acyclic
        // graph whose vertices were indexed in topological order. Traversing
        // from the last to first tends to reach the fixpoint faster.
        for (unsigned i = n; i--; ) {
try_to_compress_further:
            if (out_degree(i, G) == 1) {
                const auto j = child(i, G);
                if (in_degree(j, G) == 1) {
                    auto & A = G[i];
                    const auto & B = G[j];
                    A.insert(A.end(), B.begin(), B.end());
                    for (const auto e : make_iterator_range(out_edges(j, G))) {
                        add_edge(i, target(e, G), G[e], G);
                    }
                    clear_vertex(j, G);
                    G[j].clear();
                    unchanged = false;
                    goto try_to_compress_further;
                }
            }
        }
        if (unchanged) break;
    }

    // Since deleting vertices from a vector based graph is difficult with
    // boost::graph, regenerate G sans any isolated nodes.

    unsigned numOfKernelSets = 0;
    unsigned numOfEmptyNodes = 0;
    #ifndef NDEBUG
    std::vector<unsigned> index(n, -1U);
    #else
    std::vector<unsigned> index(n);
    #endif
    for (unsigned i = 0; i < n; ++i) {

        const auto & V = G[i];
        if (!V.empty()) {
            ++numOfKernelSets;
        } else if (in_degree(i, G) > 0 || out_degree(i, G) > 0) {
            ++numOfEmptyNodes;
        }

    }

    const auto m = (numOfKernelSets + numOfEmptyNodes);

    assert (m > 0);

    PartitionOrderingGraph GP(m);

    for (unsigned i = 0, j = numOfKernelSets, k = 0; i < n; ++i) {
        auto & V = G[i];

        if (V.empty() && (in_degree(i, G) == 0 && out_degree(i, G) == 0)) {
            continue;
        }

        unsigned t;
        if (V.empty()) {
            t = j++;
        } else {
            t = k++;
        }
        index[i] = t;
        GP[t] = std::move(V);
    }

    for (const auto e : make_iterator_range(edges(G))) {
        const auto u = index[source(e, G)];
        assert (u < m);
        const auto v = index[target(e, G)];
        assert (v < m);
        add_edge(u, v, GP);
    }

    // printOrderingGraph(GP, errs(), "O");

    return PartitionOrdering{std::move(GP), numOfKernelSets, std::move(kernels)};
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief scheduleProgramGraph
 ** ------------------------------------------------------------------------------------------------------------- */
std::vector<unsigned> PipelineAnalysis::scheduleProgramGraph(
        const PartitionGraph & P,
        const PartitionOrdering & partitionOrdering,
        const PartitionDataflowGraph & D) const {

    auto & O = partitionOrdering.Graph;
    auto & kernels = partitionOrdering.Kernels;

    assert (PartitionCount > 0);

    const auto activePartitions = (PartitionCount - 1);

    const auto numOfFrontierKernels = kernels.size();

    const auto maxPathLength = (2U * activePartitions) + 2U + numOfFrontierKernels;

    assert (num_vertices(D) >= activePartitions);

    const auto lastStreamSet = num_vertices(D);
    const auto firstStreamSet = activePartitions;

    const auto numOfStreamSets = lastStreamSet - firstStreamSet;

#if 0

    // Our ordering graph will be used to generate and validate our legal kernel invocation orderings
    // but we still need to create a DAG to indicate the relationships between our kernels and
    // streamset nodes to evaluate with the GA.

    // Prior to starting the GA, we need to construct a scheduling graph. Since we did path compression,
    // we want to take advantage of the fact some of the kernels and streamsets may have forced
    // relationships that we can amalgamate to simplify the resulting graph. By removing kernels due
    // to path compression, we avoid many trivially impossible hamiltonian paths, reducing work in the
    // repair algorithm. By removing streamsets, we eliminate work in the fitness function.

    std::vector<unsigned> kernelSet(numOfFrontierKernels);

    // Identify which kernels belong to the same compressed path set.
    for (unsigned i = 0; i < n; ++i) {
        const auto & K = O[i];
        if (K.empty()) {
            #ifndef NDEBUG
            assert (partitionOrdering.NumOfKernelSets == i);
            while (++i < n) {
                assert (O[i].empty());
            }
            #endif
            break;
        }
        for (const auto k : K) {
            kernelSet[k] = i;
        }
    }

    const auto kernelSets = partitionOrdering.NumOfKernelSets;

    auto kernelSetIdOf = [&](const unsigned kernelId) -> unsigned {
        const auto f = std::lower_bound(kernels.begin(), kernels.end(), kernelId);
        assert (f != kernels.end() && *f == kernelId);
        const unsigned k = std::distance(kernels.begin(), f);
        assert (k < kernelSet.size());
        return kernelSet[k];
    };

    // Identify which streamsets are not fully consumed within a compressed path node.
    std::vector<Vertex> streamSetProducer(numOfStreamSets);
    BitVector hasNonLocalConsumer(numOfStreamSets, false);

    for (unsigned currentPartition = 0; currentPartition < activePartitions; ++currentPartition) {
        for (const auto e : make_iterator_range(in_edges(currentPartition, D))) {
            const PartitionDataflowEdge & E = D[e];
            assert (Relationships[E.KernelId].Type == RelationshipNode::IsKernel);
            const auto consumer = kernelSetIdOf(E.KernelId);
            const auto v = source(e, D);
            assert (v >= activePartitions);
            const auto streamSet = v - activePartitions;
            assert (streamSet < numOfStreamSets);
            if (streamSetProducer[streamSet] != consumer) {
                hasNonLocalConsumer.set(streamSet);
            }
        }
        for (const auto e : make_iterator_range(out_edges(currentPartition, D))) {
            const PartitionDataflowEdge & E = D[e];
            assert (Relationships[E.KernelId].Type == RelationshipNode::IsKernel);
            const auto producer = kernelSetIdOf(E.KernelId);
            const auto v = target(e, D);
            assert (v >= activePartitions);
            const auto streamSet = v - activePartitions;
            assert (streamSet < numOfStreamSets);
            streamSetProducer[streamSet] = producer;
        }
    }

    // And which path nodes contain a streamset that is purely used within it
    BitVector hasLocalStreamSets(kernelSets, false);
    for (unsigned currentPartition = 0; currentPartition < activePartitions; ++currentPartition) {
        for (const auto e : make_iterator_range(out_edges(currentPartition, D))) {
            const PartitionDataflowEdge & C = D[e];
            assert (Relationships[C.KernelId].Type == RelationshipNode::IsKernel);
            const auto producer = kernelSetIdOf(C.KernelId);
            assert (producer < kernelSets);
            const auto v = target(e, D);
            assert (v >= activePartitions);
            const auto streamSet = v - activePartitions;
            assert (streamSet < numOfStreamSets);
            if (!hasNonLocalConsumer.test(streamSet)) {
                hasLocalStreamSets.set(producer);
            }
        }
    }

    const auto externalStreamSetNodes = hasNonLocalConsumer.count();
    const auto streamSetNodes = externalStreamSetNodes + hasLocalStreamSets.count();
    const auto m = kernelSets + streamSetNodes;
    SchedulingGraph S(m);

    for (unsigned i = 0; i < kernelSets; ++i) {
        SchedulingNode & N = S[i];
        N.Type = SchedulingNode::IsKernel;
    }

    BEGIN_SCOPED_REGION

    auto internalStreamSet = (kernelSets + externalStreamSetNodes);

    BitVector includeStreamSet(numOfStreamSets);

    for (unsigned currentKernelSet = 0; currentKernelSet < kernelSets; ++currentKernelSet) {
        // Each path compressed kernel node with an internal streamset will be
        // one-to-one associated with a stream set node. To determine the weight
        // of this amalgamated streamset, we perform the partition logic on it.
        if (hasLocalStreamSets[currentKernelSet]) {

            const auto & path = O[currentKernelSet];
            const auto numOfKernels = path.size();

            assert (numOfKernels > 1);

            std::vector<Vertex> pathSet;
            pathSet.reserve(numOfKernels);
            for (const auto k : path) {
                pathSet.push_back(kernels[k]);
            }

            assert (pathSet.size() == numOfKernels);

            auto localIdOf = [&](const unsigned kernelId) -> unsigned {
                const auto f = std::find(pathSet.begin(), pathSet.end(), kernelId);
                assert (f != pathSet.end());
                return std::distance(pathSet.begin(), f);
            };


            includeStreamSet.reset();

            for (unsigned j = 0; j < numOfStreamSets; ++j) {
                if (hasNonLocalConsumer.test(j)) {
                    continue;
                }
                if (streamSetProducer[j] == currentKernelSet) {
                    includeStreamSet.set(j);
                }
            }

            const auto numOfStreamSets = includeStreamSet.count();

            assert (numOfStreamSets > 0);

            const auto n = numOfKernels + numOfStreamSets;

            SchedulingGraph H(n);

            for (unsigned j = 0; j < numOfKernels; ++j) {
                SchedulingNode & N = H[j];
                N.Type = SchedulingNode::IsKernel;
            }

            auto streamSet = includeStreamSet.find_first();

            for (auto j = numOfKernels; j < n; ++j) {

                assert (streamSet != -1);

                const auto k = firstStreamSet + streamSet;

                const auto output = in_edge(k, D);
                const PartitionDataflowEdge & E = D[output];
                const auto producer = localIdOf(E.KernelId);
                assert (H[producer].Type == SchedulingNode::IsKernel);

                const auto partition = source(output, D);

                SchedulingNode & node = H[j];
                node.Type = SchedulingNode::IsStreamSet;
                node.Size = P[partition].ExpectedRepetitions * E.Expected * D[k];

                add_edge(producer, j, H);

                for (const auto input : make_iterator_range(out_edges(k, D))) {
                    const PartitionDataflowEdge & E = D[input];
                    const auto consumer = localIdOf(E.KernelId);
                    assert (H[consumer].Type == SchedulingNode::IsKernel);
                    add_edge(j, consumer, H);
                }

                streamSet = includeStreamSet.find_next(streamSet);
            }

            assert (streamSet == -1);

            MemoryAnalysis MA(H, numOfKernels, numOfStreamSets);

            std::vector<unsigned> L(numOfKernels);
            for (unsigned i = 0; i < numOfKernels; ++i) {
                L[i] = i;
            }

            const auto maxWeight = MA.analyze(L);

            SchedulingNode & N = S[internalStreamSet];
            N.Type = SchedulingNode::IsStreamSet;
            N.Size = Rational{maxWeight};

            add_edge(currentKernelSet, internalStreamSet, S);
            add_edge(internalStreamSet, currentKernelSet, S);

            ++internalStreamSet;
        }
    }

    assert (internalStreamSet == m);

    END_SCOPED_REGION

    BEGIN_SCOPED_REGION

    std::vector<Vertex> mapping(numOfStreamSets, 0);

    auto i = kernelSets;
    for (const auto k : hasNonLocalConsumer.set_bits()) {
        mapping[k] = i++;
    }

    for (unsigned currentPartition = 0; currentPartition < activePartitions; ++currentPartition) {

        const PartitionData & Pi = P[currentPartition];

        for (const auto e : make_iterator_range(in_edges(currentPartition, D))) {
            const PartitionDataflowEdge & C = D[e];
            assert (Relationships[C.KernelId].Type == RelationshipNode::IsKernel);
            const auto consumer = kernelSetIdOf(C.KernelId);
            const auto v = source(e, D);
            assert (v >= activePartitions);
            const auto streamSet = v - activePartitions;
            assert (streamSet < numOfStreamSets);
            if (hasNonLocalConsumer.test(streamSet)) {
                const auto k = mapping[streamSet];
                assert (k >= kernelSets);
                add_edge(k, consumer, S);
            }
        }

        for (const auto e : make_iterator_range(out_edges(currentPartition, D))) {
            const PartitionDataflowEdge & De = D[e];
            assert (Relationships[De.KernelId].Type == RelationshipNode::IsKernel);
            const auto producer = kernelSetIdOf(De.KernelId);
            const auto v = target(e, D);
            assert (v >= activePartitions);
            const auto streamSet = v - activePartitions;
            assert (streamSet < numOfStreamSets);
            if (hasNonLocalConsumer.test(streamSet)) {
                const auto k = mapping[streamSet];
                assert (k >= kernelSets);
                add_edge(producer, k, S);
                SchedulingNode & node = S[k];
                node.Type = SchedulingNode::IsStreamSet;
                node.Size = Pi.ExpectedRepetitions * De.Expected * D[v]; // bytes per segment
                assert (node.Size.denominator() == 1);
            }
        }
    }

    END_SCOPED_REGION

#endif

    const auto m = numOfFrontierKernels + numOfStreamSets;
    SchedulingGraph S(m);

    for (unsigned i = 0; i < numOfFrontierKernels; ++i) {
        SchedulingNode & N = S[i];
        N.Type = SchedulingNode::IsKernel;
    }

    auto kernelSetIdOf = [&](const unsigned kernelId) -> unsigned {
        const auto f = std::lower_bound(kernels.begin(), kernels.end(), kernelId);
        assert (f != kernels.end() && *f == kernelId);
        return std::distance(kernels.begin(), f);
    };

    for (unsigned currentPartition = 0; currentPartition < activePartitions; ++currentPartition) {

        const PartitionData & Pi = P[currentPartition + 1U];

        for (const auto e : make_iterator_range(in_edges(currentPartition, D))) {
            const PartitionDataflowEdge & C = D[e];
            assert (Relationships[C.KernelId].Type == RelationshipNode::IsKernel);
            const auto consumer = kernelSetIdOf(C.KernelId);
            const auto v = source(e, D);
            assert (v >= activePartitions);
            const auto streamSet = v - activePartitions;
            assert (streamSet < numOfStreamSets);
            const auto k = numOfFrontierKernels + streamSet;
            add_edge(k, consumer, S);
        }

        for (const auto e : make_iterator_range(out_edges(currentPartition, D))) {
            const PartitionDataflowEdge & De = D[e];
            assert (Relationships[De.KernelId].Type == RelationshipNode::IsKernel);
            const auto producer = kernelSetIdOf(De.KernelId);
            const auto v = target(e, D);
            assert (v >= activePartitions);
            const auto streamSet = v - activePartitions;
            assert (streamSet < numOfStreamSets);
            const auto k = numOfFrontierKernels + streamSet;
            add_edge(producer, k, S);
            SchedulingNode & node = S[k];
            node.Type = SchedulingNode::IsStreamSet;
            node.Size = Pi.ExpectedRepetitions * De.Expected * D[v]; // bytes per segment
        }
    }

    ProgramSchedulingAnalysis SA(S, O, numOfFrontierKernels, maxPathLength);

    // ProgramSchedulingAnalysis SA(S, O, kernelSets, streamSetNodes, pathLength);

    OrderingDAWG schedule(1);

    SA.runGA(schedule);

//    errs () << "init_time: " << init_time << "\n"
//               "fitness_time: " << fitness_time << "\n"
//               "repair_time: " << repair_time << "\n"
//               "evolutionary_time: " << evolutionary_time << "\n";

//    printDAWG(schedule, errs(), "S");


    std::vector<unsigned> program;

    program.reserve(kernels.size());

    Vertex position = 0;


    while (out_degree(position, schedule) > 0) {
        const auto e = first_out_edge(position, schedule);

        const auto s = schedule[e];
        const auto k = kernels[s];
        program.push_back(k);

//        const auto local = schedule[e];
//        assert (local < num_vertices(O));
//        for (const auto k : O[local]) {
//            program.push_back(kernels[k]);
//        }
        const auto next = target(e, schedule);
        assert (position != next);
        position = next;
    }

    assert (program.size() >= (kernels.size() - 2));

    return program;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addSchedulingConstraints
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineAnalysis::addSchedulingConstraints(const PartitionGraph & P,
                                                const std::vector<unsigned> & program) {

    // Since we compressed the graph, nodes within O represent 0 to many kernels that
    // have cross partition I/O. These kernels could be from multiple partitions so
    // to simplify the logic, we initially create a partial program list then fill it
    // in by selecting a partition schedule that matches the selected program.

    Candidate subgraph;

    std::vector<unsigned> path;
    // underflow sentinal node
    path.push_back(-1U);

    auto u = PipelineInput;

//    auto printProgram =[](const std::vector<unsigned> & L, unsigned start, unsigned end) {
//        bool addComma = false;
//        for (auto i = start; i < end; ++i) {
//            if (addComma) errs() << ',';
//            errs() << L[i];
//            addComma = true;
//        }
//        errs() << "\n";
//    };

//    errs() << "PROGRAM: ";
//    printProgram(program, 0, program.size());


    const auto end = program.end();

    for (auto i = program.begin(); i != end; ) {

        path.resize(1);

        unsigned currentPartitionId = 0;

        while (i != end) {

            const auto node = *i;

            const auto f = PartitionIds.find(node);
            assert (f != PartitionIds.end());
            const auto pid = f->second - 1;

            if (path.size() == 1) {
                currentPartitionId = pid;
            } else if (currentPartitionId != pid) {
                break;
            }
            path.push_back(node);

            ++i;
        }

        // overflow sentinal node
        path.push_back(-1U);

        assert (path.size() > 2);

        const PartitionData & partition = P[currentPartitionId + 1U];

        const auto & G = partition.Orderings;
        const auto & K = partition.Kernels;


//        printDAWG(G, K, errs(), "P" + std::to_string(currentPartitionId));

//        errs() << "PATH: ";
//        printProgram(path, 1, path.size() - 1);


        const auto numOfKernels = K.size();
        assert (path.size() <= numOfKernels + 2);

        assert (subgraph.empty());

        subgraph.reserve(numOfKernels);

        unsigned offset = 1;

        std::function<bool(unsigned)> select_path = [&](const unsigned u) {

            if (LLVM_UNLIKELY(out_degree(u, G) == 0)) {
                // when we find an ordering of the kernels within this
                // partition that matches the desired global ordering,
                // exit the function.
                return (offset == (path.size() - 1));
            } else {
                for (const auto e : make_iterator_range(out_edges(u, G))) {
                    const auto t = G[e];
                    assert (t < K.size());
                    const auto k = K[t];
                    assert (offset < path.size());
                    if (path[offset] == k) {
                        ++offset;
                    }
                    subgraph.push_back(k);
                    const auto v = target(e, G);
                    if (select_path(v)) {
                        return true;
                    }
                    assert (subgraph.back() == k);

                    subgraph.pop_back();
                    if (path[offset - 1] == k) {
                        --offset;
                    }
                }
                return false;
            }

        };

        const auto found = select_path(0);
        assert (found);

        for (const auto v : subgraph) {
            if (PipelineInput != v && u != PipelineOutput) {
                add_edge(u, v, RelationshipType{ReasonType::OrderingConstraint}, Relationships);
            }
            u = v;
        }

        subgraph.clear();

    }

    if (u != PipelineOutput) {
        add_edge(u, PipelineOutput, RelationshipType{ReasonType::OrderingConstraint}, Relationships);
    }
}

#endif

}

#endif // SCHEDULING_ANALYSIS_HPP