CXX = clang++
CXXFLAGS = -std=c++20 -O3 \
           -Wall -Wextra -Werror=format \
           -Wno-c99-extensions -Wno-sign-compare \
           -Wno-initializer-overrides -Wno-missing-field-initializers \
           -Wno-unused-parameter -Wno-unused-function -DNDEBUG

LDFLAGS = -flto 

SRC = qemu2ChampSim.cc disas.cc stream.cc
TARGET = qemu2ChampSim

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
