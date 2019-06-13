/*
 *  Copyright (c) 2018 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#include "file_select.h"
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/raw_ostream.h>
#include <boost/filesystem.hpp>
#include <toolchain/toolchain.h>
#include <re/re_re.h>
#include <re/re_alt.h>
#include <re/re_seq.h>
#include <re/re_rep.h>
#include <re/re_start.h>
#include <re/re_end.h>
#include <re/re_cc.h>
#include <re/re_utility.h>
#include <re/re_assertion.h>
#include <re/re_toolchain.h>
#include <re/parsers/parser.h>
#include <re/parsers/GLOB_parser.h>
#include <re/printer_re.h>
#include <grep/grep_engine.h>
#include <grep/searchable_buffer.h>
#include <toolchain/cpudriver.h>
#include <fstream>
#include <string>
#include <re/printer_re.h>

using namespace llvm;

namespace argv {

static cl::OptionCategory Input_Options("File Selection Options", "These options control the input sources.");

bool NoMessagesFlag;
static cl::opt<bool, true> NoMessagesOption("s", cl::location(NoMessagesFlag), cl::desc("Suppress messages for file errors."), cl::cat(Input_Options), cl::Grouping);
static cl::alias NoMessagesAlias("no-messages", cl::desc("Alias for -s"), cl::aliasopt(NoMessagesOption));

bool RecursiveFlag;
static cl::opt<bool, true> RecursiveOption("r", cl::location(RecursiveFlag), cl::desc("Recursively process files within directories, (but follow only top-level symlinks unless -R)."), cl::cat(Input_Options), cl::Grouping);
static cl::alias RecursiveAlias("recursive", cl::desc("Alias for -r"), cl::aliasopt(RecursiveOption));

bool DereferenceRecursiveFlag;
static cl::opt<bool, true> DereferenceRecursiveOption("R", cl::location(DereferenceRecursiveFlag), cl::desc("Recursively process files within directories, following symlinks at all levels."), cl::cat(Input_Options), cl::Grouping);
static cl::alias DereferenceRecursiveAlias("dereference-recursive", cl::desc("Alias for -R"), cl::aliasopt(DereferenceRecursiveOption));


bool MmapFlag;
static cl::opt<bool, true> MmapOption("mmap", cl::location(MmapFlag),  cl::init(1), cl::desc("Use mmap for file input (default)."), cl::cat(Input_Options));

static cl::list<std::string> ExcludeFiles("exclude", cl::ZeroOrMore,
                                          cl::desc("Exclude files matching the given filename GLOB pattern."), cl::cat(Input_Options));

static cl::opt<std::string> ExcludeFromFlag("exclude-from", cl::desc("Exclude files matching filename GLOB patterns from the given file."), cl::cat(Input_Options));

static cl::list<std::string> ExcludeDirectories("exclude-dir", cl::desc("Exclude directories matching the given pattern."), cl::ZeroOrMore, cl::cat(Input_Options));
    
static cl::opt<std::string> ExcludePerDirectory("exclude-per-directory",
                                                cl::desc(".gitignore (default) or other file specifying files to exclude."),
                                                cl::init(".gitignore"), cl::cat(Input_Options));
    
static cl::list<std::string> IncludeDirectories("include-dir", cl::desc("Include directories matching the given pattern."), cl::ZeroOrMore, cl::cat(Input_Options));

static cl::list<std::string> IncludeFiles("include", cl::ZeroOrMore, cl::desc("Include only files matching the given filename GLOB pattern(s)."), cl::cat(Input_Options));

DevDirAction DevicesFlag;
static cl::opt<DevDirAction, true> DevicesOption("D", cl::desc("Processing mode for devices:"),
                                                 cl::values(clEnumValN(Read, "read", "Treat devices as files to be searched."),
                                                            clEnumValN(Skip, "skip", "Silently skip devices.")
                                                            CL_ENUM_VAL_SENTINEL), cl::cat(Input_Options), cl::location(DevicesFlag), cl::init(Read));
static cl::alias DevicesAlias("devices", cl::desc("Alias for -D"), cl::aliasopt(DevicesOption));

DevDirAction DirectoriesFlag;
static cl::opt<DevDirAction, true> DirectoriesOption("d", cl::desc("Processing mode for directories:"),
                                                     cl::values(clEnumValN(Read, "read", "Print an error message for any listed directories."),
                                                                clEnumValN(Skip, "skip", "Silently skip directories."),
                                                                clEnumValN(Recurse, "recurse", "Recursive process directories, equivalent to -r.")
                                                                CL_ENUM_VAL_SENTINEL), cl::cat(Input_Options), cl::location(DirectoriesFlag), cl::init(Read));
static cl::alias DirectoriesAlias("directories", cl::desc("Alias for -d"), cl::aliasopt(DirectoriesOption));
    
static cl::opt<bool> TraceFileSelect("TraceFileSelect", cl::desc("Trace file selection"), cl::cat(Input_Options));

static cl::opt<unsigned> GitREcoalescing("git-RE-coalescing", cl::desc("gitignore RE coalescing factor"), cl::init(10), cl::cat(Input_Options));


// Command line arguments to specify file and directory includes/excludes
// use GLOB syntax, matching any full pathname suffix after a "/", or
// the full filename of any recursively selected file or directory.
re::RE * anchorToFullFileName(re::RE * glob) {
    return re::makeSeq({re::makeAlt({re::makeStart(), re::makeCC('/')}), glob, re::makeEnd()});
}

bool UseStdIn;

std::vector<std::pair<re::PatternKind, re::RE *>> getIncludeExcludePatterns() {
    std::vector<std::pair<re::PatternKind, re::RE *>> signedPatterns;
    if (IncludeDirectories.empty() && IncludeFiles.empty()) {
        // No explicit inclusion, start by including everything.
        signedPatterns.push_back(std::make_pair(re::PatternKind::Include, re::makeEnd()));
    } else if (IncludeDirectories.empty()) {
        // Include any directory, using the pattern "/$"
        signedPatterns.push_back(std::make_pair(re::PatternKind::Include, re::makeSeq({re::makeCC('/'), re::makeEnd()})));
    } else if (IncludeFiles.empty()) {
        // Include any file, using the pattern "[^/]$"
        signedPatterns.push_back(std::make_pair(re::PatternKind::Include,
                                                re::makeSeq({re::makeComplement(re::makeCC('/')), re::makeEnd()})));
    }
    for (auto & d : IncludeDirectories) {
        std::string path = d + "/";  // Force directory matching only by appending a "/".
        re::RE * includeDirRE = re::RE_Parser::parse(path, re::DEFAULT_MODE, re::RE_Syntax::FileGLOB);
        signedPatterns.push_back(std::make_pair(re::PatternKind::Include, anchorToFullFileName(includeDirRE)));
    }
    for (auto & path : IncludeFiles) {
        re::RE * includeRE = re::RE_Parser::parse(path, re::DEFAULT_MODE, re::RE_Syntax::FileGLOB);
        signedPatterns.push_back(std::make_pair(re::PatternKind::Include, anchorToFullFileName(includeRE)));
    }
    for (auto & d : ExcludeDirectories) {
        std::string path = d + "/"; // Force directory matching only by appending a "/".
        re::RE * includeDirRE = re::RE_Parser::parse(path, re::DEFAULT_MODE, re::RE_Syntax::FileGLOB);
        signedPatterns.push_back(std::make_pair(re::PatternKind::Exclude, anchorToFullFileName(includeDirRE)));
    }
    for (auto & path : ExcludeFiles) {
        re::RE * excludeRE = re::RE_Parser::parse(path, re::DEFAULT_MODE, re::RE_Syntax::FileGLOB);
        signedPatterns.push_back(std::make_pair(re::PatternKind::Exclude, anchorToFullFileName(excludeRE)));
    }
    if (ExcludeFromFlag != "") {
        std::ifstream globFile(ExcludeFromFlag.c_str());
        std::string r;
        if (globFile.is_open()) {
            while (std::getline(globFile, r)) {
                re::RE * glob = re::RE_Parser::parse(r, re::DEFAULT_MODE, re::RE_Syntax::FileGLOB);
                signedPatterns.push_back(std::make_pair(re::PatternKind::Exclude, anchorToFullFileName(glob)));
            }
            globFile.close();
        }
    }
    return signedPatterns;
}

namespace fs = boost::filesystem;
//
void selectPath(std::vector<fs::path> & collectedPaths, fs::path p) {
    collectedPaths.push_back(p);
    if (TraceFileSelect) {
        llvm::errs() << "Selecting: " << p.string() << "\n";
    }
}

//
//  Directory List: a set of directory paths that have been
//  examined to identify candidate files for searching, together
//  with a count of the number of candidate files in each directory.
//
//  FileName Buffer: an ordered sequence of NUL terminated filenames
//  for each candidate produced in the directory traversal.
//  The first mFullPathEntries entries are CWD paths.  Subsequent entries
//  are base file names relative to a directory.   The set
//  of all entries for a given directory are consecutive in the
//  buffer, and the sets are ordered consecutively by directory
//  index in the Directory List.
//
//  CollectedPaths: a vector of file paths to which the
//  selected files are added.

class FileSelectAccumulator : public grep::MatchAccumulator {
public:
    FileSelectAccumulator(std::vector<fs::path> & collectedPaths) :
        mCollectedPaths(collectedPaths),
        mFullPathEntries(0)
    {}
    void setFullPathEntries(unsigned entries) {mFullPathEntries = entries; mDirectoryIndex = 0;}
    void reset();
    void addDirectory(fs::path dirPath, unsigned cumulativeEntryCount);
    void accumulate_match(const size_t lineNum, char * line_start, char * line_end) override;
protected:
    std::vector<fs::path> & mCollectedPaths;
    unsigned mFullPathEntries;
    unsigned mDirectoryIndex;
    std::vector<fs::path> mDirectoryList;
    std::vector<unsigned> mCumulativeEntryCount;
};

void FileSelectAccumulator::reset() {
    mCollectedPaths.clear();
    mFullPathEntries = 0;
    mDirectoryIndex = 0;
    mDirectoryList.clear();
    mCumulativeEntryCount.clear();
}

void FileSelectAccumulator::addDirectory(fs::path dirPath, unsigned cumulativeEntryCount) {
    mDirectoryList.push_back(dirPath);
    mCumulativeEntryCount.push_back(cumulativeEntryCount);
}

void FileSelectAccumulator::accumulate_match(const size_t fileIdx, char * name_start, char * name_end) {
    assert(name_end > name_start);
    assert(name_end - name_start <= 4096);
    //llvm::errs() << "fileIdx = " << fileIdx << " (" << ((intptr_t) name_start) << ", " << ((intptr_t) name_end) << ")\n";
    fs::path p(std::string(name_start, name_end - name_start));
    if (fileIdx < mFullPathEntries) {
        selectPath(mCollectedPaths, p);
   } else {
        assert(mDirectoryIndex < mDirectoryList.size());
        while (fileIdx >= mCumulativeEntryCount[mDirectoryIndex]) {
            mDirectoryIndex++;
        }
        selectPath(mCollectedPaths, mDirectoryList[mDirectoryIndex]/std::string(name_start, name_end - name_start));
    }
}

std::vector<std::pair<re::PatternKind, re::RE *>> coalesceREs(std::vector<std::pair<re::PatternKind, re::RE *>> sourceREs, unsigned groupingFactor) {
    std::vector<std::pair<re::PatternKind, re::RE *>> coalesced;
    unsigned i = 0;
    while (i < sourceREs.size()) {
        std::vector<re::RE *> current = {sourceREs[i].second};
        re::PatternKind currentKind = sourceREs[i].first;
        unsigned j = 1;
        while ((j < groupingFactor) && (i + j < sourceREs.size()) && (sourceREs[i + j].first == currentKind)) {
            current.push_back(sourceREs[i + j].second);
            j++;
        }
        coalesced.push_back(std::make_pair(currentKind, re::makeAlt(current.begin(), current.end())));
        i = i + j;
    }
    return coalesced;
}

void recursiveFileSelect(CPUDriver & driver,
                         fs::path dirpath,
                         grep::InternalMultiSearchEngine & inheritedEngine,
                         std::vector<std::pair<re::PatternKind, re::RE *>> inheritedREs,
                         std::vector<fs::path> & collectedPaths) {

    // First update the search REs with any local .gitignore or other exclude file.
    bool hasLocalIgnoreFile = !ExcludePerDirectory.empty() && fs::exists(dirpath/ExcludePerDirectory);
    //llvm::errs() << "recursiveFileSelect: " << dirpath.string() << " hasLocalIgnore=" << hasLocalIgnoreFile << "\n";
    std::vector<std::pair<re::PatternKind, re::RE *>> updatedREs;
    if (hasLocalIgnoreFile) {
        updatedREs = re::parseGitIgnoreFile(dirpath, ExcludePerDirectory, inheritedREs);
        updatedREs = coalesceREs(updatedREs, GitREcoalescing);
    }
    // Gather files and subdirectories.
    grep::SearchableBuffer subdirCandidates;
    grep::SearchableBuffer fileCandidates;
    
    boost::system::error_code errc;
    fs::directory_iterator di(dirpath, errc);
    if (errc) {
        // If we cannot enter the directory, keep it in the list of files,
        // for possible error reporting.
        if (!NoMessagesFlag) {
            collectedPaths.push_back(dirpath);
        }
        return;
    }
    const auto di_end = fs::directory_iterator();
    while (di != di_end) {
        const auto & e = di->path();
        boost::system::error_code errc;
        const auto s = fs::status(e, errc);
        if (errc) {
            // If there was an error, we leave the file in the fileCandidates
            // list for later error processing.
            if (!NoMessagesFlag) {
                fileCandidates.append(e.string());
            }
        } else if (fs::is_directory(s)) {
            if (fs::is_symlink(e) && !DereferenceRecursiveFlag) {
                di.increment(errc);
                continue;
            }
            subdirCandidates.append(e.string() + "/");
        } else if (fs::is_regular_file(s) || DevicesFlag == Read) {
            fileCandidates.append(e.string());
        }
        boost::system::error_code errc2;
        di.increment(errc2);
        if (errc2) break;
    }
    // For each directory, update counts for candidates generated at this level.
    //
    FileSelectAccumulator fileAccum(collectedPaths);
    std::vector<fs::path> selectedDirectories;
    FileSelectAccumulator directoryAccum(selectedDirectories);
    auto subdirCount = subdirCandidates.getCandidateCount();
    auto fileCount = fileCandidates.getCandidateCount();
    fileAccum.setFullPathEntries(fileCount);
    directoryAccum.setFullPathEntries(subdirCount);

    if (hasLocalIgnoreFile) {
        // Compile the updated file and directory selection REs, and apply them to
        // choose files and directories for processing.
        // Apply the file selection RE to choose files for processing, adding
        // them to the global list of selected files.
        grep::InternalMultiSearchEngine pathSelectEngine(driver);
        pathSelectEngine.setRecordBreak(grep::GrepRecordBreakKind::Null);
        pathSelectEngine.grepCodeGen(updatedREs);
        if (fileCount > 0) {
            pathSelectEngine.doGrep(fileCandidates.data(), fileCandidates.size(), fileAccum);
        }
        // Now select subdirectories for recursive processing.
        if (subdirCount > 0) {
            pathSelectEngine.doGrep(subdirCandidates.data(), subdirCandidates.size(), directoryAccum);
            // Work through the subdirectories, invoking the recursive processing for each.
            for (const auto & subdir : selectedDirectories) {
                recursiveFileSelect(driver, subdir, pathSelectEngine, updatedREs, collectedPaths);
            }
        }
    } else {
        if (fileCount > 0) {
            inheritedEngine.doGrep(fileCandidates.data(), fileCandidates.size(), fileAccum);
        }
        if (subdirCount > 0) {
            inheritedEngine.doGrep(subdirCandidates.data(), subdirCandidates.size(), directoryAccum);
            for (const auto & subdir : selectedDirectories) {
                recursiveFileSelect(driver, subdir, inheritedEngine, inheritedREs, collectedPaths);
            }
        }
    }
}

std::vector<fs::path> getFullFileList(CPUDriver & driver, cl::list<std::string> & inputFiles) {
    // The vector to accumulate the full list of collected files to be searched.
    std::vector<fs::path> collectedPaths;

    // In this pass through command line arguments and the file hierarchy,
    // we are just gathering file and subdirectory entries, so we silently
    // ignore errors.  We use the boost::filesystem operations that set
    // error codes rather than raise exceptions.

    // In non-recursive greps with no include/exclude processing, we simply assemble the
    // paths.
    if ((DirectoriesFlag != Recurse) && (ExcludeFiles.empty()) && (IncludeFiles.empty()) && (ExcludeFromFlag.empty())) {
        for (const std::string & f : inputFiles) {
            if (f == "-") {  // stdin, will always be searched.
                argv::UseStdIn = true;
                continue;
            }
            fs::path p(f);
            boost::system::error_code errc;
            fs::file_status s = fs::status(p, errc);
            if (errc) {
                // If there was an error, we leave the file in the fileCandidates
                // list for later error processing.
                if (!NoMessagesFlag) {
                    selectPath(collectedPaths, p);
                }
            } else if (fs::is_directory(s)) {
                if (DirectoriesFlag == Read) {
                    selectPath(collectedPaths, p);
                }
            } else if (fs::is_regular_file(s)) {
                selectPath(collectedPaths, p);
            } else {
                // Devices and unknown file types
                if (DevicesFlag == Read) {
                    selectPath(collectedPaths, p);
                }
            }
        }
        return collectedPaths;
    }

    // Otherwise we may need to filter paths according to some include/exclude rules.
    grep::SearchableBuffer dirCandidates;
    grep::SearchableBuffer fileCandidates;

    // First level of processing:  command line files and directories.
    for (const std::string & f : inputFiles) {
        if (f == "-") {  // stdin, will always be searched.
            argv::UseStdIn = true;
            continue;
        }
        fs::path p(f);
        boost::system::error_code errc;
        fs::file_status s = fs::status(p, errc);
        if (errc) {
            // If there was an error, we leave the file in the fileCandidates
            // list for later error processing.
            if (!NoMessagesFlag) fileCandidates.append(p.string());
        } else if (fs::is_directory(s)) {
            if (DirectoriesFlag == Recurse) {
                dirCandidates.append(p.string() + "/");
            } else if (DirectoriesFlag == Read) {
                fileCandidates.append(p.string());
            }
        } else if (fs::is_regular_file(s)) {
            fileCandidates.append(p.string());
        } else {
            // Devices and unknown file types
            if (DevicesFlag == Read) {
                fileCandidates.append(p.string());
            }
        }
    }

    auto commandLineDirCandidates = dirCandidates.getCandidateCount();
    auto commandLineFileCandidates = fileCandidates.getCandidateCount();
    FileSelectAccumulator fileAccum(collectedPaths);
    fileAccum.setFullPathEntries(commandLineFileCandidates);
    //
    // Apply the file selection REs to choose files for processing, adding
    // them to the global list of selected files.

    auto patterns = getIncludeExcludePatterns();

    grep::InternalMultiSearchEngine pathSelectEngine(driver);
    pathSelectEngine.setRecordBreak(grep::GrepRecordBreakKind::Null);
    pathSelectEngine.grepCodeGen(patterns);
    if (commandLineFileCandidates > 0) {
        pathSelectEngine.doGrep(fileCandidates.data(), fileCandidates.size(), fileAccum);
    }
    //
    //
    if (commandLineDirCandidates > 0) {
        // Recursive processing of directories has been requested and we have
        // candidate directories from the command line.

        // selectedDirectories will accumulate hold the results of directory
        // include/exclude filtering at each level of processing.
        std::vector<fs::path> selectedDirectories;
        FileSelectAccumulator directoryAccum(selectedDirectories);

        // The initial grep search determines which of the command line directories to process.
        // Each of these candidates is a full path return from command line argument processing.
        directoryAccum.setFullPathEntries(dirCandidates.getCandidateCount());
        pathSelectEngine.doGrep(dirCandidates.data(), dirCandidates.size(), directoryAccum);

        // Select files from subdirectories using the recursive process.
        for (const auto & dirpath : selectedDirectories) {
            recursiveFileSelect(driver, dirpath, pathSelectEngine, patterns, collectedPaths);
        }
    }
    return collectedPaths;
}

}
