cmake_minimum_required(VERSION 3.13)

# FreeWili 2 display apps for FW-MAME, built against wilibsp
# (wilibsp/docs/app-project-setup.md). WILIBSP_DIR defaults to a wilibsp
# checkout at the repo root; point it at another checkout with
# -DWILIBSP_DIR=<path> (it must have libs/onewili initialised).
set(WILIBSP_DIR "${CMAKE_CURRENT_LIST_DIR}/../wilibsp" CACHE PATH "wilibsp checkout")
if(NOT EXISTS "${WILIBSP_DIR}/bsp/CMakeLists.txt")
    message(FATAL_ERROR "no wilibsp at ${WILIBSP_DIR}: pass -DWILIBSP_DIR=<wilibsp checkout>")
endif()

# RP2350B board header. NEVER pass -DPICO_BOARD on the command line
# (wilibsp/AGENTS.md invariant 1).
set(PICO_BOARD freewili2 CACHE STRING "Board type")
list(APPEND PICO_BOARD_HEADER_DIRS "${WILIBSP_DIR}/bsp/boards")

# agentio (input injection + screen capture): `fw screenshot` / `fw press`.
option(FW2_AGENTIO "Build the agentio remote-control/capture harness" ON)

set(PICO_PIO_USB_PATH "${WILIBSP_DIR}/bsp/third_party/Pico-PIO-USB")
include(${WILIBSP_DIR}/pico_sdk_import.cmake)
project(fwmame_fw2 C CXX ASM)
set(CMAKE_C_STANDARD 11)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
pico_sdk_init()

add_subdirectory(${WILIBSP_DIR}/bsp wilibsp_bsp)
add_subdirectory(${WILIBSP_DIR}/libs/onewili/wilibsp onewili)

add_subdirectory(apps/fwmame)
