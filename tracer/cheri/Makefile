CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O3 -march=native -flto -DNDEBUG  -DCHERI
LZMA_LIB = -llzma -lre2 -pthread
LZMA_LIB += -flto=auto

TARGET = cheriQEMU_converter
SRC = cheriQEMU_converter.cc

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LZMA_LIB)

clean:
	rm -f $(TARGET)

.PHONY: all clean
