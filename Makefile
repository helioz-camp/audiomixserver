CXXFLAGS = -Wall -ggdb -std=c++1y

all: audiomixserver
clean:
	rm -f audiomixserver *.o

.PHONY: all clean

sdl_cflags := $(shell pkg-config --cflags sdl2 SDL2_mixer)
sdl_libs := $(shell pkg-config --libs sdl2 SDL2_mixer)
override CXXFLAGS += $(sdl_cflags)
override LDFLAGS += $(sdl_libs) -lboost_program_options -levent -lGL -pthread

audiomixserver: audiomixserver.o
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)


