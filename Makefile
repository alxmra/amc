CXX      ?= g++
CXXFLAGS ?= -O2 -Wall
LIBS      = -lX11 -lXrandr -lXext

macros: macros.cpp
	$(CXX) $(CXXFLAGS) -o $@ macros.cpp $(LIBS)

clean:
	rm -f macros

.PHONY: clean
