.RECIPEPREFIX = >
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -pedantic
SRC      := WaterMonitorCore.cpp test_water_monitor.cpp

test: $(SRC)
> $(CXX) $(CXXFLAGS) $(SRC) -o water_monitor_test
> ./water_monitor_test

test-flow: $(SRC)
> $(CXX) $(CXXFLAGS) -DENABLE_FLOW_SENSOR=1 $(SRC) -o water_monitor_test_flow
> ./water_monitor_test_flow

clean:
> rm -f water_monitor_test water_monitor_test_flow *.exe
.PHONY: test test-flow clean
