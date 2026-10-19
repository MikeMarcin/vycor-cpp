// Copyright (c) 2026 The vycor-cpp Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"
#include <climits>
#include <string>
#include <system_error>

namespace vycor {

// Own a kernel handle. Job handles must never be inherited: closing the
// parent's last handle is what kills workers when the parent terminates.
class WindowsHandle {
public:
  explicit WindowsHandle(HANDLE h = nullptr) : handle_(h) {}
  ~WindowsHandle() { reset(); }
  WindowsHandle(const WindowsHandle &) = delete;
  WindowsHandle &operator=(const WindowsHandle &) = delete;
  HANDLE get() const { return handle_; }
  explicit operator bool() const {
    return handle_ && handle_ != INVALID_HANDLE_VALUE;
  }
  void reset(HANDLE h = nullptr) {
    if (*this)
      CloseHandle(handle_);
    handle_ = h;
  }

private:
  HANDLE handle_;
};

inline std::string windowsError(DWORD code = GetLastError()) {
  return std::error_code(static_cast<int>(code), std::system_category())
      .message();
}

inline bool windowsUtf16(llvm::StringRef text, std::wstring &out) {
  if (text.contains('\0') || text.size() > INT_MAX) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }
  int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                 static_cast<int>(text.size()), nullptr, 0);
  if (!size)
    return false;
  out.resize(size);
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                             static_cast<int>(text.size()), out.data(),
                             size) != 0;
}

// Wait for descendants too, before scratch files are removed. Every process
// in this job is task-owned; no PID lookup or PID-reuse race is involved.
inline void terminateWindowsJob(HANDLE job) {
  if (!TerminateJobObject(job, 1))
    llvm::report_fatal_error("cannot terminate worker job");
  for (;;) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
    if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                                   &info, sizeof(info), nullptr))
      llvm::report_fatal_error("cannot confirm worker job termination");
    if (info.ActiveProcesses == 0)
      return;
    Sleep(1);
  }
}

} // namespace vycor
#endif
