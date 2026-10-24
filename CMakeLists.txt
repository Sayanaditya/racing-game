cmake_minimum_required(VERSION 3.16)
project(ArgonRacer VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia)

set(CORE_SOURCES
    framebuffer.cpp
    spanfill_neon.cpp
    spanfill_avx2.cpp
    track.cpp
    roadrender.cpp
    sprites.cpp
    cockpit.cpp
    vehicle.cpp
    collision.cpp
    audio.cpp
    jobs.cpp
    highscores.cpp
    raster3d.cpp
    game.cpp
    gamewidget.cpp
    tests.cpp
    main.cpp
)

add_executable(racer ${CORE_SOURCES})

target_include_directories(racer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(racer PRIVATE Qt6::Widgets Qt6::Multimedia)

if(MSVC)
    target_compile_options(racer PRIVATE /O2 /fp:precise /W4)
else()
    target_compile_options(racer PRIVATE -O2 -fno-math-errno -Wall -Wextra)
endif()
