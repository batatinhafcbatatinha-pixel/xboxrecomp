# Generic port (tools/xboxport): any original-Xbox game, with the toolkit's full runtime (kernel, NV2A and APU
# emulation, input) and no game-specific code. tools/xboxport/xboxport.py copies this folder into the port folder
# (<port>/project) and writes the translated code into src/recomp/gen.
cmake_minimum_required(VERSION 3.25)
project(xboxport_game LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XBOXRECOMP_DIR "" CACHE PATH "The xboxrecomp repository (the runtime)")
if(NOT EXISTS "${XBOXRECOMP_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "XBOXRECOMP_DIR must point at the xboxrecomp repository")
endif()
file(GLOB GAME_AOT CONFIGURE_DEPENDS "src/recomp/gen/*.c")
if(NOT GAME_AOT)
  message(FATAL_ERROR "No translated code in src/recomp/gen: run tools/xboxport/xboxport.py prepare first")
endif()

add_subdirectory("${XBOXRECOMP_DIR}" "${CMAKE_BINARY_DIR}/xboxrecomp")

add_library(game_aot STATIC ${GAME_AOT} src/integration.c src/x86_fallback.c)
target_include_directories(game_aot PRIVATE src/recomp src/recomp/gen
  "${XBOXRECOMP_DIR}/include" "${XBOXRECOMP_DIR}/src/kernel" "${XBOXRECOMP_DIR}/src")
target_compile_definitions(game_aot PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

# Untranslated code runs emulated on Unicorn (src/x86_fallback.c) when tools/xboxport fetched it.
set(XBOXPORT_UNICORN_DIR "" CACHE PATH "Unicorn (include/, lib/unicorn.lib, lib/unicorn.dll) for the fallback emulator")
if(XBOXPORT_UNICORN_DIR AND EXISTS "${XBOXPORT_UNICORN_DIR}/lib/unicorn.dll")
  target_compile_definitions(game_aot PRIVATE XBOXPORT_FALLBACK)
  target_include_directories(game_aot PRIVATE "${XBOXPORT_UNICORN_DIR}/include")
  set(XBOXPORT_UNICORN_DLL "${XBOXPORT_UNICORN_DIR}/lib/unicorn.dll")   # loaded at run time (x86_fallback.c)
endif()
if(MSVC)
  target_compile_options(game_aot PRIVATE /bigobj /GS- /O1 /w)
endif()

add_executable(game src/main.c)
target_include_directories(game PRIVATE src/recomp src/recomp/gen "${XBOXRECOMP_DIR}/include" "${XBOXRECOMP_DIR}/src")
target_link_libraries(game PRIVATE game_aot xboxrecomp d3d11 dxgi dxguid xinput winmm dbghelp)
if(XBOXPORT_UNICORN_DLL)
  add_custom_command(TARGET game POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${XBOXPORT_UNICORN_DLL}" "$<TARGET_FILE_DIR:game>")
endif()
if(MSVC)
  target_link_options(game PRIVATE /LARGEADDRESSAWARE)
endif()
set_target_properties(game PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
