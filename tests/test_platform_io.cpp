// Copyright (c) 2026 The vycor-cpp Authors
// SPDX-License-Identifier: Apache-2.0
#include "vycor/callgraph/AtomicFile.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/callgraph/WorkerPool.h"
#include "vycor/compat/ToolAdjusters.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include <catch2/catch_test_macros.hpp>

using namespace vycor;

namespace {
struct Directory {
  llvm::SmallString<256> path;
  Directory() {
    REQUIRE(!llvm::sys::fs::createUniqueDirectory("vycor-platform-io", path));
  }
  ~Directory() { llvm::sys::fs::remove_directories(path); }
  std::string file(const char *name) { return std::string(path) + "/" + name; }
};
} // namespace

TEST_CASE("atomic publication preserves binary bytes and mapped readers",
          "[platform][write-integrity]") {
  Directory dir;
  auto path = dir.file("data.bin");
  std::string bytes;
  for (int i = 0; i < 256; ++i)
    bytes.push_back(static_cast<char>(i));
  bytes += "\r\n\x1a\n";
  std::string error;
  REQUIRE(writeFileAtomically(
      path, [&](llvm::raw_ostream &out) { out << bytes; }, &error));
  auto old = llvm::MemoryBuffer::getFile(path, false, false);
  REQUIRE(old);
  CHECK((*old)->getBuffer() == bytes);
  // Large enough to require a mapped reader, held across replacement.
  bytes.resize(128 * 1024, 'a');
  REQUIRE(writeFileAtomically(
      path, [&](llvm::raw_ostream &out) { out << bytes; }, &error));
  auto mapped = llvm::MemoryBuffer::getFile(path, false, false);
  REQUIRE(mapped);
  REQUIRE(writeFileAtomically(
      path, [](llvm::raw_ostream &out) { out << "replacement"; }, &error));
  auto current = llvm::MemoryBuffer::getFile(path, false, false);
  REQUIRE(current);
  CHECK((*mapped)->getBuffer() == bytes);
  CHECK((*current)->getBuffer() == "replacement");
}

TEST_CASE("empty and small snapshots reload without text translation",
          "[platform][snapshot]") {
  Directory dir;
  auto path = dir.file("long-enough-to-expose-an-unterminated-rename.vycs");
  std::string error;
  REQUIRE(
      SnapshotIO::save(path, CallGraph(), ControlFlowIndex(), SnapshotMeta()));
  auto bytes = llvm::MemoryBuffer::getFile(path, false, false);
  REQUIRE(bytes);
  INFO("size=" << (*bytes)->getBufferSize());
  INFO("magic=" << (*bytes)->getBuffer().take_front(4).str());
  SnapshotLoadStats stats;
  auto loaded = SnapshotIO::load(path, &stats);
  INFO(stats.error);
  REQUIRE(loaded);
  REQUIRE(SnapshotIO::verify(path, &error));
}

TEST_CASE("worker process reports success, crash and timeout independently",
          "[platform][worker_pool]") {
  Directory dir;
  WorkerLimits limits;
  limits.timeoutSeconds = 1;
  CHECK(runWorkerProcess({VYCOR_TEST_WORKER}, dir.file("ok.log"), limits,
                         "test") == 0);
  int crash = runWorkerProcess({VYCOR_TEST_WORKER, "--crash"},
                               dir.file("crash.log"), limits, "test");
  CHECK(crash != 0);
  CHECK(crash != -1);
  CHECK(crash != kWorkerTimedOut);
  CHECK(runWorkerProcess({VYCOR_TEST_WORKER, "--sleep"}, dir.file("hang.log"),
                         limits, "test") == kWorkerTimedOut);
}

#ifdef _WIN32
TEST_CASE("worker memory budget is enforced by its job",
          "[platform][worker_pool]") {
  Directory dir;
  WorkerLimits limits;
  limits.timeoutSeconds = 20;
  REQUIRE(runWorkerProcess({VYCOR_TEST_WORKER, "--allocate"},
                           dir.file("unlimited.log"), limits, "test") == 0);
  limits.memoryLimitMB = 64;
  const int limited = runWorkerProcess({VYCOR_TEST_WORKER, "--allocate"},
                                       dir.file("limited.log"), limits, "test");
  CHECK(limited != 0);
  CHECK(limited != kWorkerTimedOut);
}
#endif

TEST_CASE("MSVC warning policy is adapted without discarding semantic flags",
          "[platform][compat]") {
  auto adjust = getStripIncompatibleFlagsAdjuster();
  clang::tooling::CommandLineArguments command = {"cl.exe",
                                                  "/WX",
                                                  "/W4",
                                                  "/EHsc",
                                                  "/DNAME=space value",
                                                  "/std:c++20",
                                                  "/c",
                                                  "source space.cpp"};
  auto adapted = adjust(command, "source space.cpp");
  command.erase(command.begin() + 1);
  CHECK(adapted == command);
}
