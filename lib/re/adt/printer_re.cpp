/*
 *  Copyright (c) 2014 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#include <re/adt/printer_re.h>

//Regular Expressions
#include <re/adt/re_re.h>
#include <re/adt/re_alt.h>
#include <re/adt/re_any.h>
#include <re/adt/re_cc.h>
#include <re/adt/re_name.h>
#include <re/adt/re_end.h>
#include <re/adt/re_rep.h>
#include <re/adt/re_seq.h>
#include <re/adt/re_start.h>
#include <re/adt/re_range.h>
#include <re/adt/re_diff.h>
#include <re/adt/re_intersect.h>
#include <re/adt/re_assertion.h>
#include <re/adt/re_group.h>
#include <re/alphabet/alphabet.h>

using namespace re;
using namespace llvm;

const std::string Printer_RE::PrintRE(const RE * re) {
    std::string retVal = "";

    if (re == nullptr) {
        retVal = "<NULL>";
    } else if (const Alt* re_alt = dyn_cast<const Alt>(re)) {
        retVal += "(Alt[";
        bool comma = false;
        for (const RE * re : *re_alt) {
            if (comma) {
                retVal += ',';
            }
            retVal += PrintRE(re);
            comma = true;
        }
        retVal += "])";
    } else if (const CC* re_cc = dyn_cast<const CC>(re)) {
        retVal = "CC \"";
        retVal += re_cc->canonicalName();
        retVal += "\" ";
    } else if (const Name* re_name = dyn_cast<const Name>(re)) {
        retVal = "Name \"";
        if (re_name->hasNamespace()) {
            retVal += re_name->getNamespace();
            retVal += ":";
        }
        retVal += re_name->getName();
        retVal += "\" ";
        if (re_name->getType() == Name::Type::Capture) {
            retVal += "=(" + PrintRE(re_name->getDefinition()) + ")";
        }
    } else if (const Range* rg = dyn_cast<const Range>(re)) {
        retVal = "Range (";
        retVal += PrintRE(rg->getLo());
        retVal += " , ";
        retVal += PrintRE(rg->getHi());
        retVal += ") ";
    } else if (const Assertion * a = dyn_cast<const Assertion>(re)) {
        retVal = (a->getSense() == Assertion::Sense::Positive) ? "" : "Negative";
        switch (a->getKind()) {
            case Assertion::Kind::Lookahead:
                retVal += "Lookahead";
                break;
            case Assertion::Kind::Lookbehind:
                retVal += "Lookbehind";
                break;
            case Assertion::Kind::Boundary:
                retVal += "Boundary";
                break;
        }
        retVal += "Assertion(";
        retVal += PrintRE(a->getAsserted());
        retVal += ") ";
    } else if (const Diff* diff = dyn_cast<const Diff>(re)) {
        retVal = "Diff (";
        retVal += PrintRE(diff->getLH());
        retVal += " , ";
        retVal += PrintRE(diff->getRH());
        retVal += ") ";
    } else if (const Intersect* x = dyn_cast<const Intersect>(re)) {
        retVal = "Intersect (";
        retVal += PrintRE(x->getLH());
        retVal += " , ";
        retVal += PrintRE(x->getRH());
        retVal += ") ";
    } else if (isa<const End>(re)) {
        retVal = "End";
    } else if (const Rep* re_rep = dyn_cast<const Rep>(re)) {
        retVal = "Rep(";
        retVal.append(PrintRE(re_rep->getRE()));
        retVal.append(",");
        retVal.append(std::to_string(re_rep->getLB()));
        retVal.append(",");
        if (re_rep->getUB() == Rep::UNBOUNDED_REP) {
            retVal.append("Unbounded");
        }
        else {
            retVal.append(std::to_string(re_rep->getUB()));
        }
        retVal.append(")");
    } else if (const Seq* re_seq = dyn_cast<const Seq>(re)) {
        retVal = "(Seq[";
        bool comma = false;
        for (const RE * re : *re_seq) {
            if (comma) {
                retVal.append(",");
            }
            retVal.append(PrintRE(re));
            comma = true;
        }
        retVal.append("])");
    } else if (const Group * g = dyn_cast<const Group>(re)) {
        retVal = "Group(";
        if (g->getMode() == Group::Mode::GraphemeMode) {
            retVal.append((g->getSense() == Group::Sense::On) ? "+g:" : "-g:");
        }
        else if (g->getMode() == Group::Mode::CaseInsensitiveMode) {
            retVal.append((g->getSense() == Group::Sense::On) ? "+i:" : "-i:");
        }
        else if (g->getMode() == Group::Mode::CompatibilityMode) {
            retVal.append((g->getSense() == Group::Sense::On) ? "+K:" : "-K:");
        }
        retVal.append(PrintRE(g->getRE()));
        retVal.append(")");
    } else if (isa<const Start>(re)) {
        retVal = "Start";
    } else if (isa<const Any>(re)) {
        retVal = "Any";
    } else {
        retVal = "???";
    }
    return retVal;
}