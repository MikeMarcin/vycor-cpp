// Copyright (c) 2026 The vycor-cpp Authors
// SPDX-License-Identifier: Apache-2.0
// Real subprocess used by containment tests; no shell or shell quoting.
#include "vycor/callgraph/Interrupt.h"
#include "vycor/callgraph/WorkerPool.h"
#include "vycor/compat/WindowsSupport.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <thread>

int main(int argc, const char **argv) {
  llvm::InitLLVM init(argc, argv);
  const std::string self = llvm::sys::fs::getMainExecutable(
      argv[0], reinterpret_cast<void *>(&main));
  if (argc > 1 && llvm::StringRef(argv[1]) == "--sleep") {
    llvm::errs() << "WORKER-PID " << llvm::sys::Process::getProcessId() << "\n";
    for (;;)
      std::this_thread::sleep_for(std::chrono::hours(1));
  }
  if (argc > 1 && llvm::StringRef(argv[1]) == "--crash") {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
#else
    std::raise(SIGSEGV);
#endif
    return 99;
  }
  if (argc > 2 && llvm::StringRef(argv[1]) == "--controller") {
    vycor::installInterruptHandler();
    vycor::InterruptCleanup cleanup(argv[2]);
    // A second-generation worker proves the job terminates descendants too.
    int rc = vycor::runWorkerProcess({self, "--descendant", argv[2]},
                                     std::string(argv[2]) + "/worker.log", {},
                                     "controller");
    vycor::exitIfInterrupted();
    return rc;
  }
  if (argc > 2 && llvm::StringRef(argv[1]) == "--descendant") {
    llvm::errs() << "WORKER-PID " << llvm::sys::Process::getProcessId() << "\n";
    return vycor::runWorkerProcess({self, "--sleep"},
                                   std::string(argv[2]) + "/descendant.log", {},
                                   "descendant");
  }
  if (argc > 1 && llvm::StringRef(argv[1]) == "--allocate") {
    std::vector<std::unique_ptr<char[]>> allocations;
    for (int n = 0; n < 256; ++n) {
      // Touch every page, then keep it alive until the test finishes.
      allocations.push_back(std::make_unique<char[]>(1024 * 1024));
      volatile char *memory = allocations.back().get();
      for (size_t i = 0; i < 1024 * 1024; i += 4096)
        memory[i] = 1;
    }
    return 0;
  }

  std::string shard;
  double pause = 0;
  if (auto config = llvm::MemoryBuffer::getFile(self + ".vycor-test")) {
    auto lines = (*config)->getBuffer().split('\n');
    shard = lines.first.str();
    pause = std::strtod(lines.second.str().c_str(), nullptr);
  }
  std::vector<std::string> tus;
  std::string output;
  for (int i = 1; i < argc; ++i) {
    llvm::StringRef arg(argv[i]);
    if (arg == "--worker-out" && i + 1 < argc)
      output = argv[++i];
    else if (arg == "--source" && i + 1 < argc)
      tus.emplace_back(argv[++i]);
    else if (!arg.starts_with("-") && arg.ends_with(".cpp"))
      tus.emplace_back(arg);
  }
  for (const auto &tu : tus) {
    llvm::errs() << "WORKER-TU " << tu << "\n";
    if (llvm::StringRef(tu).contains("hang"))
      for (;;)
        std::this_thread::sleep_for(std::chrono::hours(1));
    std::this_thread::sleep_for(std::chrono::duration<double>(pause));
  }
  if (!shard.empty() && !output.empty())
    if (auto ec = llvm::sys::fs::copy_file(shard, output)) {
      llvm::errs() << ec.message() << "\n";
      return 1;
    }
  return 0;
}
