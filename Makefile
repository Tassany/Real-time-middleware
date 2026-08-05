CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Iinclude

EXAMPLES_DIR = examples

# -----------------------------------------------------------------------
#  Main binary (main.cc)
# -----------------------------------------------------------------------
TARGET = main
SRCS   = main.cc parser_json.cpp dag.cpp
HDRS   = component.hpp adapter.hpp dag.hpp dispatcher.hpp \
         deployment_plan.hpp parser_json.hpp ring_buffer.hpp allocator.hpp

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

example_pipeline: $(EXAMPLES_DIR)/example_pipeline.cpp dispatcher.hpp ring_buffer.hpp
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $<

example_team_manager: $(EXAMPLES_DIR)/example_team_manager.cpp team_manager.cpp team_manager.hpp dag.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $(EXAMPLES_DIR)/example_team_manager.cpp team_manager.cpp dag.cpp

example_full_pipeline: $(EXAMPLES_DIR)/example_full_pipeline.cpp team_manager.cpp team_manager.hpp dag.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $(EXAMPLES_DIR)/example_full_pipeline.cpp team_manager.cpp dag.cpp

example_from_plan: $(EXAMPLES_DIR)/example_from_plan.cpp team_manager.cpp team_manager.hpp dag.cpp parser_json.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $(EXAMPLES_DIR)/example_from_plan.cpp team_manager.cpp dag.cpp parser_json.cpp

example_eval: $(EXAMPLES_DIR)/example_eval.cpp team_manager.cpp team_manager.hpp dag.cpp parser_json.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $(EXAMPLES_DIR)/example_eval.cpp team_manager.cpp dag.cpp parser_json.cpp

example_bin_packing: $(EXAMPLES_DIR)/example_bin_packing.cpp parser_json.cpp dag.cpp allocator.hpp $(HDRS)
	$(CXX) $(CXXFLAGS) -I. -o $@ $(EXAMPLES_DIR)/example_bin_packing.cpp parser_json.cpp dag.cpp

show_alloc: $(EXAMPLES_DIR)/show_alloc.cpp parser_json.cpp dag.cpp allocator.hpp $(HDRS)
	$(CXX) $(CXXFLAGS) -I. -o $@ $(EXAMPLES_DIR)/show_alloc.cpp parser_json.cpp dag.cpp

example_saturation: $(EXAMPLES_DIR)/example_saturation.cpp allocator.hpp $(HDRS)
	$(CXX) $(CXXFLAGS) -I. -o $@ $(EXAMPLES_DIR)/example_saturation.cpp

example_heuristic_eval: $(EXAMPLES_DIR)/example_heuristic_eval.cpp team_manager.cpp team_manager.hpp dag.cpp allocator.hpp $(HDRS)
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $(EXAMPLES_DIR)/example_heuristic_eval.cpp team_manager.cpp dag.cpp

example_eval_preemptive: $(EXAMPLES_DIR)/example_eval_preemptive.cpp preemptive_team_manager.cpp preemptive_team_manager.hpp preemptive_dispatcher.hpp dag.cpp parser_json.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $(EXAMPLES_DIR)/example_eval_preemptive.cpp preemptive_team_manager.cpp dag.cpp parser_json.cpp

examples: example_ring example_eventfd example_two_threads example_dispatcher example_epoll example_pipeline example_team_manager example_full_pipeline example_from_plan example_eval

# -----------------------------------------------------------------------
#  Phase 6 — Code generator
# -----------------------------------------------------------------------
TOOLS_DIR = tools

codegen: $(TOOLS_DIR)/codegen.cpp dag.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -I. -o $@ $(TOOLS_DIR)/codegen.cpp dag.cpp

codegen_run: codegen
	./codegen deployment_plan_6tasks.json

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

test_team_manager: $(TESTS_DIR)/test_team_manager.cpp team_manager.cpp dag.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -I. -pthread -o $@ $(TESTS_DIR)/test_team_manager.cpp team_manager.cpp dag.cpp

test: test_parser test_dag test_ringbuf test_component test_dispatcher test_team_manager
	@echo "--- Fase 0: Parser ---"
	@./test_parser
	@echo "--- Fase 1: DAG ---"
	@./test_dag
	@echo "--- Fase 2: Ring Buffer ---"
	@./test_ringbuf
	@echo "--- Fase 3: Component Model ---"
	@./test_component
	@echo "--- Fase 4: Dispatcher ---"
	@./test_dispatcher
	@echo "--- Fase 5: Team Manager ---"
	@./test_team_manager

tests: $(TESTS_BINS)

clean:
	rm -f $(TARGET) $(TESTS_BINS) \
	      example_ring example_eventfd example_two_threads example_dispatcher \
	      example_epoll example_pipeline example_team_manager example_full_pipeline \
	      example_from_plan example_eval example_bin_packing \
	      example_saturation example_heuristic_eval show_alloc \
	      codegen
	rm -f generated/main_generated.cpp generated/Makefile generated/main_generated

.PHONY: clean examples tests test codegen_run
