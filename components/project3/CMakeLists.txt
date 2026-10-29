cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(rt_audio_project3 VERSION 0.1.0 LANGUAGES CXX)
  include(CTest)
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  message(FATAL_ERROR "Project 3 currently targets GCC/Clang with native lock-free 64-bit atomics.")
endif()

find_package(Threads REQUIRED)
add_library(rt_audio_project3 STATIC src/engine.cpp src/processor.cpp src/worker.cpp)
add_library(rt_audio::project3 ALIAS rt_audio_project3)
target_include_directories(rt_audio_project3 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(rt_audio_project3 PUBLIC cxx_std_20)
target_link_libraries(rt_audio_project3 PUBLIC Threads::Threads)
target_compile_options(rt_audio_project3 PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(BUILD_TESTING)
  add_executable(project3_tests tests/project3_tests.cpp)
  target_link_libraries(project3_tests PRIVATE rt_audio::project3)
  target_compile_options(project3_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME project3_tests COMMAND project3_tests)
  set_tests_properties(project3_tests PROPERTIES TIMEOUT 30)
endif()

option(RT_AUDIO_PROJECT3_WITH_PORTAUDIO "Build optional Project 3 PortAudio device component" OFF)
if(RT_AUDIO_PROJECT3_WITH_PORTAUDIO)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PROJECT3_PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)
  add_library(rt_audio_project3_portaudio STATIC backends/portaudio/portaudio_device.cpp)
  add_library(rt_audio::project3_portaudio ALIAS rt_audio_project3_portaudio)
  target_link_libraries(rt_audio_project3_portaudio
    PUBLIC rt_audio::project3 PRIVATE PkgConfig::PROJECT3_PORTAUDIO)
  if(APPLE)
    target_link_libraries(rt_audio_project3_portaudio PRIVATE
      "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit"
      "-framework CoreFoundation" "-framework CoreServices")
  elseif(UNIX)
    target_link_libraries(rt_audio_project3_portaudio PRIVATE ${PROJECT3_PORTAUDIO_STATIC_LIBRARIES})
  endif()
  target_compile_options(rt_audio_project3_portaudio PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
