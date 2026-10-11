# Prince of Persia (native Apple Silicon macOS port of SDLPoP)
#
# GPL-3.0-or-later (engine and app modifications); original game assets are
# separately copyrighted and are never bundled with this project.

cmake_minimum_required(VERSION 3.24)

project(PrinceOfPersia LANGUAGES C OBJC)

# ---------------------------------------------------------------------------
# Toolchain / platform
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Build architecture (arm64 only)")
set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "macOS deployment target")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies: fixed, hash-pinned SDL2 and SDL2_image (static)
# ---------------------------------------------------------------------------
include(FetchContent)

# SDL2 2.32.10, static, SDL2main disabled: this app has its own C main().
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
FetchContent_Declare(SDL2
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz
    URL_HASH SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# SDL2_image 2.8.8, static, STB backend only (PNG load/save), no other codecs,
# no vendored libpng/libjpeg/etc., no samples/tests/install.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_DEPS_SHARED OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_VENDORED OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_TESTS OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_BACKEND_STB ON CACHE BOOL "" FORCE)
set(SDL2IMAGE_BACKEND_IMAGEIO OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_PNG ON CACHE BOOL "" FORCE)
set(SDL2IMAGE_PNG_SAVE ON CACHE BOOL "" FORCE)
set(SDL2IMAGE_AVIF OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_BMP OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_GIF OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_JPG OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_JXL OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_LBM OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_PCX OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_PNM OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_QOI OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_SVG OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_TGA OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_TIF OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_WEBP OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_XCF OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_XPM OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_XV OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL2_image
    URL https://github.com/libsdl-org/SDL_image/releases/download/release-2.8.8/SDL2_image-2.8.8.tar.gz
    URL_HASH SHA256=2213b56fdaff2220d0e38c8e420cbe1a83c87374190cba8c70af2156097ce30a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(SDL2 SDL2_image)

# ---------------------------------------------------------------------------
# Engine core library
# ---------------------------------------------------------------------------
add_subdirectory(src)

# ---------------------------------------------------------------------------
# Application bundle
# ---------------------------------------------------------------------------
set(APP_NAME "Prince of Persia")
set(APP_EXECUTABLE_NAME "PrinceOfPersia")
set(APP_BUNDLE_ID "local.princeofpersia.metal")
set(APP_VERSION "1.0.0")
set(APP_BUNDLE_DIR "${CMAKE_CURRENT_BINARY_DIR}/${APP_NAME}.app")

add_executable(${APP_EXECUTABLE_NAME}
    src/main.c
    src/macos_app.m
)

set_target_properties(${APP_EXECUTABLE_NAME} PROPERTIES
    OUTPUT_NAME "${APP_EXECUTABLE_NAME}"
)

target_link_libraries(${APP_EXECUTABLE_NAME} PRIVATE
    pop_core
    SDL2::SDL2-static
    SDL2_image::SDL2_image-static
)

# System frameworks required by SDL's static library and SDL2_image.
target_link_libraries(${APP_EXECUTABLE_NAME} PRIVATE
    "-framework AppKit"
    "-framework Foundation"
    "-framework CoreAudio"
    "-framework CoreHaptics"
    "-framework IOKit"
    "-framework GameController"
    "-framework CoreGraphics"
    "-framework Metal"
    "-framework QuartzCore"
    "-framework AudioToolbox"
    "-framework CoreServices"
    "-framework CoreVideo"
    "-framework Carbon"
    "-framework AudioUnit"
)

set_source_files_properties(src/macos_app.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")

# Assemble the .app bundle by hand so the on-disk name ("Prince of Persia.app")
# and the executable name ("Contents/MacOS/PrinceOfPersia") are both exact.
# Only app-owned resources are packaged: Info.plist, the default configuration
# and license/help notices; no game artwork, DAT files or level fallback tree.
set(APP_CONTENTS_DIR "${APP_BUNDLE_DIR}/Contents")
set(APP_RESOURCES_DIR "${APP_CONTENTS_DIR}/Resources")
add_custom_command(TARGET ${APP_EXECUTABLE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${APP_BUNDLE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${APP_BUNDLE_DIR}/Contents/MacOS"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${APP_RESOURCES_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${APP_EXECUTABLE_NAME}>"
            "${APP_BUNDLE_DIR}/Contents/MacOS/${APP_EXECUTABLE_NAME}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in" "${APP_CONTENTS_DIR}/Info.plist"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/macos/SDLPoP.ini" "${APP_RESOURCES_DIR}/SDLPoP.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/COPYING" "${APP_RESOURCES_DIR}/COPYING.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/doc/Readme.txt" "${APP_RESOURCES_DIR}/Readme.txt"
    COMMENT "Packaging app-owned resources"
)

# Ad-hoc code sign so the app runs locally (Developer ID not required).
add_custom_command(TARGET ${APP_EXECUTABLE_NAME} POST_BUILD
    COMMAND /usr/bin/codesign --force --sign -
            "${APP_BUNDLE_DIR}"
    COMMENT "Ad-hoc signing app bundle"
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(BUILD_TESTING "Build the test suite" ON)
if(BUILD_TESTING)
    enable_testing()
endif()

# POP_TEST_ASSETS may be supplied at configure time to run asset-dependent
# integration tests against the consented installation; otherwise those cases
# are omitted with a visible configuration status.
set(POP_TEST_ASSETS "$ENV{POP_TEST_ASSETS}" CACHE PATH "Path to copied original DAT assets")

function(add_pop_test name source)
    add_executable(${name} ${source} src/macos_app.m)
    target_link_libraries(${name} PRIVATE
        pop_core
        SDL2::SDL2-static
        SDL2_image::SDL2_image-static
    )
    # macOS-specific frameworks, mirroring the app target.
    target_link_libraries(${name} PRIVATE
        "-framework AppKit"
        "-framework Foundation"
        "-framework CoreAudio"
        "-framework CoreHaptics"
        "-framework IOKit"
        "-framework GameController"
        "-framework CoreGraphics"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework AudioToolbox"
        "-framework CoreServices"
        "-framework CoreVideo"
        "-framework Carbon"
        "-framework AudioUnit"
    )
    set_source_files_properties(src/macos_app.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_pop_test(save_rejection_tests "tests/save_rejection_tests.c")
add_pop_test(save_persistence_tests "tests/save_persistence_tests.m")
add_pop_test(save_continuation_tests "tests/save_continuation_tests.c")
