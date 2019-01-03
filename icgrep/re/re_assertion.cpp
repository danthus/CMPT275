/*
 *  Copyright (c) 2018 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#include "re_assertion.h"
#include "re_cc.h"
#include "re_start.h"
#include "re_end.h"
#include "re_any.h"
#include "re_seq.h"
#include "re_alt.h"
#include "re_diff.h"
#include "re_nullable.h"
#include <re/re_toolchain.h>

using namespace llvm;

namespace re {
    
RE * makeSOT () {
    //return makeNegativeLookBehindAssertion(makeByte(0x00,0xFF));
    return makeStart();
}

RE * makeEOT () {
    //return makeNegativeLookAheadAssertion(makeByte(0x00,0xFF));
    return makeEnd();
}

RE * expandBoundaryAssertion (RE * re) {
    if (Assertion * a = dyn_cast<Assertion>(re)) {
        if (a->getKind() == Assertion::Kind::Boundary) {
            RE * asserted = a->getAsserted();
            RE * behindP = makeAssertion(asserted, Assertion::Kind::Lookbehind, Assertion::Sense::Positive);
            RE * behindN = makeAssertion(asserted, Assertion::Kind::Lookbehind, Assertion::Sense::Negative);
            RE * aheadP = makeAssertion(asserted, Assertion::Kind::Lookahead, Assertion::Sense::Positive);
            RE * aheadN = makeAssertion(asserted, Assertion::Kind::Lookahead, Assertion::Sense::Negative);
            if (a->getSense() == Assertion::Sense::Positive) {
                return makeAlt({makeSeq({behindP, aheadN}), makeSeq({behindN, aheadP})});
            } else {
                return makeAlt({makeSeq({behindP, aheadP}), makeSeq({behindN, aheadN})});
            }
        }
    }
    return re;
}
    
    
struct FinalLookaheadPromotion : public RE_Transformer {
    FinalLookaheadPromotion() : RE_Transformer("FinalLookaheadPromotion") {}
    RE * transformSeq(Seq * s) override {
        if (s->empty()) return s;
        RE * s_last = s->back();
        RE * t = transform(s_last);
        if (s_last == t) return s;
        std::vector<RE *> elems;
        for (unsigned i = 0; i < s->size() - 1; i++) {
            elems.push_back((*s)[i]);
        }
        elems.push_back(t);
        return makeSeq(elems.begin(), elems.end());
    }
    RE * transformDiff(Diff * d) override { return d;}
    RE * transformRep(Rep * r) override { return r;}
    RE * transformAssertion(Assertion * a) override {
        if ((a->getKind() == Assertion::Kind::Lookahead) && (a->getSense() == Assertion::Sense::Positive)) {
            return transform(a->getAsserted());
        }
        return a;
    }
};
    
RE * lookaheadPromotion(RE * r) {
    return FinalLookaheadPromotion().transformRE(r);
}
}
