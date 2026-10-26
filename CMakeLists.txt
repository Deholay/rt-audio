cmake_minimum_required(VERSION 3.20)
project(rt_audio VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
option(RT_AUDIO_BUILD_LIVE "Build Project 2 microphone/speaker application" OFF)
if(RT_AUDIO_BUILD_LIVE)
  set(RT_AUDIO_PROJECT2_WITH_PORTAUDIO ON)
endif()
add_subdirectory(components/project2)

add_executable(project2_sim apps/project2_sim.cpp)
target_link_libraries(project2_sim PRIVATE rt_audio::project2)
target_compile_options(project2_sim PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(BUILD_TESTING)
  add_test(NAME project2_sim COMMAND project2_sim)
  set_tests_properties(project2_sim PROPERTIES TIMEOUT 10)
endif()

if(RT_AUDIO_BUILD_LIVE)
  add_executable(project2_live apps/project2_live.cpp)
  target_link_libraries(project2_live PRIVATE rt_audio::project2_portaudio)
  target_compile_options(project2_live PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
