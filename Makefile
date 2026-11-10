APP      := kitchen
SRC_DIR  := src
OBJ_DIR  := build/obj
DIST_DIR := dist
TARGET   := $(DIST_DIR)/$(APP)

CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -MMD -MP
LDLIBS   := -lz

SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(SRCS))

.PHONY: all run clean rebuild
all: $(TARGET)

$(TARGET): $(OBJS) | $(DIST_DIR)
	$(CXX) $(OBJS) -o $@ $(LDLIBS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR) $(DIST_DIR):
	mkdir -p $@

run: all
	./$(TARGET)
clean:
	rm -rf build dist
rebuild: clean all
-include $(OBJS:.o=.d)
