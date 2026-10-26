cmake_minimum_required(VERSION 3.20)
project(rt_audio_project2 VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  message(FATAL_ERROR "The reused SPSC header requires GCC or Clang atomic builtins.")
endif()

set(QUANT_ARCHITECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../quant-architect"
    CACHE PATH "Existing quant-architect checkout (header only)")
if(NOT EXISTS "${QUANT_ARCHITECT_DIR}/include/quant/ipc/spsc_ring.hpp")
  message(FATAL_ERROR "Set -DQUANT_ARCHITECT_DIR=/path/to/quant-architect")
endif()

find_package(Threads REQUIRED)
add_library(rt_audio_project2 src/project2.cpp)
target_include_directories(rt_audio_project2 PUBLIC include "${QUANT_ARCHITECT_DIR}/include")
target_compile_features(rt_audio_project2 PUBLIC cxx_std_20)
target_link_libraries(rt_audio_project2 PUBLIC Threads::Threads)
target_compile_options(rt_audio_project2 PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(project2_sim apps/project2_sim.cpp)
target_link_libraries(project2_sim PRIVATE rt_audio_project2)
target_compile_options(project2_sim PRIVATE -Wall -Wextra -Wpedantic -Werror)

include(CTest)
if(BUILD_TESTING)
  add_executable(project2_tests tests/project2_tests.cpp)
  target_link_libraries(project2_tests PRIVATE rt_audio_project2)
  target_compile_options(project2_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME project2_tests COMMAND project2_tests)
  set_tests_properties(project2_tests PROPERTIES TIMEOUT 30)
  add_test(NAME project2_sim COMMAND project2_sim)
  set_tests_properties(project2_sim PROPERTIES TIMEOUT 10)
endif()

option(RT_AUDIO_BUILD_LIVE "Build microphone/speaker PortAudio adapter" OFF)
if(RT_AUDIO_BUILD_LIVE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)
  add_executable(project2_live apps/project2_live.cpp)
  target_link_libraries(project2_live PRIVATE rt_audio_project2 PkgConfig::PORTAUDIO)
  # A locally bootstrapped static PortAudio needs its private host API libraries.
  if(APPLE)
    target_link_libraries(project2_live PRIVATE
      "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit"
      "-framework CoreFoundation" "-framework CoreServices")
  elseif(UNIX)
    target_link_libraries(project2_live PRIVATE ${PORTAUDIO_STATIC_LIBRARIES})
  endif()
  target_compile_options(project2_live PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
