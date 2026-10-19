// Copyright (c) 2026 The vycor-cpp Authors
// Original author: Alex Mason
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// TU selection for the megascope bake (vycor/cli/SourceSelection.h).

#include "vycor/cli/SourceSelection.h"

#include "vycor/compat/Path.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <sstream>

using namespace vycor;

namespace {
#ifdef _WIN32
const std::string kAbsolutePrefix = "C:/x/";
#else
const std::string kAbsolutePrefix = "/x/";
#endif

/// A compilation database whose only job is to answer getAllFiles().
class FilesDb : public clang::tooling::CompilationDatabase {
public:
  explicit FilesDb(std::vector<std::string> files)
      : files_(std::move(files)) {}
  std::vector<clang::tooling::CompileCommand>
  getCompileCommands(llvm::StringRef) const override {
    return {};
  }
  std::vector<std::string> getAllFiles() const override { return files_; }

private:
  std::vector<std::string> files_;
};

/// A scratch tree with real files: default-all keeps only C/C++ sources
/// that exist, so the database fixture has to be on disk.
struct Tree {
  llvm::SmallString<128> root;
  Tree() {
    llvm::sys::fs::createUniqueDirectory("vycor-src-sel", root);
    for (const char *rel :
         {"Network/src/a.cpp", "Network/tests/a_test.cpp",
          "ThirdParty/zlib/z.c", "Core/b.cpp", "Core/asm.S"}) {
      llvm::SmallString<128> p(root);
      llvm::sys::path::append(p, rel);
      llvm::sys::fs::create_directories(llvm::sys::path::parent_path(p));
      std::error_code ec;
      llvm::raw_fd_ostream os(p, ec);
      REQUIRE_FALSE(ec);
      os << "// fixture\n";
    }
  }
  ~Tree() {
    std::error_code ec;
    for (llvm::sys::fs::recursive_directory_iterator it(root, ec), end;
         it != end && !ec; it.increment(ec))
      if (!llvm::sys::fs::is_directory(it->path()))
        llvm::sys::fs::remove(it->path());
    // Directories, deepest first.
    for (const char *d : {"Network/src", "Network/tests", "Network",
                          "ThirdParty/zlib", "ThirdParty", "Core"}) {
      llvm::SmallString<128> p(root);
      llvm::sys::path::append(p, d);
      llvm::sys::fs::remove(p);
    }
    llvm::sys::fs::remove(root);
  }
  std::string at(llvm::StringRef rel) const {
    llvm::SmallString<128> p(root);
    llvm::sys::path::append(p, rel);
    return normalizedPath(p);
  }
  /// What a compilation database would list: the sources, a duplicate
  /// (multi-config databases), an assembly file, and a deleted file.
  std::vector<std::string> dbEntries() const {
    return {at("Network/src/a.cpp"), at("Network/tests/a_test.cpp"),
            at("ThirdParty/zlib/z.c"), at("Core/b.cpp"),
            at("Network/src/a.cpp"), at("Core/asm.S"),
            at("Core/deleted.cpp")};
  }
};

std::vector<std::string> select(const Tree &tree, const SourceSelection &sel,
                                const std::string &stdinText = "",
                                SourceSelectionStats *stats = nullptr) {
  FilesDb db(tree.dbEntries());
  std::istringstream in(stdinText);
  auto r = selectSources(db, sel, in, stats);
  REQUIRE(bool(r));
  return *r;
}

std::string writeTempList(llvm::StringRef body) {
  llvm::SmallString<128> p;
  llvm::sys::fs::createUniquePath("vycor-source-list-%%%%%%.txt", p, true);
  std::error_code ec;
  llvm::raw_fd_ostream os(p, ec);
  REQUIRE_FALSE(ec);
  os << body;
  return p.str().str();
}

} // namespace

TEST_CASE("no selection flag means every C/C++ database entry on disk, "
          "deduplicated and sorted",
          "[source-selection]") {
  Tree tree;
  SourceSelectionStats stats;
  auto files = select(tree, {}, "", &stats);
  std::vector<std::string> expected = {
      tree.at("Core/b.cpp"), tree.at("Network/src/a.cpp"),
      tree.at("Network/tests/a_test.cpp"), tree.at("ThirdParty/zlib/z.c")};
  CHECK(files == expected);
  CHECK(stats.base == 4);
  CHECK(stats.dbSkipped == 2); // asm.S and deleted.cpp
  CHECK(std::string(stats.baseSource) == "database");

  SECTION("a database that cannot enumerate its files is an error") {
    FilesDb empty({});
    std::istringstream in;
    auto r = selectSources(empty, {}, in);
    REQUIRE_FALSE(bool(r));
    CHECK(llvm::toString(r.takeError()).find("--source-list") !=
          std::string::npos);
  }
}

