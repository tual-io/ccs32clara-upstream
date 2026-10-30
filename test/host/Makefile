# Host tests: code without hardware access, built and run on the PC.
#   make -C test/host

CXX      ?= g++
CXXFLAGS  = -std=c++11 -Wall -Wextra -Werror -O1 -g -I../../ccs -fsanitize=address,undefined -fno-sanitize-recover=all

all: run

v2gtp_test: v2gtp_test.cpp ../../ccs/v2gtp.cpp ../../ccs/v2gtp.h
	$(CXX) $(CXXFLAGS) -o $@ v2gtp_test.cpp ../../ccs/v2gtp.cpp

run: v2gtp_test
	./v2gtp_test

clean:
	rm -f v2gtp_test

.PHONY: all run clean
