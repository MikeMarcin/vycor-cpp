# Native Windows x64

Use a full LLVM/Clang development SDK. The ordinary LLVM Windows installer
contains compilers but does not provide all the CMake exports, C++ headers and
link libraries needed to build this LibTooling application. WSL builds exercise
the Linux implementation; they do not validate the Windows implementation.

The Windows APIs require Windows 10 1709 or newer. The build and tests were run
on Windows 11 x64 and a local NTFS volume. Use Visual Studio 2022/2026 with
**Desktop development with C++** (including
the Windows SDK and DIA SDK), CMake, Ninja, Python 3, and the official LLVM 22.1.8
x64 MSVC development archive. Run the commands in an **x64 Native Tools Command
Prompt** or a PowerShell session with that developer environment imported.

```powershell
git clone https://github.com/bearbones/vycor-cpp.git
cd vycor-cpp
# Only Catch2 is needed when building against an installed LLVM SDK.
git submodule update --init extern/Catch2
$sdk = ./scripts/install-windows-llvm.ps1 -Destination C:/dev/llvm-sdk
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_C_COMPILER=$sdk/bin/clang-cl.exe" `
  "-DCMAKE_CXX_COMPILER=$sdk/bin/clang-cl.exe" `
  "-DLLVM_DIR=$sdk/lib/cmake/llvm" `
  "-DClang_DIR=$sdk/lib/cmake/clang" `
  -DVYCOR_USE_SUBMODULE=OFF -DVYCOR_WERROR=ON
cmake --build build --parallel 4
ctest --test-dir build --parallel 4 --output-on-failure
cmake --install build --prefix C:/dev/vycor
C:/dev/vycor/bin/vycor-cpp.exe --version
```

`install-windows-llvm.ps1` downloads the complete
`clang+llvm-22.1.8-x86_64-pc-windows-msvc.tar.xz` release asset and verifies SHA256
`d96c2cc1736f4eb7fa43cb9bbdf56d93551a9ae0a9aadb9c99c3c3b2b712a234` before
extraction. It leaves PATH and Visual Studio installations unchanged. CMake
resolves the archive's stale absolute `diaguids.lib` reference against the active
VS DIA SDK; `VYCOR_DIAGUIDS_LIBRARY` can select that library explicitly.

The installed tree contains `bin/vycor-cpp.exe` and matching
`lib/clang/22/include` resource headers. Move the whole tree together. The
executable prefers those adjacent headers, then the configured SDK for a build
tree. It does not substitute the resources of an arbitrary Clang on PATH.
The pinned Release build statically links LLVM, Clang and the C/C++ runtime;
its imports are Windows system DLLs. It needs neither an LLVM DLL directory on
PATH nor a separate MSVC redistributable. Other SDK builds may use dynamic
libraries: inspect their imports and ship their matching runtime dependencies.
Visual Studio/Windows SDK headers are still needed to analyze code that includes
the Windows or MSVC standard library headers. Resource headers are not a full
platform SDK.

## Compilation databases and paths

Pass a CMake-generated `compile_commands.json` with its original working
directories and compiler arguments. Both `cl.exe` and `clang-cl.exe` command
lines use Windows quoting. GNU-style Clang commands also work. Use `arguments`
arrays when generating a database yourself. Preserve `/D`, `/I`, `/FI`, language
mode and ABI options. Response files must exist relative to the recorded
working directory. A moved build tree or missing generated header must be fixed
in the build environment; the analyzer cannot reconstruct it.

LibTooling removes compiler outputs/dependency-file arguments and runs syntax
analysis. vycor removes `/WX` and `-WX` for the same reason it already removes
`-Werror`: warnings from the analyzing Clang need not match those of the original
compiler. Diagnostics remain visible and actual parse errors produce `partial`
TU outcomes. The tool injects its matching resource directory. These adaptations
do not change include directories, definitions, exception handling or language
mode. Precompiled module/PCH artifacts from another compiler are not portable;
use compatible source headers or rebuild a supported PCH with `--pch-dir` and
`--clang <matching-clang++.exe>`.

