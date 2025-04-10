CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O3 -march=native -flto -DNDEBUG 
#-g -fsanitize=address,undefined
LDFLAGS = -llzma -lre2 -pthread  -flto=auto -lboost_regex
LDCHERI = -L/home/charles-williams/Documents/CHERI/CheriTrace/cheri-compressed-cap/build -lcheri_compressed_cap14

TARGET = cheriQEMU_converter
SRC = cheriQEMU_converter.cc

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
