CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g -O2 -pipe

SRC := myshell.cpp
OBJ := $(SRC:.cpp=.o)
TARGET := myshell 

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(OBJ)

.PHONY: all clean run