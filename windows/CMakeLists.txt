# GTA IV for Windows: the Graine ReXGlue consumer (glue/) built as the desktop
# app (LibertyRecomp.exe and its DLLs). Build steps: windows/BUILD.md.
cmake_minimum_required(VERSION 3.25)
project(gta4-windows LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "windows/ builds the Windows app; see ps5/ for the PS5")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(FMT_USE_CONSTEVAL=0)

set(LIBERTY_RECOMP_TARGET_PLATFORM "desktop" CACHE STRING "" FORCE)
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../glue/rexglue-sdk-main" CACHE PATH "")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../glue" glue)
