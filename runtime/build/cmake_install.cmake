# Install script for directory: /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime

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

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/build/src/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/build/third_party/third_party_bounds_checking_function/cmake_install.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/runtime/lib/linux_aarch64_cjnative" TYPE SHARED_LIBRARY FILES "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/output/temp/lib/aarch64_Debug/libcangjie-runtime.dylib")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/runtime/lib/linux_aarch64_cjnative/libcangjie-runtime.dylib" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/runtime/lib/linux_aarch64_cjnative/libcangjie-runtime.dylib")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" -x "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/runtime/lib/linux_aarch64_cjnative/libcangjie-runtime.dylib")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  include("/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/build/src/CMakeFiles/cangjie-runtime.dir/install-cxx-module-bmi-Debug.cmake" OPTIONAL)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE FILE FILES
    "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/src/RuntimeConfig.h"
    "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/src/Cangjie.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/linux_aarch64_cjnative" TYPE PROGRAM FILES
    "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/build/lds/aarch64_linux/cjld.lds"
    "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/build/lds/aarch64_linux/discard_eh_frame.lds"
    "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/build/lds/aarch64_linux/cld.lds"
    "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/build/lds/aarch64_linux/cjld.shared.lds"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/linux_aarch64_cjnative" TYPE PROGRAM RENAME "cjstart.o" FILES "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/build/CMakeFiles/cjstart.dir/./src/arch/aarch64_linux/Cjstart.S.o")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/linux_aarch64_cjnative" TYPE STATIC_LIBRARY FILES "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/output/temp/ar/aarch64_Debug/libcangjie-runtime.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/linux_aarch64_cjnative/libcangjie-runtime.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/linux_aarch64_cjnative/libcangjie-runtime.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/linux_aarch64_cjnative/libcangjie-runtime.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "cangjie-runtime" OR NOT CMAKE_INSTALL_COMPONENT)
  execute_process(COMMAND objcopy --rename-section .text=.text_runtime                     /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/output/cj/lib/linux_aarch64_cjnative/libcangjie-runtime.a)
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/build/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
if(CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_COMPONENT MATCHES "^[a-zA-Z0-9_.+-]+$")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
  else()
    string(MD5 CMAKE_INST_COMP_HASH "${CMAKE_INSTALL_COMPONENT}")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INST_COMP_HASH}.txt")
    unset(CMAKE_INST_COMP_HASH)
  endif()
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/build/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
