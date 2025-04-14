CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O3 -march=native -flto -DNDEBU # -g -fsanitize=address,undefined
LDFLAGS = -llzma -lre2 -pthread  -flto=auto -lre2 -lboost_regex
LDCHERI = -L/home/charles-williams/Documents/CHERI/CheriTrace/cheri-compressed-cap/build -lcheri_compressed_cap14

SRC ?= cheriQEMU_converter.cc
TARGET = $(basename $(notdir $(SRC)))

ifeq ($(SRC), cheriQEMU_converter.cc)
	LINK_EXTRA = $(LDCHERI)
endif

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS) $(LINK_EXTRA)

clean:
	rm -f $(TARGET)

.PHONY: all clean
