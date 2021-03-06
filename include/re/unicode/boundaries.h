#ifndef BOUNDARIES_H
#define BOUNDARIES_H

namespace UCD { class EnumeratedPropertyObject;}

namespace re {
    
class RE;
class Name;

class EnumeratedPropertyObject;

bool hasGraphemeClusterBoundary(const RE * re);

bool hasWordBoundary(const RE * re);

RE * resolveGraphemeMode(RE * re, bool inGraphemeMode);

RE * generateGraphemeClusterBoundaryRule(bool extendedGraphemeClusters = true);

RE * EnumeratedPropertyBoundary(UCD::EnumeratedPropertyObject * enumObj);

RE * resolveBoundaryProperties(RE * r);
}

#endif

