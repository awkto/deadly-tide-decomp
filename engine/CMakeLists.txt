cmake_minimum_required(VERSION 3.16)
project(deadlytide C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(DT_HEADLESS "Build without SDL: headless modes only (platform_null.c)" OFF)

# SDL3 is the only dependency, built from source and linked statically.
# Emscripten ships its own SDL3 port (--use-port=sdl3) -- skip the fetch there.
if(DT_HEADLESS)
  set(DT_SDL_TARGET "")
  set(DT_PLATFORM_SRC src/platform_null.c)
elseif(EMSCRIPTEN)
  set(DT_SDL_TARGET "")
  add_compile_options(--use-port=sdl3)
  add_link_options(--use-port=sdl3)
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
if(NOT DT_PLATFORM_SRC)
  set(DT_PLATFORM_SRC src/platform.c)
endif()

# Portable core: container reader + codec. No SDL, no threads.
add_library(dtcore STATIC src/l96.c src/cinepak.c src/scene.c src/sprites.c src/chain.c src/game.c src/bot.c src/freelook.c src/save.c)
target_include_directories(dtcore PUBLIC src)
if(NOT MSVC)
  target_link_libraries(dtcore PUBLIC m)
endif()

# Platform layer (all SDL calls live in platform.c) + front end.
add_executable(deadlytide src/main.c ${DT_PLATFORM_SRC})
target_link_libraries(deadlytide PRIVATE dtcore ${DT_SDL_TARGET})

if(MSVC)
  set(DT_WARN /W4)
else()
  set(DT_WARN -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()
target_compile_options(dtcore PRIVATE ${DT_WARN})
target_compile_options(deadlytide PRIVATE ${DT_WARN})
if(MSVC)   # fopen etc. are fine; the _s variants are not portable C11
  target_compile_definitions(dtcore PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_definitions(deadlytide PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(EMSCRIPTEN)
  # Browser build: deadlytide.js + deadlytide.wasm, driven by web/index.html. The user's own
  # .L96 files stay as JS File objects (lazy slice reads); JS calls dt_start() from the Play click.
  set_target_properties(deadlytide PROPERTIES SUFFIX ".js")
  target_link_options(deadlytide PRIVATE
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sMAXIMUM_MEMORY=2GB
    "-sEXPORTED_FUNCTIONS=_main,_dt_start,_dt_segment_count,_dt_frames,_malloc,_free"
    "-sEXPORTED_RUNTIME_METHODS=FS,HEAPU8,ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8"
    -sFORCE_FILESYSTEM=1 -sENVIRONMENT=web -sASYNCIFY=1 -sASYNCIFY_STACK_SIZE=65536 -sSTACK_SIZE=1MB -O2)
  add_custom_command(TARGET deadlytide POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/web $<TARGET_FILE_DIR:deadlytide>)
endif()
