CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0 2>/dev/null)

all: dist/kitchen gui

dist/kitchen: src/common.cpp src/apk.cpp src/main.cpp src/common.h src/apk.h
	@mkdir -p dist
	$(CXX) $(CXXFLAGS) -o $@ src/common.cpp src/apk.cpp src/main.cpp -lz

gui: dist/kitchen
	@if [ -z "$(GTK_LIBS)" ]; then echo "GTK3 dev files missing. Run: sudo apt install libgtk-3-dev pkg-config  then: make"; exit 1; fi
	$(CXX) $(CXXFLAGS) $(GTK_CFLAGS) -o dist/kitchen-gui src/gui_gtk.cpp $(GTK_LIBS) -pthread

clean:
	rm -rf dist build
.PHONY: all gui clean
