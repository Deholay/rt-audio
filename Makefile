CXX ?= c++
QUANT_ARCHITECT_DIR ?= $(abspath ../../quant-architect)
BUILD_DIR ?= build
CPPFLAGS += -Iinclude -I"$(QUANT_ARCHITECT_DIR)/include"
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -Werror -pthread
LDFLAGS += -pthread
PORTAUDIO_PREFIX ?= $(abspath build/portaudio)
PORTAUDIO_CFLAGS ?= $(shell PKG_CONFIG_PATH="$(PORTAUDIO_PREFIX)/lib/pkgconfig:$$PKG_CONFIG_PATH" pkg-config --cflags portaudio-2.0 2>/dev/null)
PORTAUDIO_LIBS ?= $(shell PKG_CONFIG_PATH="$(PORTAUDIO_PREFIX)/lib/pkgconfig:$$PKG_CONFIG_PATH" pkg-config --libs --static portaudio-2.0 2>/dev/null)

.PHONY: all test live check-deps
all: $(BUILD_DIR)/project2_sim $(BUILD_DIR)/project2_tests

check-deps:
	@test -f "$(QUANT_ARCHITECT_DIR)/include/quant/ipc/spsc_ring.hpp" || \
	  (echo 'Set QUANT_ARCHITECT_DIR to the existing quant-architect checkout.' >&2; exit 1)

$(BUILD_DIR)/project2.o: src/project2.cpp include/rt_audio/project2.hpp $(QUANT_ARCHITECT_DIR)/include/quant/ipc/spsc_ring.hpp | check-deps
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/project2_sim: apps/project2_sim.cpp $(BUILD_DIR)/project2.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

$(BUILD_DIR)/project2_tests: tests/project2_tests.cpp $(BUILD_DIR)/project2.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

test: all
	./$(BUILD_DIR)/project2_tests
	./$(BUILD_DIR)/project2_sim

live: $(BUILD_DIR)/project2_live

$(BUILD_DIR)/project2_live: apps/project2_live.cpp $(BUILD_DIR)/project2.o
	@test -n "$(PORTAUDIO_LIBS)" || \
	  (echo 'PortAudio missing: run bash scripts/bootstrap_portaudio.sh, then make live.' >&2; exit 1)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(PORTAUDIO_CFLAGS) $^ $(LDFLAGS) $(PORTAUDIO_LIBS) -o $@

-include $(BUILD_DIR)/project2.d
