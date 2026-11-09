CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -fno-plt -DNDEBUG -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
LDFLAGS  ?=
SRC      := $(wildcard src/*.cpp)
OBJ      := $(SRC:.cpp=.o)

minivectordb: $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.cpp src/simdb.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) minivectordb

.PHONY: clean
