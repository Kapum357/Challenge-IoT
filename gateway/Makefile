.RECIPEPREFIX = >
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic
LDLIBS   := -lmosquitto -lsqlite3 -lcurl -lpthread
SRC      := GatewayDaemon.cpp AiEngineOpenRouter.cpp ../src/WaterMonitorCore.cpp

sabana_gateway: $(SRC)
> $(CXX) $(CXXFLAGS) $(SRC) -o $@ $(LDLIBS)

clean:
> rm -f sabana_gateway
.PHONY: clean
