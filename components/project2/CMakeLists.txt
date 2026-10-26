cmake_minimum_required(VERSION 3.20)

# Works both on its own and as add_subdirectory() in another application.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(rt_audio_project2 VERSION 0.1.0 LANGUAGES CXX)
  include(CTest)
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  message(FATAL_ERROR "The local SPSC implementation requires GCC/Clang atomic builtins.")
endif()

find_package(Threads REQUIRED)
add_library(rt_audio_project2 STATIC src/engine.cpp src/processor.cpp src/worker.cpp)
add_library(rt_audio::project2 ALIAS rt_audio_project2)
target_include_directories(rt_audio_project2 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(rt_audio_project2 PUBLIC cxx_std_20)
target_link_libraries(rt_audio_project2 PUBLIC Threads::Threads)
target_compile_options(rt_audio_project2 PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(BUILD_TESTING)
  add_executable(project2_tests tests/project2_tests.cpp)
  target_link_libraries(project2_tests PRIVATE rt_audio::project2)
  target_compile_options(project2_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME project2_tests COMMAND project2_tests)
  set_tests_properties(project2_tests PROPERTIES TIMEOUT 30)
endif()

option(RT_AUDIO_PROJECT2_WITH_PORTAUDIO "Build optional Project 2 PortAudio device component" OFF)
if(RT_AUDIO_PROJECT2_WITH_PORTAUDIO)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)
  add_library(rt_audio_project2_portaudio STATIC backends/portaudio/portaudio_device.cpp)
  add_library(rt_audio::project2_portaudio ALIAS rt_audio_project2_portaudio)
  target_link_libraries(rt_audio_project2_portaudio
    PUBLIC rt_audio::project2 PRIVATE PkgConfig::PORTAUDIO)
  # Local PortAudio may be static: include host API dependencies as well.
  if(APPLE)
    target_link_libraries(rt_audio_project2_portaudio PRIVATE
      "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit"
      "-framework CoreFoundation" "-framework CoreServices")
  elseif(UNIX)
    target_link_libraries(rt_audio_project2_portaudio PRIVATE ${PORTAUDIO_STATIC_LIBRARIES})
  endif()
  target_compile_options(rt_audio_project2_portaudio PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
