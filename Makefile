CXXFLAGS = -Wall -ggdb -std=c++1z

all: audiomixserver
clean:
	rm -f audiomixserver *.o

.PHONY: all clean brew-install apt-install

pkgs = sdl2 SDL2_mixer glew assimp
pkg_cflags := $(shell pkg-config --cflags $(pkgs))
pkg_libs := $(shell pkg-config --libs $(pkgs))

override CXXFLAGS += $(pkg_cflags)
override LDFLAGS += $(pkg_libs) -lboost_program_options -levent -pthread

audiomixserver: audiomixserver.o
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)


# for Mac OS X
brew-install:
	for pkg in sdl2 sdl2_mixer boost libevent glew pkg-config; do \
		brew install $$pkg; \
	done

# for Ubuntu/Debian
apt-install:
	apt install libevent-dev libboost-program-options-dev libsdl2-mixer-dev libglew-dev libsdl2-dev