Paths and environment values use UTF-8 internally and UTF-16 Windows APIs at
process/file boundaries. Spaces, Unicode and shell metacharacters are passed as
arguments. `VYCOR_EXTRA_ARGS` uses Windows command-line quoting on Windows and
shell-style quoting on Unix. Source-selection regexes and normalized diagnostic
paths use `/` on Windows. Unix filenames containing literal backslashes retain
their meaning.

Always inspect `megascope info --files --index <path>` after indexing. Exit 0
alone does not establish complete parsing. A complete run has
`requested == indexed`, `partial == failed == 0`, and every file status
`indexed`. `anneal` is a subcommand of this same executable.

## Worker and file guarantees

Each isolated worker belongs to a non-inheritable, kill-on-close Windows Job
Object from the instant `CreateProcessW` creates it. The process attribute list
also restricts inherited handles to stdin and the worker log. There is no
spawn-then-attach interval and no PID-based termination race. Descendants remain
in the job. Normal worker completion, crash, timeout and cancellation terminate
remaining descendants before scratch removal. Killing the parent closes its job
handles and the kernel terminates the workers, including nested descendants.

The progress timeout restarts on each `WORKER-TU` marker, as on Unix. Windows
memory budgets limit per-process committed memory through the job; Unix uses
LLVM's address-space limits. Neither is an aggregate RSS budget. Native crash
codes are reported as Windows exception codes, not translated into Unix signal
numbers. Stack-overflow recovery reserves stack space and restores the guard
page before a thread is reused; failure to restore it is fatal.

Ctrl+C and Ctrl+Break wake a cleanup thread, which stops workers, removes
registered scratch paths and exits 130. Console close/SIGTERM handling exits
143 after cleanup; Windows can enforce its own close/logoff/shutdown deadline.
`TerminateProcess`, Task Manager forced termination and system failure cannot
run user-space cleanup. Workers still die through kernel job ownership, but
scratch directories may remain, just as after SIGKILL on Unix. Do not promise
scratch removal after forced termination.

Index writers serialize through a byte-range lock on `<index>.lock`.
`--no-wait` reports contention; other locking errors are failures. Publication
flushes the temporary file and uses one `FileRenameInfoEx` replacement with
POSIX replacement semantics. Existing mapped readers retain the old contents;
new readers see the replacement. There is no intermediate rename-away gap.
Unsupported filesystems return an error without falling back to a non-atomic
replacement. NTFS data is flushed with `FlushFileBuffers`; Windows offers no
portable equivalent of the Unix directory `fsync` used here, so persistence of
the renamed directory entry across sudden power loss is not guaranteed.

## Tests

Native CI builds with clang-cl and LLVM 22, runs CTest, then installs and moves
the result to a path containing spaces and Unicode. The install test checks
resource-header discovery without LLVM on PATH and argument-based PCH execution.
Containment tests exercise native exceptions, stack overflow, timeout progress,
memory limits, console cancellation and forced parent death with two generations
of workers. Integrity tests cover concurrent writers, lock waiting, failure
preserving the prior index, checksums, and mapped readers during replacement.

NTFS and APFS do not represent the raw invalid UTF-8 filenames used by three
Unix filesystem tests; those tests report explicit skips there. The CLI test
instead uses distinct Unicode filenames, spaces and Latin-1 source comments,
including warm refresh and isolated workers. Byte-preserving codec tests still
run everywhere. The directory-symlink test requires Windows Developer Mode or
symlink privilege and reports a skip when neither is available. Windows resolves
`..` before following a directory symlink, so its test uses a direct symlink;
the Unix test retains the climb-through-symlink case.

The Bash fixture generators and `cf-access-bench.py` (GNU `/usr/bin/time` and
Unix page-fault metrics) remain Unix developer utilities. Use the native Python
corpus runner for Windows wall-time and peak-working-set measurements. None of
these Unix benchmark results are counted as native Windows validation.
