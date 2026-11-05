# Versioned KCS releases behind rt_audio::kcs (include <rt_audio/kcs.hpp>), on Project 3.
# Pure C++ port; the Python reference under kcs-*/upstream/ is not compiled.
add_library(rt_audio_kcs STATIC
  registry.cpp
  kcs-v23-ce6237c12/pipeline.cpp
  kcs-v23-ce6237c12/processor.cpp)
add_library(rt_audio::kcs ALIAS rt_audio_kcs)
target_include_directories(rt_audio_kcs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rt_audio_kcs PUBLIC rt_audio::project3)
target_compile_options(rt_audio_kcs PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(kcs_v23_equiv kcs-v23-ce6237c12/equiv_main.cpp kcs-v23-ce6237c12/pipeline.cpp)
target_compile_features(kcs_v23_equiv PRIVATE cxx_std_20)
target_compile_options(kcs_v23_equiv PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(BUILD_TESTING)
  add_executable(kcs_tests tests/kcs_tests.cpp)
  target_link_libraries(kcs_tests PRIVATE rt_audio::kcs)
  target_compile_options(kcs_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME kcs_tests COMMAND kcs_tests)
  set_tests_properties(kcs_tests PROPERTIES TIMEOUT 60)
endif()
