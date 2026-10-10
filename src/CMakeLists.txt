# Engine source library, supplied to the native root build.
#
# This file only exposes the SDLPoP engine core as a static library. The old
# CREATE_BUNDLE / copy_directory(data) / bare -lSDL2 linkage recipe has been
# removed; bundle creation lives entirely in the root CMakeLists.txt.

set(POP_CORE_SOURCES
        common.h
        config.h
        data.c
        data.h
        proto.h
        types.h
        seg000.c
        seg001.c
        seg002.c
        seg003.c
        seg004.c
        seg005.c
        seg006.c
        seg007.c
        seg008.c
        seg009.c
        seqtbl.c
        options.c
        midi.c
        opl3.c
        opl3.h
        replay.c
        lighting.c
        screenshot.c
        menu.c
        stb_vorbis.c
)

add_library(pop_core STATIC ${POP_CORE_SOURCES})
target_include_directories(pop_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pop_core PRIVATE SDL2::SDL2-static SDL2_image::SDL2_image-static)
set_target_properties(pop_core PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
)
