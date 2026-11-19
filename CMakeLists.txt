cmake_minimum_required(VERSION 3.25)

project(zero_bot VERSION 0.4.6 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Keep the build machine's directory names out of the binary (assert messages and debug info
# would otherwise carry the full source path).
add_compile_options("-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=.")

# ZB_DESKTOP=ON  -> PC simulator (SDL window + headless screenshots)
# ZB_DESKTOP=OFF -> CardputerZero (fbdev + evdev), cross-built against the BSP sysroot
option(ZB_DESKTOP "Build the desktop simulator instead of the device binary" ON)
# ZB_CLI_ONLY=ON -> terminal-only build of the agent (no screen code, no LVGL); quick to compile,
# used for testing and tuning the agent loop on a PC.
option(ZB_CLI_ONLY "Build only the command-line agent, without the on-screen app" OFF)
# ZB_EXPERIMENTS=ON -> the ZB_EXP_* measurement switches (src/agent/experiment.h: a trace of model
# calls and requests, probes) are compiled in. On by default in the cli build only, so the device
# and simulator binaries cannot carry them by accident.
if(ZB_CLI_ONLY)
    option(ZB_EXPERIMENTS "Compile in the ZB_EXP_* measurement switches" ON)
else()
    option(ZB_EXPERIMENTS "Compile in the ZB_EXP_* measurement switches" OFF)
endif()

if(NOT ZB_CLI_ONLY)
set(ZB_CACHE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.cache")
set(LVGL_DIR "${ZB_CACHE_DIR}/lvgl" CACHE PATH "LVGL v9.5 source checkout")
if(NOT EXISTS "${LVGL_DIR}/lvgl.h")
    message(STATUS "Fetching LVGL v9.5.0 into ${LVGL_DIR}")
    execute_process(
        COMMAND git clone --depth 1 -b v9.5.0 https://github.com/lvgl/lvgl.git "${LVGL_DIR}"
        RESULT_VARIABLE _lvgl_clone)
    if(NOT _lvgl_clone EQUAL 0)
        message(FATAL_ERROR "Could not fetch LVGL")
    endif()
endif()

# ---------------------------------------------------------------- LVGL
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/lv_conf.h" CACHE PATH "" FORCE)
add_subdirectory("${LVGL_DIR}" "${CMAKE_BINARY_DIR}/lvgl" EXCLUDE_FROM_ALL)
target_compile_options(lvgl PRIVATE -w)

if(ZB_DESKTOP)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ZB_DEPS REQUIRED IMPORTED_TARGET sdl2 freetype2 libpng libcurl libcrypto)
    target_link_libraries(lvgl PUBLIC PkgConfig::ZB_DEPS)
    target_compile_definitions(lvgl PUBLIC ZB_DESKTOP=1)
else()
    set(_sr "${CMAKE_SYSROOT}")
    set(_ma "aarch64-linux-gnu")
    foreach(_lib freetype png16 curl crypto z)
        find_library(ZB_LIB_${_lib} NAMES ${_lib}
            HINTS "${_sr}/usr/lib/${_ma}" "${_sr}/usr/lib" NO_DEFAULT_PATH REQUIRED)
        list(APPEND ZB_DEVICE_LIBS "${ZB_LIB_${_lib}}")
    endforeach()
    target_include_directories(lvgl SYSTEM PUBLIC
        "${_sr}/usr/include/freetype2"
        "${_sr}/usr/include/libpng16")
    target_link_libraries(lvgl PUBLIC ${ZB_DEVICE_LIBS} pthread m)
    target_compile_definitions(lvgl PUBLIC ZB_DESKTOP=0)
endif()

endif()

# ---------------------------------------------------------------- app
file(GLOB_RECURSE ZB_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
if(ZB_CLI_ONLY)
    list(FILTER ZB_SOURCES EXCLUDE REGEX "/src/ui/|/platform/display\\.cpp$|/platform/input\\.cpp$")
endif()

add_executable(zero_bot ${ZB_SOURCES})
target_include_directories(zero_bot PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
target_compile_options(zero_bot PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
target_compile_definitions(zero_bot PRIVATE ZB_VERSION="${PROJECT_VERSION}" ZB_EXPERIMENTS=$<BOOL:${ZB_EXPERIMENTS}>)
if(ZB_CLI_ONLY)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ZB_CLI_DEPS REQUIRED IMPORTED_TARGET libcurl libcrypto)
    target_compile_definitions(zero_bot PRIVATE ZB_CLI_ONLY=1 ZB_DESKTOP=1)
    target_link_libraries(zero_bot PRIVATE PkgConfig::ZB_CLI_DEPS pthread ${CMAKE_DL_LIBS})
else()
    target_compile_definitions(zero_bot PRIVATE ZB_CLI_ONLY=0)
    target_link_libraries(zero_bot PRIVATE lvgl pthread ${CMAKE_DL_LIBS})
endif()
if(NOT ZB_DESKTOP)
    # Run on the stock image without needing a newer libstdc++ than Debian trixie ships.
    target_link_options(zero_bot PRIVATE -static-libstdc++ -static-libgcc)
endif()

install(TARGETS zero_bot RUNTIME DESTINATION bin)
