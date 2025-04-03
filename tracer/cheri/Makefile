CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O3 -march=native -flto -DNDEBUG  -DCHERI
LDFLAGS = -llzma -lre2 -pthread -L/home/grads/c/charlesw2000/QEMU-to-ChampSim/cheri-compressed-cap/build -lcheri_compressed_cap14 -flto=auto

TARGET = cheriQEMU_converter
SRC = cheriQEMU_converter.cc

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
