/*
 *  Copyright (c) 2014 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#ifndef SHOW_H
#define SHOW_H

namespace llvm { class raw_ostream; }

namespace pablo {

class PabloKernel;
class PabloBlock;
class Statement;
class PabloAST;

class PabloPrinter {
public:
    static void print(const PabloKernel * kernel, llvm::raw_ostream & out);
    static void print(const PabloAST * expr, llvm::raw_ostream & out);
    static void print(const PabloBlock * block, llvm::raw_ostream & strm, const bool expandNested = false, const unsigned indent = 0);
    static void print(const Statement * stmt, llvm::raw_ostream & out, const bool expandNested = false, const unsigned indent = 0);
};

}

#endif // SHOW_H
