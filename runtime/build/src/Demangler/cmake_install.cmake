# Install script for directory: /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/src/Demangler

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/output/cj")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Debug")
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
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/output/temp/bin/aarch64_Debug/cjfilt")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/cjfilt" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/cjfilt")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" -u -r "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/cjfilt")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  include("/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/build/src/Demangler/CMakeFiles/cjfilt.dir/install-cxx-module-bmi-Debug.cmake" OPTIONAL)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE FILE FILES "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/src/Demangler/CangjieDemangle.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/output/cj/runtime/lib/linux_aarch64_cjnative/libcangjie-demangle.a")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/output/cj/runtime/lib/linux_aarch64_cjnative" TYPE STATIC_LIBRARY FILES "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/output/temp/ar/aarch64_Debug/libcangjie-demangle.a")
  if(EXISTS "$ENV{DESTDIR}/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/output/cj/runtime/lib/linux_aarch64_cjnative/libcangjie-demangle.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/output/cj/runtime/lib/linux_aarch64_cjnative/libcangjie-demangle.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/output/cj/runtime/lib/linux_aarch64_cjnative/libcangjie-demangle.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  include("/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/build/src/Demangler/CMakeFiles/cangjie-demangle.dir/install-cxx-module-bmi-Debug.cmake" OPTIONAL)
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/build/src/Demangler/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
