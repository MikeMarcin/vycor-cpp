// Copyright (c) 2026 The vycor-cpp Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "vycor/callgraph/WorkerPool.h"
#include "vycor/compat/WindowsSupport.h"
#include "llvm/Support/Program.h"

namespace vycor {

// One job per worker: timeout, cancellation and parent death also terminate
// descendants. Association at CreateProcess time avoids the orphan window
// between spawning a suspended process and AssignProcessToJobObject.
class WindowsWorker {
public:
  ~WindowsWorker() {
    if (job_)
      terminateWindowsJob(job_.get());
  }

  intptr_t spawn(const std::vector<std::string> &argv,
                 const std::string &logPath, unsigned memoryLimitMB,
                 std::string &error) {
    auto fail = [&]() -> intptr_t {
      error = windowsError();
      return 0;
    };
    std::wstring program, log;
    if (!windowsUtf16(argv.front(), program) || !windowsUtf16(logPath, log))
      return fail();
    std::vector<llvm::StringRef> args(argv.begin(), argv.end());
    auto command = llvm::sys::flattenWindowsCommandLine(args);
    if (!command) {
      error = command.getError().message();
      return 0;
    }
    job_.reset(CreateJobObjectW(nullptr, nullptr));
    if (!job_)
      return fail();
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (memoryLimitMB) {
      limits.BasicLimitInformation.LimitFlags |=
          JOB_OBJECT_LIMIT_PROCESS_MEMORY;
      limits.ProcessMemoryLimit =
          static_cast<SIZE_T>(memoryLimitMB) * 1024 * 1024;
    }
    if (!SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)))
      return fail();

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    WindowsHandle input(CreateFileW(L"NUL", GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &security, OPEN_EXISTING, 0, nullptr));
    WindowsHandle output(
        CreateFileW(log.c_str(), GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!input || !output)
      return fail();
    HANDLE handles[] = {input.get(), output.get()};
    HANDLE job = job_.get();
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
    std::vector<unsigned char> storage(size);
    auto *attributes =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 2, 0, &size))
      return fail();
    struct AttributeCleanup {
      LPPROC_THREAD_ATTRIBUTE_LIST list;
      ~AttributeCleanup() { DeleteProcThreadAttributeList(list); }
    } cleanup{attributes};
    if (!UpdateProcThreadAttribute(attributes, 0,
                                   PROC_THREAD_ATTRIBUTE_JOB_LIST, &job,
                                   sizeof(job), nullptr, nullptr) ||
        !UpdateProcThreadAttribute(attributes, 0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                   sizeof(handles), nullptr, nullptr))
      return fail();
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.get();
    startup.StartupInfo.hStdOutput = output.get();
    startup.StartupInfo.hStdError = output.get();
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(program.c_str(), command->data(), nullptr, nullptr,
                        TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup.StartupInfo, &process))
      return fail();
    process_.reset(process.hProcess);
    CloseHandle(process.hThread);
    return reinterpret_cast<intptr_t>(job_.get());
  }

  // 1 = exited, 0 = still running, -1 = wait failed. NT exception exit
  // codes are failures, not POSIX wait statuses (no WIFEXITED decoding).
  int wait(bool block, int &code) {
    DWORD result = WaitForSingleObject(process_.get(), block ? INFINITE : 0);
    if (result == WAIT_TIMEOUT)
      return 0;
    DWORD status = 0;
    if (result != WAIT_OBJECT_0 || !GetExitCodeProcess(process_.get(), &status))
      return -1;
    code = static_cast<int>(status);
    return 1;
  }

  void terminate() { terminateWindowsJob(job_.get()); }

private:
  WindowsHandle job_;
  WindowsHandle process_;
};

} // namespace vycor
