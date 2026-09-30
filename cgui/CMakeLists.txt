cmake_minimum_required(VERSION 3.16)
project(cgui C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Freetype REQUIRED)

add_library(cgui STATIC
    src/render.c
    src/font.c
    src/fontdb.c
    src/theme.c
    src/ui.c
    src/textedit.c
    src/window.c
)
target_include_directories(cgui PUBLIC include PRIVATE src)
target_link_libraries(cgui PUBLIC Freetype::Freetype)

if(WIN32)
    target_sources(cgui PRIVATE src/platform_win32.c)
    target_link_libraries(cgui PUBLIC user32 gdi32)
    if(MSVC)
        target_compile_definitions(cgui PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    endif()
elseif(APPLE)
    enable_language(OBJC)
    target_sources(cgui PRIVATE src/platform_cocoa.m)
    target_link_libraries(cgui PUBLIC "-framework Cocoa" "-framework QuartzCore")
else()
    find_package(X11 REQUIRED)
    target_sources(cgui PRIVATE src/platform_x11.c)
    target_link_libraries(cgui PUBLIC X11::X11)
    if(X11_Xext_FOUND)
        target_compile_definitions(cgui PRIVATE CGUI_HAVE_XSHAPE)
        target_link_libraries(cgui PUBLIC X11::Xext)
    endif()
endif()

if(NOT MSVC)
    target_link_libraries(cgui PUBLIC m)
    target_compile_options(cgui PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_executable(preetum WIN32
    apps/preetum/main.c
    apps/preetum/logo.c
    apps/preetum/fonts.c
    apps/preetum/tool_compare.c
    apps/preetum/tool_glyphs.c
)
target_link_libraries(preetum PRIVATE cgui)
if(MSVC)
    # Keep a plain main() while using the GUI subsystem (no console window).
    target_link_options(preetum PRIVATE /ENTRY:mainCRTStartup)
endif()
