// Copyright (c) 2026 The vycor-cpp Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>

namespace vycor {
/// Matching resource headers shipped beside the Windows executable, or the
/// configured SDK for a development build. Never borrow another LLVM major.
const std::string &clangResourceDir();
} // namespace vycor
