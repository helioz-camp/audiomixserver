CXXFLAGS = -Wall -ggdb -std=c++1z

all: audiomixserver
clean:
	rm -f audiomixserver *.o

.PHONY: all clean

pkg_cflags := $(shell pkg-config --cflags sdl2 SDL2_mixer glew)
pkg_libs := $(shell pkg-config --libs sdl2 SDL2_mixer glew)

override CXXFLAGS += $(pkg_cflags)
override LDFLAGS += $(pkg_libs) -lboost_program_options -levent -pthread

audiomixserver: audiomixserver.o
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)


