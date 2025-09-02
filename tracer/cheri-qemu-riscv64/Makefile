CXX = clang++
CXXFLAGS = -std=c++20 -O3 \
           -Wall -Wextra -Werror=format \
           -Wno-c99-extensions -Wno-sign-compare \
           -Wno-initializer-overrides -Wno-missing-field-initializers \
           -Wno-unused-parameter -Wno-unused-function -DNDEBUG

LDFLAGS = -flto 

SRC = riscv64purecapQEMU2ChampSim.cc disas.cc stream.cc
TARGET = riscv64purecapQEMU2ChampSim  

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
