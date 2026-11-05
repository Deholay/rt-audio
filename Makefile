CXX ?= c++
CC ?= cc
AR ?= ar
BUILD_DIR ?= build
COMPONENT_DIR := components/project2
CPPFLAGS += -I$(COMPONENT_DIR)/include
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -Werror -pthread
LDFLAGS += -pthread
PORTAUDIO_PREFIX ?= $(abspath build/portaudio)
PORTAUDIO_CFLAGS ?= $(shell PKG_CONFIG_PATH="$(PORTAUDIO_PREFIX)/lib/pkgconfig:$$PKG_CONFIG_PATH" pkg-config --cflags portaudio-2.0 2>/dev/null)
PORTAUDIO_LIBS ?= $(shell PKG_CONFIG_PATH="$(PORTAUDIO_PREFIX)/lib/pkgconfig:$$PKG_CONFIG_PATH" pkg-config --libs --static portaudio-2.0 2>/dev/null)
CORE_OBJECTS := $(addprefix $(BUILD_DIR)/project2/,engine.o processor.o worker.o)
CORE_LIBRARY := $(BUILD_DIR)/librt_audio_project2.a
DEVICE_OBJECT := $(BUILD_DIR)/project2/portaudio_device.o
DEVICE_LIBRARY := $(BUILD_DIR)/librt_audio_project2_portaudio.a

# Versioned AHS releases (release/ahs/). Vendored upstream files are -isystem so
# their warnings do not fail -Werror; the Speex prefix keeps C symbols per release.
AHS_RELEASE := ahs-v7-c0291dd1e
AHS_UPSTREAM := release/ahs/$(AHS_RELEASE)/upstream
AHS_FLAGS := -Irelease/ahs/include -isystem $(AHS_UPSTREAM)/src -isystem $(AHS_UPSTREAM)/apps \
  -isystem $(AHS_UPSTREAM)/third_party/speexdsp \
  -DOUTSIDE_SPEEX -DRANDOM_PREFIX=rt_audio_ahs_v7_c0291dd1e_speex -DFLOATING_POINT
AHS_OBJECTS := $(BUILD_DIR)/release/ahs/registry.o $(BUILD_DIR)/release/ahs/$(AHS_RELEASE)/processor.o \
  $(BUILD_DIR)/release/ahs/$(AHS_RELEASE)/resample.o \
  $(BUILD_DIR)/release/ahs/ahs-v7-split-c0291dd1e/split.o \
  $(BUILD_DIR)/release/ahs/ahs-v7-lowlat-c0291dd1e/lowlat.o \
  $(BUILD_DIR)/release/ahs/ahs-v7-lowlat-fir-c0291dd1e/fir.o
AHS_LIBRARY := $(BUILD_DIR)/librt_audio_ahs.a

.PHONY: all component test live
all: $(BUILD_DIR)/project2_sim $(BUILD_DIR)/project2_tests $(BUILD_DIR)/project3_sim $(BUILD_DIR)/project3_tests $(AHS_LIBRARY)
component: $(CORE_LIBRARY) $(BUILD_DIR)/librt_audio_project3.a $(AHS_LIBRARY)

$(BUILD_DIR)/project2/%.o: $(COMPONENT_DIR)/src/%.cpp
	@mkdir -p "$(dir $@)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(CORE_LIBRARY): $(CORE_OBJECTS)
	$(AR) rcs $@ $^

$(BUILD_DIR)/project2_sim: apps/project2_sim.cpp $(CORE_LIBRARY)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

$(BUILD_DIR)/project2_tests: $(COMPONENT_DIR)/tests/project2_tests.cpp $(CORE_LIBRARY)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

test: all
	./$(BUILD_DIR)/project2_tests
	./$(BUILD_DIR)/project2_sim
	./$(BUILD_DIR)/project3_tests
	./$(BUILD_DIR)/project3_sim

live: $(BUILD_DIR)/project2_live $(BUILD_DIR)/project3_live

