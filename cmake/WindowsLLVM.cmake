# Some official Windows LLVM SDKs export the packager's absolute DIA path
# from LLVMDebugInfoPDB. Resolve only that missing import library against the
# active VS installation; never rewrite or remove other SDK dependencies.
if(WIN32 AND TARGET LLVMDebugInfoPDB)
  get_target_property(_pdb_links LLVMDebugInfoPDB INTERFACE_LINK_LIBRARIES)
  set(_pdb_resolved "")
  foreach(_link IN LISTS _pdb_links)
    if(_link MATCHES "[/\\\\]diaguids\\.lib$" AND NOT EXISTS "${_link}")
      find_library(VYCOR_DIAGUIDS_LIBRARY NAMES diaguids
        HINTS "$ENV{VSINSTALLDIR}/DIA SDK/lib/amd64"
        DOC "x64 DIA SDK import library for the LLVM Windows SDK")
      if(NOT VYCOR_DIAGUIDS_LIBRARY)
        message(FATAL_ERROR
          "LLVM exports a missing DIA SDK library: ${_link}. Run from an x64 "
          "VS developer shell or set VYCOR_DIAGUIDS_LIBRARY to diaguids.lib.")
      endif()
      message(STATUS "LLVM DIA SDK library: ${VYCOR_DIAGUIDS_LIBRARY}")
      list(APPEND _pdb_resolved "${VYCOR_DIAGUIDS_LIBRARY}")
    else()
      list(APPEND _pdb_resolved "${_link}")
    endif()
  endforeach()
  set_property(TARGET LLVMDebugInfoPDB PROPERTY INTERFACE_LINK_LIBRARIES "${_pdb_resolved}")
endif()
