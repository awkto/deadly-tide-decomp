cmake_minimum_required(VERSION 3.16)
project(deadlytide C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# SDL3 is the only dependency, built from source and linked statically so the
# same recipe works natively and (later) under Emscripten, which ships its own
# SDL3 port -- skip the fetch there.
if(EMSCRIPTEN)
  set(DT_SDL_TARGET SDL3::SDL3)
  find_package(SDL3 REQUIRED)
else()
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.30
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(SDL3)
  set(DT_SDL_TARGET SDL3::SDL3-static)
endif()

# Portable core: container reader + codec. No SDL, no threads.
add_library(dtcore STATIC src/l96.c src/cinepak.c)
target_include_directories(dtcore PUBLIC src)

# Platform layer (all SDL calls live in platform.c) + front end.
add_executable(deadlytide src/main.c src/platform.c)
target_link_libraries(deadlytide PRIVATE dtcore ${DT_SDL_TARGET})

if(MSVC)
  set(DT_WARN /W4)
else()
  set(DT_WARN -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()
target_compile_options(dtcore PRIVATE ${DT_WARN})
target_compile_options(deadlytide PRIVATE ${DT_WARN})
