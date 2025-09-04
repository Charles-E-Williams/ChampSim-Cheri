CXX = clang++
CXXFLAGS = -std=c++20 -Wall -Wextra -Werror=format -Wno-sign-compare -Wno-initializer-overrides -Wno-missing-field-initializers -Wno-unused-parameter -Wno-unused-function -O3 -g3 -march=native  -fsanitize=undefined
LDFLAGS = -flto=auto -lre2 -lboost_regex

SRC = riscv64qemu2champsim.cc 
TARGET = riscv64qemu2champsim  

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
