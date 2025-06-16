CXX = clang++
CXXFLAGS = -std=c++20 -Wall -Wextra -O3 -march=native -flto -DNDEBUG
LDFLAGS = -llzma -pthread -flto=auto -lre2 -lboost_regex

SRC = riscv64purecapQEMU2ChampSim.cc disas.cc
TARGET = riscv64purecapQEMU2ChampSim  

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