$(DEVICE_OBJECT): $(COMPONENT_DIR)/backends/portaudio/portaudio_device.cpp
	@test -n "$(PORTAUDIO_LIBS)" || \
	  (echo 'PortAudio missing: run bash scripts/bootstrap_portaudio.sh, then make live.' >&2; exit 1)
	@mkdir -p "$(dir $@)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(PORTAUDIO_CFLAGS) -MMD -MP -c $< -o $@

$(DEVICE_LIBRARY): $(DEVICE_OBJECT)
	$(AR) rcs $@ $^

$(BUILD_DIR)/release/ahs/%.o: release/ahs/%.cpp
	@mkdir -p "$(dir $@)"
	$(CXX) $(CPPFLAGS) $(AHS_FLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/release/ahs/$(AHS_RELEASE)/resample.o: $(AHS_UPSTREAM)/third_party/speexdsp/resample.c
	@mkdir -p "$(dir $@)"
	$(CC) $(AHS_FLAGS) -O2 -g -c $< -o $@

$(AHS_LIBRARY): $(AHS_OBJECTS)
	$(AR) rcs $@ $^

$(BUILD_DIR)/project2_live: apps/project2_live.cpp $(DEVICE_LIBRARY) $(AHS_LIBRARY) $(CORE_LIBRARY)
	$(CXX) $(CPPFLAGS) -Irelease/ahs/include $(CXXFLAGS) $^ $(LDFLAGS) $(PORTAUDIO_LIBS) -o $@

-include $(CORE_OBJECTS:.o=.d) $(DEVICE_OBJECT:.o=.d) $(AHS_OBJECTS:.o=.d)

PROJECT3_DIR := components/project3
PROJECT3_OBJECTS := $(addprefix $(BUILD_DIR)/project3/,engine.o processor.o worker.o)
PROJECT3_LIBRARY := $(BUILD_DIR)/librt_audio_project3.a
PROJECT3_DEVICE := $(BUILD_DIR)/project3/portaudio_device.o
PROJECT3_DEVICE_LIBRARY := $(BUILD_DIR)/librt_audio_project3_portaudio.a

$(BUILD_DIR)/project3/%.o: $(PROJECT3_DIR)/src/%.cpp
	@mkdir -p "$(dir $@)"
	$(CXX) $(CPPFLAGS) -I$(PROJECT3_DIR)/include $(CXXFLAGS) -MMD -MP -c $< -o $@

$(PROJECT3_LIBRARY): $(PROJECT3_OBJECTS)
	$(AR) rcs $@ $^

$(BUILD_DIR)/project3_sim: apps/project3_sim.cpp $(PROJECT3_LIBRARY)
	$(CXX) $(CPPFLAGS) -I$(PROJECT3_DIR)/include $(CXXFLAGS) $^ $(LDFLAGS) -o $@

$(BUILD_DIR)/project3_tests: $(PROJECT3_DIR)/tests/project3_tests.cpp $(PROJECT3_LIBRARY)
	$(CXX) $(CPPFLAGS) -I$(PROJECT3_DIR)/include $(CXXFLAGS) $^ $(LDFLAGS) -o $@

$(PROJECT3_DEVICE): $(PROJECT3_DIR)/backends/portaudio/portaudio_device.cpp
	@test -n "$(PORTAUDIO_LIBS)" || \
	  (echo 'PortAudio missing: run bash scripts/bootstrap_portaudio.sh, then make live.' >&2; exit 1)
	@mkdir -p "$(dir $@)"
	$(CXX) $(CPPFLAGS) -I$(PROJECT3_DIR)/include $(CXXFLAGS) $(PORTAUDIO_CFLAGS) -MMD -MP -c $< -o $@

$(PROJECT3_DEVICE_LIBRARY): $(PROJECT3_DEVICE)
	$(AR) rcs $@ $^

$(BUILD_DIR)/project3_live: apps/project3_live.cpp $(PROJECT3_DEVICE_LIBRARY) $(PROJECT3_LIBRARY)
	$(CXX) $(CPPFLAGS) -I$(PROJECT3_DIR)/include $(CXXFLAGS) $^ $(LDFLAGS) $(PORTAUDIO_LIBS) -o $@

-include $(PROJECT3_OBJECTS:.o=.d) $(PROJECT3_DEVICE:.o=.d)
