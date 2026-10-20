# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.
cmake_minimum_required(VERSION 3.24)
project(MiniElectronDawn LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/lib")
set(DAWN_BUILD_GEN_DIR "${CMAKE_BINARY_DIR}/gen" CACHE PATH "" FORCE)

# Chromium and Dawn pin different Abseil revisions. Their C++ layouts are not
# interchangeable: isolate Dawn's symbols without modifying verified sources.
file(READ "${MINI_ELECTRON_DAWN_SOURCE}/third_party/abseil-cpp/absl/base/options.h" absl_options)
if(NOT absl_options MATCHES "#define ABSL_OPTION_USE_INLINE_NAMESPACE 0"
   OR NOT absl_options MATCHES "#define ABSL_OPTION_INLINE_NAMESPACE_NAME head")
  message(FATAL_ERROR "Unexpected pinned Dawn Abseil namespace configuration")
endif()
string(REPLACE "#define ABSL_OPTION_USE_INLINE_NAMESPACE 0"
               "#define ABSL_OPTION_USE_INLINE_NAMESPACE 1" absl_options "${absl_options}")
string(REPLACE "#define ABSL_OPTION_INLINE_NAMESPACE_NAME head"
               "#define ABSL_OPTION_INLINE_NAMESPACE_NAME mini_electron_dawn" absl_options "${absl_options}")
set(absl_override "${CMAKE_BINARY_DIR}/absl-include")
file(MAKE_DIRECTORY "${absl_override}/absl/base")
file(CONFIGURE OUTPUT "${absl_override}/absl/base/options.h" CONTENT "${absl_options}" @ONLY)
include_directories(BEFORE "${absl_override}")
add_subdirectory("${MINI_ELECTRON_DAWN_SOURCE}" dawn EXCLUDE_FROM_ALL)

# Export the actual static dependency closure, not a hand-maintained list of
# Tint/Abseil libraries. No bundle target or second copy of Dawn is built.
function(collect_dawn_archives target)
  get_target_property(alias "${target}" ALIASED_TARGET)
  if(alias)
    set(target "${alias}")
  endif()
  get_property(visited GLOBAL PROPERTY MINI_ELECTRON_DAWN_VISITED)
  if(target IN_LIST visited)
    return()
  endif()
  set_property(GLOBAL APPEND PROPERTY MINI_ELECTRON_DAWN_VISITED "${target}")
  get_target_property(type "${target}" TYPE)
  if(type STREQUAL "STATIC_LIBRARY")
    set_property(GLOBAL APPEND PROPERTY MINI_ELECTRON_DAWN_ARCHIVES "$<TARGET_FILE:${target}>")
  endif()
  # Target names inside LINK_ONLY / BUILD_INTERFACE / TARGET_OBJECTS generator
  # expressions are deliberately included. Non-target system libraries aren't.
  foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES SOURCES)
    get_target_property(dependencies "${target}" "${property}")
    string(REGEX MATCHALL "[A-Za-z0-9_.:-]+" tokens "${dependencies}")
    foreach(dependency IN LISTS tokens)
      # The ':' introducing a generator expression is not part of the name.
      string(REGEX REPLACE "^(LINK_ONLY|BUILD_INTERFACE|TARGET_OBJECTS):" "" dependency "${dependency}")
      if(TARGET "${dependency}")
        collect_dawn_archives("${dependency}")
      endif()
    endforeach()
  endforeach()
endfunction()
foreach(target dawn_native dawn_wire dawn_proc)
  collect_dawn_archives("${target}")
endforeach()
get_property(archives GLOBAL PROPERTY MINI_ELECTRON_DAWN_ARCHIVES)
string(JOIN "\n" archive_manifest ${archives})
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/archives-$<CONFIG>.txt" CONTENT "${archive_manifest}\n")
add_custom_target(mini_electron_dawn DEPENDS dawn_native dawn_wire dawn_proc)
