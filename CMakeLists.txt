cmake_minimum_required(VERSION 3.20)
# The version goes up with each milestone of docs/gui-plan.html: M1 is 0.1.0, M2 is 0.2.0,
# M3.5 is 0.3.5, M4 is 0.4.0, M5 is 0.5.0, M6 is 0.6.0, M6.5 is 0.6.5, M6.6 is 0.6.6, M6.6.2 is
# 0.6.6.2, M6.7 is 0.6.7, M7 is 0.7.0, M8 is 0.8.0, M9.5 (native code, docs/native-plan.html) is 0.9.5.
# Between milestones each minor build counts up after it (the user, 2026-11-17): 0.9.5001, 0.9.5002...
project(WinterWolf VERSION 0.9.5077 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Link the C++ runtime into the executable: ww.exe, and the programs that
# `ww build --exe` makes from it, then run on machines without Visual Studio's runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The standard library is written in WinterWolf (src/std/*.ww) and carried inside ww.exe.
file(GLOB WW_STD CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/std/*.ww")
list(SORT WW_STD)
string(REPLACE ";" "|" WW_STD_ARG "${WW_STD}")
add_custom_command(
  OUTPUT "${CMAKE_BINARY_DIR}/std_source.cpp"
  COMMAND ${CMAKE_COMMAND} "-DOUT=${CMAKE_BINARY_DIR}/std_source.cpp" "-DFILES=${WW_STD_ARG}"
          -P "${CMAKE_SOURCE_DIR}/cmake/embed.cmake"
  DEPENDS ${WW_STD} "${CMAKE_SOURCE_DIR}/cmake/embed.cmake"
  COMMENT "Packing the standard library"
  VERBATIM)

# The libraries that come with WinterWolf (lib/: Jade) are carried inside ww.exe too
# (2026-11-18), so that a lone ww.exe runs a program that imports them; a lib folder beside it,
# where there is one, is looked in first (src/command.cpp, libraryPaths).
file(GLOB_RECURSE WW_LIB RELATIVE "${CMAKE_SOURCE_DIR}/lib" CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/lib/*")
list(SORT WW_LIB)
string(REPLACE ";" "|" WW_LIB_ARG "${WW_LIB}")
list(TRANSFORM WW_LIB PREPEND "${CMAKE_SOURCE_DIR}/lib/" OUTPUT_VARIABLE WW_LIB_FILES)
add_custom_command(
  OUTPUT "${CMAKE_BINARY_DIR}/lib_source.cpp"
  COMMAND ${CMAKE_COMMAND} "-DOUT=${CMAKE_BINARY_DIR}/lib_source.cpp" "-DROOT=${CMAKE_SOURCE_DIR}/lib" "-DFILES=${WW_LIB_ARG}"
          -P "${CMAKE_SOURCE_DIR}/cmake/embed_lib.cmake"
  DEPENDS ${WW_LIB_FILES} "${CMAKE_SOURCE_DIR}/cmake/embed_lib.cmake"
  COMMENT "Packing the libraries that come with WinterWolf"
  VERBATIM)

# The executable in three parts (M6.5 part 5, D161): what every program needs to run, Jade's
# window with the text engine and the image codecs, and the compiler side with the ww command.
# The last two are compiled into sections of their own (src/section_*.h), which `ww build`
# leaves out of a built program that does not need them.
add_library(ww_core OBJECT
  src/value.cpp
  src/bytecode.cpp
  src/vm.cpp
  src/builtins.cpp
  src/stdlib.cpp
  src/reflect.cpp
  src/threads.cpp
  src/profile.cpp
  src/io.cpp
  src/host.cpp
  src/sandbox.cpp
  src/registers.cpp
  src/charset.cpp
  src/grid.cpp
  src/regex.cpp
  src/unicode.cpp
  src/regex_api.cpp
  src/xml_scan.cpp
  src/codec.cpp
  src/bits_api.cpp
  src/picture.cpp
  src/numeric.cpp
  src/fmath.cpp
  src/tensor.cpp
  src/webp.cpp
  src/foreign.cpp
  src/worker.cpp
  src/parallel.cpp
  src/run.cpp
  src/crypto/random.cpp
  src/crypto/common.cpp
  src/crypto/sha2.cpp
  src/crypto/sha3.cpp
  src/crypto/mac.cpp
  src/crypto/argon2.cpp
  src/crypto/aes_gcm.cpp
  src/crypto/chacha_poly.cpp
  src/crypto/curve25519.cpp
  src/crypto/ec.cpp
  src/crypto/rsa.cpp
  src/crypto/api.cpp
)
# WinterWolf's own math functions give the same bits everywhere: no multiply and add fused into
# one step there (o-complex-linux). The file says so to Clang and MSVC itself; this says it to
# GCC, and to Clang told -ffp-contract=fast, which leaves the file's word aside.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
  set_source_files_properties(src/fmath.cpp PROPERTIES COMPILE_OPTIONS "/clang:-ffp-contract=off")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(src/fmath.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
add_library(ww_window OBJECT
  src/jade/platform_win.cpp
  src/jade/portal_linux.cpp
  src/jade/gl.cpp
  src/jade/render.cpp
  src/jade/api.cpp
  src/text/face.cpp
  src/text/cff.cpp
  src/text/variation.cpp
  src/text/raster.cpp
  src/text/color.cpp
  src/text/bitmaps.cpp
  src/text/system.cpp
  src/text/fonts.cpp
  src/text/engine.cpp
  src/text/shapes.cpp
  src/text/api.cpp
  src/image/png.cpp
)
add_library(ww_compiler OBJECT
  "${CMAKE_BINARY_DIR}/std_source.cpp"
  "${CMAKE_BINARY_DIR}/lib_source.cpp"
  src/lexer.cpp
  src/parser.cpp
  src/checker.cpp
  src/cache.cpp
  src/compiler.cpp
  src/command.cpp
  src/script.cpp
  src/snippy.cpp
  src/doc.cpp
)
# Jade's windows on Linux (docs/linux-gui-plan.html): Wayland, talked to directly, as Win32 is on
# Windows. ww is linked with none of the system's libraries for it (wayland-client, wayland-egl,
# EGL, xkbcommon): src/jade/platform_linux.cpp opens them as the first window is made, so ww runs
# where they are missing. The descriptions of the protocols it speaks are made from their XML by
# wayland-scanner as ww is built, and included by platform_linux.cpp; where the scanner, the
# protocols or the headers are missing, ww is built with no windows on Linux, as before.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_program(WW_WAYLAND_SCANNER wayland-scanner)
  set(WW_WAYLAND_CORE /usr/share/wayland/wayland.xml)
  set(WW_PROTOCOLS /usr/share/wayland-protocols)
  set(WW_WAYLAND_EXTENSIONS
      "xdg-shell|${WW_PROTOCOLS}/stable/xdg-shell/xdg-shell.xml"
      "viewporter|${WW_PROTOCOLS}/stable/viewporter/viewporter.xml"
      "fractional-scale|${WW_PROTOCOLS}/staging/fractional-scale/fractional-scale-v1.xml"
      "xdg-decoration|${WW_PROTOCOLS}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"
      "xdg-activation|${WW_PROTOCOLS}/staging/xdg-activation/xdg-activation-v1.xml"
      "pointer-constraints|${WW_PROTOCOLS}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml"
      "relative-pointer|${WW_PROTOCOLS}/unstable/relative-pointer/relative-pointer-unstable-v1.xml"
      "xdg-foreign|${WW_PROTOCOLS}/unstable/xdg-foreign/xdg-foreign-unstable-v2.xml")
  set(WW_WAYLAND_FOUND ON)
  foreach(needed ${WW_WAYLAND_CORE} /usr/include/wayland-client-core.h /usr/include/wayland-egl-core.h /usr/include/EGL/egl.h
          /usr/include/xkbcommon/xkbcommon.h /usr/include/xkbcommon/xkbcommon-compose.h)
    if(NOT EXISTS ${needed})
      set(WW_WAYLAND_FOUND OFF)
    endif()
  endforeach()
  foreach(extension ${WW_WAYLAND_EXTENSIONS})
    string(REPLACE "|" ";" parts "${extension}")
    list(GET parts 1 xml)
    if(NOT EXISTS ${xml})
      set(WW_WAYLAND_FOUND OFF)
    endif()
  endforeach()
  if(WW_WAYLAND_SCANNER AND WW_WAYLAND_FOUND)
    set(made "${CMAKE_BINARY_DIR}/wayland")
    file(MAKE_DIRECTORY ${made})
    set(WW_WAYLAND_MADE "${made}/core-protocol.c")
    add_custom_command(OUTPUT "${made}/core-protocol.c"
                       COMMAND ${WW_WAYLAND_SCANNER} private-code ${WW_WAYLAND_CORE} "${made}/core-protocol.c"
                       DEPENDS ${WW_WAYLAND_CORE} VERBATIM)
    foreach(extension ${WW_WAYLAND_EXTENSIONS})
      string(REPLACE "|" ";" parts "${extension}")
      list(GET parts 0 name)
      list(GET parts 1 xml)
      add_custom_command(OUTPUT "${made}/${name}-protocol.c" "${made}/${name}-client.h"
                         COMMAND ${WW_WAYLAND_SCANNER} private-code ${xml} "${made}/${name}-protocol.c"
                         COMMAND ${WW_WAYLAND_SCANNER} client-header ${xml} "${made}/${name}-client.h"
                         DEPENDS ${xml} VERBATIM)
      list(APPEND WW_WAYLAND_MADE "${made}/${name}-protocol.c" "${made}/${name}-client.h")
    endforeach()
    add_custom_target(ww_wayland_protocols DEPENDS ${WW_WAYLAND_MADE})
    target_sources(ww_window PRIVATE src/jade/platform_linux.cpp)
    set_source_files_properties(src/jade/platform_linux.cpp PROPERTIES OBJECT_DEPENDS "${WW_WAYLAND_MADE}")
    add_dependencies(ww_window ww_wayland_protocols)
    target_include_directories(ww_window PRIVATE ${made})
    target_compile_definitions(ww_window PRIVATE WW_WAYLAND=1)
  else()
    message(STATUS "Jade's windows on Linux left out: wayland-scanner, the Wayland protocols or the headers of wayland-client, wayland-egl, EGL or xkbcommon are missing")
  endif()
endif()

# Native code (M9.5, docs/native-plan.html): the register stencils compiled by Clang into an
# object file of their own, read by tools/stencils into a table that ww.exe carries, and checked
# by ww itself as the build's step. With Clang on x64 Windows (clang-cl, COFF) and on x86-64
# Linux (ELF); elsewhere ww is built without native code (native.cpp then has no stencils).
target_sources(ww_core PRIVATE src/native/native.cpp src/native/regions.cpp)
set(WW_NATIVE_HERE OFF)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$" AND
   (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
  set(WW_NATIVE_HERE ON)
endif()
if(WW_NATIVE_HERE)
  add_library(ww_stencils OBJECT src/native/stencils.cpp)
  # Each stencil in a section of its own, and nothing in them a copied stencil could not carry
  # (no library function called: no builtins, which keeps Clang from making a loop a call of
  # memcpy; no table of where a switch goes, which would be data of its own; on Linux no code
  # reached through a table either, and nothing for unwinding or stack checks; and never a
  # sanitizer's checks, in a build that has them, which would call its library).
  if(WIN32)
    target_compile_options(ww_stencils PRIVATE /O2 /GS- /Gy /Zl /Oi- /clang:-fno-jump-tables /clang:-fno-sanitize=all)
  else()
    target_compile_options(ww_stencils PRIVATE -O2 -fno-pic -fno-pie -ffunction-sections -fno-builtin -fno-jump-tables
                           -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-exceptions -fcf-protection=none
                           -fno-sanitize=all)
    set_target_properties(ww_stencils PROPERTIES POSITION_INDEPENDENT_CODE OFF)
  endif()
  add_executable(ww_stencil_tool tools/stencils/stencils.cpp)
  set(WW_STENCIL_TABLE "${CMAKE_BINARY_DIR}/native/stencils.h")
  add_custom_command(
    OUTPUT "${WW_STENCIL_TABLE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/native"
    COMMAND ww_stencil_tool "$<TARGET_OBJECTS:ww_stencils>" "${WW_STENCIL_TABLE}"
    DEPENDS ww_stencils ww_stencil_tool "$<TARGET_OBJECTS:ww_stencils>"
    COMMENT "Reading the native code's stencils"
    VERBATIM)
  target_sources(ww_core PRIVATE "${WW_STENCIL_TABLE}")
  target_include_directories(ww_core PRIVATE "${CMAKE_BINARY_DIR}/native")
  target_compile_definitions(ww_core PRIVATE WW_NATIVE=1)
  add_custom_target(ww_check_native ALL
    COMMAND $<TARGET_FILE:ww> --native-check
    DEPENDS ww
    COMMENT "Checking the native code"
    VERBATIM)
else()
  target_compile_definitions(ww_core PRIVATE WW_NATIVE=0)
endif()

# The instruction loop (VM::run) runs as fast whatever is linked before it: with Clang, the
# benchmarks' build, every jump target in vm.cpp starts on 32 bytes (and VM::run on 64, in
# vm.cpp). Without this, code growing elsewhere moved numeric by 14% (G42).
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set_source_files_properties(src/vm.cpp PROPERTIES COMPILE_OPTIONS "-mllvm;-align-all-nofallthru-blocks=5")
endif()
add_executable(ww src/main.cpp $<TARGET_OBJECTS:ww_core> $<TARGET_OBJECTS:ww_window> $<TARGET_OBJECTS:ww_compiler>)

# The proof that what a built program keeps needs nothing left out of it: the core and the
# window linked without the compiler side, and the core alone, the window's two entry points
# (which the core calls only when the window's section is there) standing in. Either fails to
# link if anything kept names what is left out. Neither is shipped.
add_executable(ww_proof src/proof_main.cpp $<TARGET_OBJECTS:ww_core> $<TARGET_OBJECTS:ww_window>)
add_executable(ww_proof_plain src/proof_main.cpp src/window_absent.cpp $<TARGET_OBJECTS:ww_core>)
foreach(proof ww_proof ww_proof_plain)
  set_target_properties(${proof} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/proof")
endforeach()
foreach(part ww ww_core ww_window ww_compiler ww_proof ww_proof_plain)
  target_compile_definitions(${part} PRIVATE WW_VERSION="${PROJECT_VERSION}")
endforeach()
if(WIN32)
  foreach(linked ww ww_proof ww_proof_plain)
    target_link_libraries(${linked} PRIVATE opengl32 ws2_32 bcrypt crypt32 shell32 ole32)
  endforeach()
  # __HrLoadAllImportsForDll, which the native layer calls whether or not the program
  # delay-loads OpenGL (only ww does, below).
  target_link_libraries(ww_proof PRIVATE delayimp)
endif()

# No copy of lib/ is put next to the executable (it was until 2026-11-18): ww carries lib/
# inside itself, and a lib folder beside it would be used in its place, so the suite would not
# test what a lone ww.exe does.

if(MSVC)
  foreach(part ww ww_core ww_window ww_compiler ww_proof ww_proof_plain)
    target_compile_options(${part} PRIVATE /W4 /utf-8 /permissive-)
    target_compile_definitions(${part} PRIVATE _CRT_SECURE_NO_WARNINGS)
    # Under AddressSanitizer, the Microsoft library marks a vector's room past its end as not to
    # be touched; native code's list add writes there and moves the end on (2026-11-15), which
    # the library's marks would not follow. (libstdc++ marks nothing unless asked.)
    target_compile_definitions(${part} PRIVATE _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
  endforeach()
  # The parser and compiler recurse over nested source; give them room.
  target_link_options(ww PRIVATE /STACK:16777216)
  # OpenGL and the shell are loaded when first used, not when ww starts: loading
  # opengl32.dll costs about 57 ms, which every script paid, window or not. A delay-loaded DLL
  # whose functions take float arguments is loaded whole before any is called
  # (__HrLoadAllImportsForDll): lld-link's stub for a first call can lose the second float
  # argument (src/jade/platform_win.cpp, WinPlatform::start).
  target_link_libraries(ww PRIVATE delayimp)
  target_link_options(ww PRIVATE /DELAYLOAD:opengl32.dll /DELAYLOAD:shell32.dll /DELAYLOAD:ole32.dll)
else()
  foreach(part ww ww_core ww_window ww_compiler ww_proof ww_proof_plain)
    target_compile_options(${part} PRIVATE -Wall -Wextra)
  endforeach()
endif()

# The check of what a built program leaves out (D161): nothing of it runs as the program starts,
# and every function the proofs need is in ww.exe in a section a built program keeps.
if(MSVC)
  foreach(linked ww ww_proof ww_proof_plain)
    target_link_options(${linked} PRIVATE "/MAP:$<TARGET_FILE_DIR:${linked}>/${linked}.map")
  endforeach()
  add_custom_target(ww_check_sections ALL
    COMMAND $<TARGET_FILE:ww> --check-sections "$<TARGET_FILE_DIR:ww>/ww.map"
            "$<TARGET_FILE_DIR:ww_proof>/ww_proof.map" "$<TARGET_FILE_DIR:ww_proof_plain>/ww_proof_plain.map"
    DEPENDS ww ww_proof ww_proof_plain
    COMMENT "Checking what a built program leaves out"
    VERBATIM)
endif()

# The standard library, compiled once as the build's last step and attached to ww.exe, so
# that no script compiles it or checks it again (D145). Off, ww compiles it at each start.
option(WW_ATTACH_PRELUDE "Attach the compiled standard library to ww.exe" ON)
if(WW_ATTACH_PRELUDE)
  add_custom_command(TARGET ww POST_BUILD
    COMMAND $<TARGET_FILE:ww> --attach-prelude $<TARGET_FILE_DIR:ww>/ww-attached.exe
    COMMAND ${CMAKE_COMMAND} -E rename $<TARGET_FILE_DIR:ww>/ww-attached.exe $<TARGET_FILE:ww>
    COMMENT "Attaching the compiled standard library"
    VERBATIM)
endif()
