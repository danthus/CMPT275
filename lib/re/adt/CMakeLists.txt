# Copyright (c) 2019 International Characters.
# This software is licensed to the public under the Open Software License 3.0.

# module: re.adt

parabix_add_module(
NAME
    re.adt
SRC
    printer_re.cpp
    re_assertion.cpp
    re_cc.cpp
    re_diff.cpp
    re_empty_set.cpp
    re_intersect.cpp
    re_range.cpp
    re_re.cpp
    re_rep.cpp
    memoization.cpp
    re_utility.cpp
DEPS
    re.alphabet
    re.toolchain
)
