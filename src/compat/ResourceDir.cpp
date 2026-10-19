// Copyright (c) 2026 The vycor-cpp Authors
// SPDX-License-Identifier: Apache-2.0
#include "vycor/compat/ResourceDir.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace vycor {
const std::string &clangResourceDir() {
  static const std::string dir = []() -> std::string {
#if defined(_WIN32) && defined(VYCOR_CLANG_RESOURCE_VERSION)
    llvm::SmallString<256> installed(llvm::sys::fs::getMainExecutable(
        nullptr, reinterpret_cast<void *>(&clangResourceDir)));
    llvm::sys::path::remove_filename(installed);
    llvm::sys::path::append(installed, "..", "lib", "clang",
                            VYCOR_CLANG_RESOURCE_VERSION);
    llvm::SmallString<256> header(installed);
    llvm::sys::path::append(header, "include", "stddef.h");
    if (llvm::sys::fs::is_regular_file(header))
      return std::string(installed);
#endif
#ifdef VYCOR_CLANG_RESOURCE_DIR
    return VYCOR_CLANG_RESOURCE_DIR;
#else
    return {};
#endif
  }();
  return dir;
}
} // namespace vycor