TEST_CASE("the recorded index set is the base only for a bare invocation",
          "[source-selection]") {
  Tree tree;
  SourceSelection sel;
  sel.recordedFiles = {tree.at("Core/b.cpp"), tree.at("Core/gone.cpp")};
  SourceSelectionStats stats;
  // Recorded paths are taken as given (existence is the bake's problem:
  // a missing TU is dropped from the index with a message).
  CHECK(select(tree, sel, "", &stats) == sel.recordedFiles);
  CHECK(std::string(stats.baseSource) == "index");

  SECTION("--source-re re-selects against the database") {
    sel.regex = "/Network/";
    CHECK(select(tree, sel, "", &stats) ==
          std::vector<std::string>{tree.at("Network/src/a.cpp"),
                                   tree.at("Network/tests/a_test.cpp")});
    CHECK(std::string(stats.baseSource) == "database");
  }
  SECTION("--source replaces it too") {
    sel.explicitFiles = {tree.at("Core/b.cpp")};
    CHECK(select(tree, sel, "", &stats) == sel.explicitFiles);
    CHECK(std::string(stats.baseSource) == "source");
  }
}

TEST_CASE("source-re and skip-paths narrow the set", "[source-selection]") {
  Tree tree;
  SourceSelection sel;
  sel.regex = "/Network/";
  SourceSelectionStats stats;
  auto files = select(tree, sel, "", &stats);
  CHECK(files == std::vector<std::string>{
                     tree.at("Network/src/a.cpp"),
                     tree.at("Network/tests/a_test.cpp")});
  CHECK(stats.regexDropped == 2);

  sel.skipPaths = {"Network/tests"};
  files = select(tree, sel, "", &stats);
  CHECK(files == std::vector<std::string>{tree.at("Network/src/a.cpp")});
  CHECK(stats.skipDropped == 1);

  SECTION("an invalid regex is an error, not an empty set") {
    FilesDb db(tree.dbEntries());
    std::istringstream in;
    SourceSelection bad;
    bad.regex = "(unclosed";
    auto r = selectSources(db, bad, in);
    REQUIRE_FALSE(bool(r));
    CHECK(llvm::toString(r.takeError()).find("invalid --source-re") !=
          std::string::npos);
  }
}

TEST_CASE("explicit files and list files are unioned, canonicalized, and "
          "filtered",
          "[source-selection]") {
  Tree tree;
  SourceSelection sel;
  sel.explicitFiles = {kAbsolutePrefix + "one.cpp",
                       kAbsolutePrefix + "two.cpp"};

  SECTION("explicit only: the database is not consulted") {
    SourceSelectionStats stats;
    CHECK(select(tree, sel, "", &stats) == sel.explicitFiles);
    CHECK(std::string(stats.baseSource) == "source");
  }

  SECTION("--source-list from a file, comments and blanks ignored") {
    std::string list =
        writeTempList("# header\n" + kAbsolutePrefix + "two.cpp\n\n  " +
                      kAbsolutePrefix + "three.cpp  \n");
    sel.listFile = list;
    auto files = select(tree, sel);
    std::remove(list.c_str());
    CHECK(files == std::vector<std::string>{kAbsolutePrefix + "one.cpp",
                                            kAbsolutePrefix + "two.cpp",
                                            kAbsolutePrefix + "three.cpp"});
  }

  SECTION("relative and dotted spellings collapse onto one TU") {
    llvm::SmallString<128> cwd;
    REQUIRE_FALSE(llvm::sys::fs::current_path(cwd));
    SourceSelection dotted;
    dotted.explicitFiles = {kAbsolutePrefix + "../x/one.cpp", "rel/./two.cpp",
                            kAbsolutePrefix + "one.cpp"};
    auto files = select(tree, dotted);
    std::string relAbs = normalizedPath(cwd) + "/rel/two.cpp";
    CHECK(files ==
          std::vector<std::string>{kAbsolutePrefix + "one.cpp", relAbs});
  }

  SECTION("--source-list - reads stdin") {
    SourceSelection fromStdin;
    fromStdin.listFile = "-";
    fromStdin.regex = "two|three";
    auto files = select(tree, fromStdin,
                        kAbsolutePrefix + "one.cpp\n" + kAbsolutePrefix +
                            "two.cpp\n" + kAbsolutePrefix + "three.cpp\n");
    CHECK(files == std::vector<std::string>{kAbsolutePrefix + "two.cpp",
                                            kAbsolutePrefix + "three.cpp"});
  }

  SECTION("a missing list file is an error with the OS reason") {
    FilesDb db(tree.dbEntries());
    std::istringstream in;
    SourceSelection missing;
    missing.listFile = "/nonexistent/list.txt";
    auto r = selectSources(db, missing, in);
    REQUIRE_FALSE(bool(r));
    std::string msg = llvm::toString(r.takeError());
    CHECK(msg.find("cannot read --source-list") != std::string::npos);
    CHECK(msg.find("o such file") != std::string::npos);
  }

  SECTION("a directory is not a list") {
    FilesDb db(tree.dbEntries());
    std::istringstream in;
    SourceSelection dir;
    dir.listFile = std::string(tree.root.str());
    auto r = selectSources(db, dir, in);
    REQUIRE_FALSE(bool(r));
    CHECK(llvm::toString(r.takeError()).find("cannot read --source-list") !=
          std::string::npos);
  }
}
