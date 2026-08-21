# Install script for directory: /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/src/CJThread/src

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/output/common/darwin_release_aarch64")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/objdump")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/build/cjthread_build/src/base/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/build/cjthread_build/src/runtime/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/build/cjthread_build/src/sync/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/build/cjthread_build/src/syscall/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/build/cjthread_build/src/aio/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/build/cjthread_build/src/trace/cmake_install.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/output/common/darwin_release_aarch64/lib/darwin_aarch64_cjnative/libcangjie-thread.a")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/output/common/darwin_release_aarch64/lib/darwin_aarch64_cjnative" TYPE STATIC_LIBRARY FILES "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/output/temp/lib/libcangjie-thread.a")
  if(EXISTS "$ENV{DESTDIR}/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/output/common/darwin_release_aarch64/lib/darwin_aarch64_cjnative/libcangjie-thread.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/output/common/darwin_release_aarch64/lib/darwin_aarch64_cjnative/libcangjie-thread.a")
    execute_process(COMMAND "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/ranlib" "$ENV{DESTDIR}/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/output/common/darwin_release_aarch64/lib/darwin_aarch64_cjnative/libcangjie-thread.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "cangjie-thread" OR NOT CMAKE_INSTALL_COMPONENT)
  execute_process(COMMAND objcopy -D --rename-section .text=.text_thread             /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/output/common/darwin_release_aarch64/lib/darwin_aarch64_cjnative/libcangjie-thread.a)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "cangjie-thread" OR NOT CMAKE_INSTALL_COMPONENT)
  execute_process(COMMAND  -x /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/output/common/darwin_release_aarch64/lib/libcangjie-thread.a)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/output/common/darwin_release_aarch64/lib/darwin_aarch64_cjnative/libcangjie-aio.a")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/output/common/darwin_release_aarch64/lib/darwin_aarch64_cjnative" TYPE STATIC_LIBRARY FILES "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/output/temp/lib/libcangjie-aio.a")
  if(EXISTS "$ENV{DESTDIR}/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/output/common/darwin_release_aarch64/lib/darwin_aarch64_cjnative/libcangjie-aio.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/output/common/darwin_release_aarch64/lib/darwin_aarch64_cjnative/libcangjie-aio.a")
    execute_process(COMMAND "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/ranlib" "$ENV{DESTDIR}/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/output/common/darwin_release_aarch64/lib/darwin_aarch64_cjnative/libcangjie-aio.a")
  endif()
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/build/cjthread_build/src/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
