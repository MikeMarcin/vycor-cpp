// Copyright (c) 2026 The vycor-cpp Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "llvm/Support/Path.h"

namespace vycor {
// Keep path filters, JSON paths and identities independent of which Windows
// separator a compilation database uses. Backslashes remain legal filename
// bytes on Unix and must not be rewritten there.
inline std::string normalizedPath(llvm::StringRef path) {
#ifdef _WIN32
  return llvm::sys::path::convert_to_slash(path);
#else
  return path.str();
#endif
}
} // namespace vycor
