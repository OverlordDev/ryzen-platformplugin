# Host-side test of the actuator's safety logic (Actuator.cpp), no kernel and no SDK needed.
#   make -C kext/test        builds and runs 12 scenarios; non-zero exit on any failure
CXX      ?= c++
SRC      := ../Source
CXXFLAGS ?= -std=gnu++17 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter

acttest: $(SRC)/Actuator.cpp mock.cpp test_actuator.cpp $(SRC)/Actuator.hpp
	$(CXX) $(CXXFLAGS) -Istub -I$(SRC) -I../../shared $(SRC)/Actuator.cpp mock.cpp test_actuator.cpp -o $@

check: acttest
	./acttest

clean:
	rm -f acttest

.PHONY: check clean
.DEFAULT_GOAL := check
