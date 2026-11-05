# Versioned AHS releases behind rt_audio::ahs (include <rt_audio/ahs.hpp>).
enable_language(C)

add_library(rt_audio_ahs STATIC registry.cpp)
add_library(rt_audio::ahs ALIAS rt_audio_ahs)
target_include_directories(rt_audio_ahs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rt_audio_ahs PUBLIC rt_audio::project2)
target_compile_options(rt_audio_ahs PRIVATE -Wall -Wextra -Wpedantic -Werror)

# One OBJECT library per release. Vendored files are SYSTEM includes so their
# warnings do not fail the build; the Speex prefix keeps C symbols per release.
set(release ahs-v7-c0291dd1e)
set(upstream "${CMAKE_CURRENT_SOURCE_DIR}/${release}/upstream")
# ahs-v7-split-c0291dd1e and ahs-v7-lowlat-c0291dd1e reuse this upstream, so
# they build with the same flags and link the same Speex object.
add_library(rt_audio_ahs_v7_c0291dd1e OBJECT
  ${release}/processor.cpp ahs-v7-split-c0291dd1e/split.cpp
  ahs-v7-lowlat-c0291dd1e/lowlat.cpp ahs-v7-lowlat-fir-c0291dd1e/fir.cpp
  "${upstream}/third_party/speexdsp/resample.c")
target_include_directories(rt_audio_ahs_v7_c0291dd1e SYSTEM PRIVATE
  "${upstream}/src" "${upstream}/apps" "${upstream}/third_party/speexdsp")
target_compile_definitions(rt_audio_ahs_v7_c0291dd1e PRIVATE
  OUTSIDE_SPEEX RANDOM_PREFIX=rt_audio_ahs_v7_c0291dd1e_speex FLOATING_POINT)
target_link_libraries(rt_audio_ahs_v7_c0291dd1e PRIVATE rt_audio::project2)
target_compile_options(rt_audio_ahs_v7_c0291dd1e PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic -Werror>)
target_sources(rt_audio_ahs PRIVATE $<TARGET_OBJECTS:rt_audio_ahs_v7_c0291dd1e>)
