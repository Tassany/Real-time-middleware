CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Iinclude

EXAMPLES_DIR = examples

# -----------------------------------------------------------------------
#  Main binary (main.cc)
# -----------------------------------------------------------------------
TARGET = main
SRCS   = main.cc parser_json.cpp dag.cpp
HDRS   = component.hpp adapter.hpp dag.hpp runtime.hpp \
         host_manager.hpp team_manager.hpp dispatcher.hpp \
         deployment_plan.hpp parser_json.hpp ring_buffer.hpp

$(TARGET): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $(SRCS)

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

example_team_manager: $(EXAMPLES_DIR)/example_team_manager.cpp team_manager.hpp demultiplexer.hpp ring_buffer.hpp
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $<

example_host_manager: $(EXAMPLES_DIR)/example_host_manager.cpp host_manager.hpp team_manager.hpp demultiplexer.hpp ring_buffer.hpp
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $<

example_runtime: $(EXAMPLES_DIR)/example_runtime.cpp runtime.hpp host_manager.hpp team_manager.hpp demultiplexer.hpp ring_buffer.hpp parser_json.cpp dag.cpp dag.hpp
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $< parser_json.cpp dag.cpp

example_demux: $(EXAMPLES_DIR)/example_demux.cpp demultiplexer.hpp
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $<

example_dag_runtime: $(EXAMPLES_DIR)/example_dag_runtime.cpp runtime.hpp dag.hpp dag.cpp component.hpp adapter.hpp host_manager.hpp team_manager.hpp demultiplexer.hpp deployment_plan.hpp parser_json.cpp
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $< dag.cpp parser_json.cpp

examples: example_ring example_eventfd example_two_threads example_dispatcher example_epoll example_team_manager example_host_manager

# -----------------------------------------------------------------------
#  Phase 1 tests
#  Normal build:      make test_phase1
#  UB sanitizer:      make test_phase1_ubsan
#  Thread sanitizer:  make test_phase1_tsan  (requires single-threaded TSan)
# -----------------------------------------------------------------------
TEST_SRCS = tests/test_phase1.cpp parser_json.cpp
TEST_HDRS = ring_buffer.hpp demultiplexer.hpp parser_json.hpp deployment_plan.hpp

test_phase1: $(TEST_SRCS) $(TEST_HDRS)
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $(TEST_SRCS) && ./$@

test_phase1_ubsan: $(TEST_SRCS) $(TEST_HDRS)
	$(CXX) $(CXXFLAGS) -I. -pthread -fsanitize=undefined -fno-omit-frame-pointer -o $@ $(TEST_SRCS) && ./$@

test_phase1_tsan: $(TEST_SRCS) $(TEST_HDRS)
	$(CXX) $(CXXFLAGS) -I. -pthread -fsanitize=thread -fno-omit-frame-pointer -o $@ $(TEST_SRCS) && ./$@

# -----------------------------------------------------------------------
#  Phase 2 tests
# -----------------------------------------------------------------------
TEST2_SRCS = tests/test_phase2.cpp
TEST2_HDRS = component.hpp ring_buffer.hpp demultiplexer.hpp team_manager.hpp

test_phase2: $(TEST2_SRCS) $(TEST2_HDRS)
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $(TEST2_SRCS) && ./$@

test_phase2_ubsan: $(TEST2_SRCS) $(TEST2_HDRS)
	$(CXX) $(CXXFLAGS) -I. -pthread -fsanitize=undefined -fno-omit-frame-pointer -o $@ $(TEST2_SRCS) && ./$@

test_phase2_tsan: $(TEST2_SRCS) $(TEST2_HDRS)
	$(CXX) $(CXXFLAGS) -I. -pthread -fsanitize=thread -fno-omit-frame-pointer -o $@ $(TEST2_SRCS) && ./$@

clean:
	rm -f $(TARGET) example_ring example_eventfd example_two_threads example_dispatcher example_epoll example_team_manager example_host_manager test_phase1 test_phase1_ubsan test_phase1_tsan test_phase2 test_phase2_ubsan test_phase2_tsan

.PHONY: clean examples test_phase1 test_phase1_ubsan test_phase1_tsan test_phase2 test_phase2_ubsan test_phase2_tsan
