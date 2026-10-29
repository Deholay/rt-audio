CXX ?= c++
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

.PHONY: all component test live
all: $(BUILD_DIR)/project2_sim $(BUILD_DIR)/project2_tests $(BUILD_DIR)/project3_sim $(BUILD_DIR)/project3_tests
component: $(CORE_LIBRARY) $(BUILD_DIR)/librt_audio_project3.a

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

$(BUILD_DIR)/project2_live: apps/project2_live.cpp $(DEVICE_LIBRARY) $(CORE_LIBRARY)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDFLAGS) $(PORTAUDIO_LIBS) -o $@

-include $(CORE_OBJECTS:.o=.d) $(DEVICE_OBJECT:.o=.d)

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
