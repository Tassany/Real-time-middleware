CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Iinclude

EXAMPLES_DIR = examples

# -----------------------------------------------------------------------
#  Main binary (main.cc)
# -----------------------------------------------------------------------
TARGET = main
SRCS   = main.cc parser_json.cpp dag.cpp
HDRS   = component.hpp adapter.hpp dag.hpp dispatcher.hpp \
         deployment_plan.hpp parser_json.hpp ring_buffer.hpp

$(TARGET): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

# -----------------------------------------------------------------------
#  Examples
#  -I. : allows examples to include headers from the project root
#  -pthread : required for POSIX threads (affinity, scheduling, etc.)
#  Run example_dispatcher with sudo to enable SCHED_FIFO real-time priority
# -----------------------------------------------------------------------
example_ring: $(EXAMPLES_DIR)/example_ring.cpp ring_buffer.hpp
	$(CXX) $(CXXFLAGS) -I. -o $@ $<

example_eventfd: $(EXAMPLES_DIR)/example_eventfd.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

example_two_threads: $(EXAMPLES_DIR)/example_two_threads.cpp ring_buffer.hpp
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $<

example_dispatcher: $(EXAMPLES_DIR)/example_dispatcher.cpp dispatcher.hpp ring_buffer.hpp
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $<

example_epoll: $(EXAMPLES_DIR)/example_epoll.cpp
	$(CXX) $(CXXFLAGS) -pthread -o $@ $<

examples: example_ring example_eventfd example_two_threads example_dispatcher example_epoll

# -----------------------------------------------------------------------
#  Tests
# -----------------------------------------------------------------------
TESTS_DIR  = tests
TESTS_BINS = test_parser test_dag test_ringbuf test_component test_dispatcher test_team_manager

test_parser: $(TESTS_DIR)/test_parser.cpp parser_json.cpp dag.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -I. -o $@ $(TESTS_DIR)/test_parser.cpp parser_json.cpp dag.cpp

test_dag: $(TESTS_DIR)/test_dag.cpp dag.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -I. -o $@ $(TESTS_DIR)/test_dag.cpp dag.cpp

test_ringbuf: $(TESTS_DIR)/test_ringbuf.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $(TESTS_DIR)/test_ringbuf.cpp

test_component: $(TESTS_DIR)/test_component.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -I. -o $@ $(TESTS_DIR)/test_component.cpp

test_dispatcher: $(TESTS_DIR)/test_dispatcher.cpp dag.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $(TESTS_DIR)/test_dispatcher.cpp dag.cpp

test_team_manager: $(TESTS_DIR)/test_team_manager.cpp parser_json.cpp dag.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $(TESTS_DIR)/test_team_manager.cpp parser_json.cpp dag.cpp

test: test_parser test_dag
	@echo "--- Fase 0: Parser ---"
	@./test_parser
	@echo "--- Fase 1: DAG ---"
	@./test_dag

tests: $(TESTS_BINS)

clean:
	rm -f $(TARGET) $(TESTS_BINS) \
	      example_ring example_eventfd example_two_threads example_dispatcher example_epoll

.PHONY: clean examples tests test
