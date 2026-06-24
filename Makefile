# Makefile for the Student Record Management System.
#
# Targets:
#   make            - build everything (server, client, tests)
#   make server     - build only server
#   make client     - build only client
#   make tests      - build tests
#   make test       - build and run tests
#   make run        - build and start the server
#   make clean      - remove build artifacts

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread
LDFLAGS  ?= -pthread

BIN_DIR  := bin
SERVER   := $(BIN_DIR)/student_server
CLIENT   := $(BIN_DIR)/student_client
TESTBIN  := $(BIN_DIR)/student_tests

SERVER_SRC := server/main.cpp
CLIENT_SRC := client/main.cpp
TEST_SRC   := tests/test_main.cpp

# Pick up every header for change detection.
HEADERS := $(wildcard common/*.h) $(wildcard server/*.h)

.PHONY: all server client tests test run clean dirs

all: server client tests

dirs:
	@mkdir -p $(BIN_DIR)

server: dirs $(SERVER)
client: dirs $(CLIENT)
tests:  dirs $(TESTBIN)

$(SERVER): $(SERVER_SRC) $(HEADERS) | dirs
	$(CXX) $(CXXFLAGS) $(SERVER_SRC) -o $@ $(LDFLAGS)

$(CLIENT): $(CLIENT_SRC) $(HEADERS) | dirs
	$(CXX) $(CXXFLAGS) $(CLIENT_SRC) -o $@ $(LDFLAGS)

$(TESTBIN): $(TEST_SRC) $(HEADERS) | dirs
	$(CXX) $(CXXFLAGS) $(TEST_SRC) -o $@ $(LDFLAGS)

test: $(TESTBIN)
	@./$(TESTBIN)

run: server
	./$(SERVER)

clean:
	rm -rf $(BIN_DIR)
