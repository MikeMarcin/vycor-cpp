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

#include "vycor/callgraph/Interrupt.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include "vycor/compat/WindowsSupport.h"
#include <csignal>
#else
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <sys/prctl.h>
#endif

namespace vycor {

namespace {

// Name of the environment variable a parent sets to its own pid, so a
// worker can tell whether the parent died before it bound itself to it.
#ifndef _WIN32
constexpr const char *kParentPidEnv = "VYCOR_WORKER_PARENT_PID";
#endif

// Leaked on purpose: the watcher thread may touch it while static
// destructors run at exit.
struct Registry {
  std::mutex mu;
  std::unordered_set<detail::ChildId> children;
  std::vector<std::string> paths; // multiset: the same path may nest
  bool interrupted = false;
};

Registry &registry() {
  static Registry *r = new Registry;
  return *r;
}

std::atomic<bool> g_interrupted{false};
std::once_flag g_installOnce;

#ifndef _WIN32
void blockedSet(sigset_t &set) {
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
}

#else
HANDLE g_interruptEvent = nullptr;
std::atomic<DWORD> g_consoleEvent{CTRL_C_EVENT};

BOOL WINAPI consoleHandler(DWORD event) {
  if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT &&
      event != CTRL_CLOSE_EVENT && event != CTRL_LOGOFF_EVENT &&
      event != CTRL_SHUTDOWN_EVENT)
    return FALSE;
  g_consoleEvent.store(event);
  SetEvent(g_interruptEvent);
  // Close/logoff handlers must remain alive until cleanup finishes. Windows
  // may still impose its shutdown deadline; forced termination cannot run
  // user-space cleanup, but kernel job ownership still kills every worker.
  if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
    Sleep(INFINITE);
  return TRUE;
}

void crtInterrupt(int sig) {
  g_consoleEvent.store(sig == SIGINT ? CTRL_C_EVENT : CTRL_CLOSE_EVENT);
  SetEvent(g_interruptEvent);
}
#endif

#ifdef _WIN32
[[noreturn]] void watcherMain() {
  WaitForSingleObject(g_interruptEvent, INFINITE);
  const char *reason = g_consoleEvent.load() == CTRL_C_EVENT ? "Ctrl+C"
                       : g_consoleEvent.load() == CTRL_BREAK_EVENT
                           ? "Ctrl+Break"
                           : "console close/SIGTERM";
#else
[[noreturn]] void watcherMain(sigset_t set) {
  int sig = 0;
  while (sigwait(&set, &sig) != 0) {
  }
  const char *reason = sig == SIGINT ? "SIGINT" : "SIGTERM";
#endif
  std::vector<std::string> paths;
  size_t killed = 0;
  {
    Registry &r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    r.interrupted = true;
    g_interrupted.store(true);
    for (auto child : r.children) {
#ifdef _WIN32
      terminateWindowsJob(reinterpret_cast<HANDLE>(child));
      ++killed;
#else
      if (::kill(static_cast<pid_t>(child), SIGKILL) == 0)
        ++killed;
#endif
    }
    paths = r.paths;
  }
  llvm::errs() << "vycor-cpp: interrupted (" << reason << "): killed " << killed
               << " worker(s), removing " << paths.size()
               << " scratch path(s)\n";
  // Innermost registrations last in, first out (a file inside a registered
  // directory goes before the directory).
  for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
    if (llvm::sys::fs::is_directory(*it))
      llvm::sys::fs::remove_directories(*it, /*IgnoreErrors=*/true);
    else
      llvm::sys::fs::remove(*it);
  }
  // Files registered with RemoveFileOnSignal (llvm::sys::fs::TempFile).
  llvm::sys::RunInterruptHandlers();

#ifdef _WIN32
  const DWORD event = g_consoleEvent.load();
  std::_Exit(event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ? 130 : 143);
#else
  struct sigaction dfl {};
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  sigaction(sig, &dfl, nullptr);
  sigset_t one;
  sigemptyset(&one);
  sigaddset(&one, sig);
  pthread_sigmask(SIG_UNBLOCK, &one, nullptr);
  ::raise(sig);
  // Not reached unless the signal was somehow ignored.
  std::_Exit(128 + sig);
#endif
}

} // namespace

void installInterruptHandler() {
  std::call_once(g_installOnce, [] {
#ifdef _WIN32
    g_interruptEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // SSH and other launchers can inherit an ignored Ctrl+C flag.
    if (!g_interruptEvent || !SetConsoleCtrlHandler(nullptr, FALSE) ||
        !SetConsoleCtrlHandler(consoleHandler, TRUE)) {
      llvm::errs() << "vycor-cpp: cannot install interrupt handler: "
                   << windowsError() << "\n";
      std::exit(1);
    }
    std::signal(SIGINT, crtInterrupt);
    std::signal(SIGTERM, crtInterrupt);
    std::thread(watcherMain).detach();
#else
    sigset_t set;
    blockedSet(set);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    // Workers read this to detect a parent that died before they bound to
    // it (bindWorkerToParent).
    ::setenv(kParentPidEnv, std::to_string(::getpid()).c_str(), 1);
    std::thread(watcherMain, set).detach();
#endif
  });
}

bool interruptRequested() { return g_interrupted.load(); }

void exitIfInterrupted() {
  if (!interruptRequested())
    return;
  // The signals stay blocked in this thread; the watcher unblocks and
  // re-raises in its own thread once cleanup is done, which ends the
  // process. Sleep until then.
  for (;;)
    std::this_thread::sleep_for(std::chrono::seconds(60));
}

void bindWorkerToParent() {
#ifdef _WIN32
  // WindowsWorker assigns the non-inheritable kill-on-close job atomically
  // with process creation. There is no parent-PID poll or association race.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#else
  sigset_t set;
  blockedSet(set);
  pthread_sigmask(SIG_UNBLOCK, &set, nullptr);

  long expected = 0;
  if (const char *env = std::getenv(kParentPidEnv))
    expected = std::atol(env);
#if defined(__linux__)
  ::prctl(PR_SET_PDEATHSIG, SIGKILL);
#else
  // No parent-death signal: poll for reparenting.
  const pid_t parent = ::getppid();
  std::thread([parent] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      if (::getppid() != parent)
        std::_Exit(1);
    }
  }).detach();
#endif
  // The parent may have died between the spawn and the line above.
  if (expected > 0 && ::getppid() != static_cast<pid_t>(expected))
    std::_Exit(1);
#endif
}

InterruptCleanup::InterruptCleanup(std::string path) : path_(std::move(path)) {
  Registry &r = registry();
  std::lock_guard<std::mutex> lock(r.mu);
  r.paths.push_back(path_);
}

InterruptCleanup::~InterruptCleanup() {
  Registry &r = registry();
  std::lock_guard<std::mutex> lock(r.mu);
  for (auto it = r.paths.rbegin(); it != r.paths.rend(); ++it)
    if (*it == path_) {
      r.paths.erase(std::next(it).base());
      break;
    }
}

namespace detail {

ChildId spawnTrackedChild(llvm::function_ref<ChildId()> spawn) {
  Registry &r = registry();
  std::lock_guard<std::mutex> lock(r.mu);
  if (r.interrupted)
    return 0;
  ChildId pid = spawn();
  if (pid != 0)
    r.children.insert(pid);
  return pid;
}

void untrackChild(ChildId pid) {
  Registry &r = registry();
  std::lock_guard<std::mutex> lock(r.mu);
  r.children.erase(pid);
}

} // namespace detail

} // namespace vycor
