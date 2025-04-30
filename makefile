all: compile

compile: main.cpp bus.hpp cache.hpp types.hpp
	g++ -std=c++17 -O2 -o L1simulate main.cpp

clean:
	rm *.log L1simulate