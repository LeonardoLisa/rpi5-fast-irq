# Compiler settings
CXX := g++
# Fetch ROOT C++ flags automatically
CXXFLAGS := -Wall -Wextra -O3 -std=c++17 $(shell root-config --cflags)
# Fetch ROOT linker libraries automatically
LDFLAGS := -pthread $(shell root-config --glibs)

# Target executable name
TARGET := cps_root.x

# Source files
SRCS := cps_root.cpp RpiFastIrq.cpp

# Object files
OBJS := $(SRCS:.cpp=.o)

# Default rule
all: $(TARGET)

# Link the executable
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Compile source files into object files
%.o: %.cpp RpiFastIrq.hpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Clean rule
clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean