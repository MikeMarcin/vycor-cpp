add_library(vycor_lib
  anneal/GlobalIndex.cpp
  anneal/Indexer.cpp
  anneal/Analyzer.cpp
  anneal/Checkpoint.cpp
  anneal/CheckSet.cpp
  anneal/DeadCodeAnalyzer.cpp
  anneal/Report.cpp
  morph/MatcherEngine.cpp
  morph/TemplateEngine.cpp
  morph/RulesParser.cpp
  morph/TransformPipeline.cpp
  compat/PchCache.cpp
  compat/ResourceDir.cpp
  callgraph/AtomicFile.cpp
  callgraph/CallGraph.cpp
  callgraph/CallGraphBuilder.cpp
  callgraph/CollapseFilter.cpp
  callgraph/ControlFlowIndex.cpp
  callgraph/ControlFlowContextVisitor.cpp
  callgraph/ChannelIndex.cpp
  callgraph/ControlFlowOracle.cpp
  callgraph/InputFingerprint.cpp
  callgraph/PathSearch.cpp
  callgraph/Snapshot.cpp
  callgraph/WorkerPool.cpp
  callgraph/Interrupt.cpp
  callgraph/CrashGuard.cpp
  callgraph/RetainUntilExit.cpp
  query/Serialize.cpp
  query/Identity.cpp
  query/GraphTools.cpp
  query/ExceptionTools.cpp
  query/LockTools.cpp
  query/DeadCodeTools.cpp
  query/ChannelTools.cpp
  query/Registry.cpp
  query/Limits.cpp
  query/ImpactTools.cpp
  impact/SemanticDiff.cpp
  impact/ImpactSearch.cpp
  impact/PatchMapping.cpp
  cli/BakeConfig.cpp
  cli/DiffVerb.cpp
  cli/MegascopeCli.cpp
  cli/SourceSelection.cpp
  ext/Extensions.cpp
  ext/OrgConfig.cpp
)

target_include_directories(vycor_lib PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}/generated
)

target_compile_options(vycor_lib PRIVATE ${VYCOR_WARNING_FLAGS})
if(WIN32)
  target_compile_definitions(vycor_lib PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(vycor_lib PUBLIC psapi)
endif()

target_link_libraries(vycor_lib PUBLIC
  clangTooling
  clangDynamicASTMatchers
  clangASTMatchers
  clangAST
  clangIndex
  clangBasic
  clangDriver
  clangFrontend
  clangSema
  clangSerialization
  clangRewrite
  clangToolingCore
  LLVMSupport
)

# Embed the Clang resource directory so the tool can find built-in headers
# (stdarg.h, etc.) when consuming compilation databases from other toolchains.
if(LLVM_FOUND)
  execute_process(
    COMMAND "${LLVM_TOOLS_BINARY_DIR}/clang${CMAKE_EXECUTABLE_SUFFIX}" -print-resource-dir
    OUTPUT_VARIABLE _VYCOR_RESOURCE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(_VYCOR_RESOURCE_DIR)
    file(TO_CMAKE_PATH "${_VYCOR_RESOURCE_DIR}" _VYCOR_RESOURCE_DIR)
    target_compile_definitions(vycor_lib PRIVATE
      "VYCOR_CLANG_RESOURCE_VERSION=\"${LLVM_VERSION_MAJOR}\"")
    # ResourceDir.cpp resolves this fallback or the adjacent installed headers.
    # All consumers, including input fingerprinting, use that same function.
    target_compile_definitions(vycor_lib PRIVATE
      "VYCOR_CLANG_RESOURCE_DIR=\"${_VYCOR_RESOURCE_DIR}\"")
  endif()
endif()

# On macOS, detect the default SDK sysroot and libc++ include path so that
# ClangTool can find C++ stdlib headers even when the compilation database
# doesn't provide -isysroot (e.g. FixedCompilationDatabase in tests, or
# standalone invocations without a game-engine build path).
if(APPLE)
  execute_process(
    COMMAND xcrun --show-sdk-path
    OUTPUT_VARIABLE _VYCOR_DEFAULT_SYSROOT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(_VYCOR_DEFAULT_SYSROOT)
    message(STATUS "Default macOS sysroot: ${_VYCOR_DEFAULT_SYSROOT}")
    # PUBLIC: getSysrootAdjuster's inline fallback is #ifdef'd on this macro.
    # All translation units must see the same body to avoid an ODR violation.
    target_compile_definitions(vycor_lib PUBLIC
      "VYCOR_DEFAULT_SYSROOT=\"${_VYCOR_DEFAULT_SYSROOT}\"")
  endif()
endif()

# Organization extension slot-in (docs/EXTENDING.md): every .cpp directly
# under <repo>/ext/ is compiled into the executable. Deliberately NOT part
# of vycor_lib — these files typically contain only static registrars
# (VYCOR_REGISTER_ANNEAL_CHECK / VYCOR_EXTENSION_SETUP), and the linker
# drops unreferenced members of a static archive, silently discarding the
# registration. Objects listed directly on the target are always linked.
file(GLOB VYCOR_EXT_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/ext/*.cpp")
if(VYCOR_EXT_SOURCES)
  message(STATUS "Organization extensions: ${VYCOR_EXT_SOURCES}")
endif()

add_executable(vycor-cpp main.cpp ${VYCOR_EXT_SOURCES} ${VYCOR_SANITIZER_SOURCES})
target_link_libraries(vycor-cpp PRIVATE vycor_lib)
target_compile_options(vycor-cpp PRIVATE ${VYCOR_WARNING_FLAGS})

# Embed the default --clang path for PCH compilation into the executable.
target_compile_definitions(vycor-cpp PRIVATE
  "VYCOR_DEFAULT_CLANG=\"${VYCOR_DEFAULT_CLANG}\"")

# Release packaging: `cmake --install build --prefix <dir>` stages a single
# self-contained bin/ dir that release.yml tars up. No CPack -- one binary
# doesn't need it (see docs/toolchain-portability.md for why this project
# keeps packaging minimal).
include(GNUInstallDirs)
install(TARGETS vycor-cpp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(WIN32)
  if(NOT EXISTS "${_VYCOR_RESOURCE_DIR}/include/stddef.h")
    message(FATAL_ERROR "The Windows LLVM SDK must include matching Clang resource headers")
  endif()
  install(DIRECTORY "${_VYCOR_RESOURCE_DIR}/include"
    DESTINATION "lib/clang/${LLVM_VERSION_MAJOR}")
endif()
