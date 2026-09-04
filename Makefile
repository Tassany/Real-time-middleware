CXX      = g++
CC       = gcc
CXXFLAGS = -std=c++17 -Wall -Iinclude -Isrc

EXAMPLES_DIR = examples
SCRIPTS_DIR  = scripts

# -----------------------------------------------------------------------
#  Mälardalen benchmarks as subtask bodies
#
#  BENCH_CFLAGS reproduces wcet_bench/cycle_counter/RELATORIO.md sec. 2. Every
#  wcet_ns in the deployment plans was measured with exactly these flags on a
#  Raspberry Pi 5; changing the -O level invalidates all of them silently.
#
#  -Dmain=bench_entry_<name> turns each standalone main() into a callable
#  function, and objcopy then demotes every other global symbol to local:
#  bsort100.c and matmult.c both define Seed and Initialize, so linking the six
#  objects together without this either fails or binds to the wrong one.
# -----------------------------------------------------------------------
BENCH_DIR    = wcet_bench
BENCH_OBJDIR = $(BENCH_DIR)/obj
BENCH_NAMES  = matmult bsort100 crc ud fft1 statemate
BENCH_OBJS   = $(addprefix $(BENCH_OBJDIR)/,$(addsuffix .o,$(BENCH_NAMES)))
BENCH_CFLAGS = -std=gnu89 -O0 -fno-builtin -fno-stack-protector

$(BENCH_OBJDIR):
	mkdir -p $@

$(BENCH_OBJDIR)/%.o: $(BENCH_DIR)/%.c | $(BENCH_OBJDIR)
	$(CC) $(BENCH_CFLAGS) -Dmain=bench_entry_$* -c $< -o $@
	objcopy --keep-global-symbol=bench_entry_$* $@

benchmarks: $(BENCH_OBJS)

# -----------------------------------------------------------------------
#  Main binary (main.cc)
# -----------------------------------------------------------------------
SRC_DIR = src

TARGET = main
SRCS   = main.cc $(SRC_DIR)/parser_json.cpp $(SRC_DIR)/dag.cpp
HDRS   = $(addprefix $(SRC_DIR)/,component.hpp adapter.hpp dag.hpp dispatcher.hpp \
         deployment_plan.hpp parser_json.hpp ring_buffer.hpp allocator.hpp)

$(TARGET): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

# -----------------------------------------------------------------------
#  Examples
#  -pthread : required for POSIX threads (affinity, scheduling, etc.)
#  Run example_dispatcher with sudo to enable SCHED_FIFO real-time priority
# -----------------------------------------------------------------------
example_ring: $(EXAMPLES_DIR)/example_ring.cpp $(SRC_DIR)/ring_buffer.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<

example_eventfd: $(EXAMPLES_DIR)/example_eventfd.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

example_two_threads: $(EXAMPLES_DIR)/example_two_threads.cpp $(SRC_DIR)/ring_buffer.hpp
	$(CXX) $(CXXFLAGS) -pthread -o $@ $<

example_dispatcher: $(EXAMPLES_DIR)/example_dispatcher.cpp $(SRC_DIR)/dispatcher.hpp $(SRC_DIR)/ring_buffer.hpp
	$(CXX) $(CXXFLAGS) -pthread -o $@ $<

example_epoll: $(EXAMPLES_DIR)/example_epoll.cpp
	$(CXX) $(CXXFLAGS) -pthread -o $@ $<

example_pipeline: $(EXAMPLES_DIR)/example_pipeline.cpp $(SRC_DIR)/dispatcher.hpp $(SRC_DIR)/ring_buffer.hpp
	$(CXX) $(CXXFLAGS) -pthread -o $@ $<

example_team_manager: $(EXAMPLES_DIR)/example_team_manager.cpp $(SRC_DIR)/team_manager.cpp $(SRC_DIR)/team_manager.hpp $(SRC_DIR)/dag.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(EXAMPLES_DIR)/example_team_manager.cpp $(SRC_DIR)/team_manager.cpp $(SRC_DIR)/dag.cpp

example_full_pipeline: $(EXAMPLES_DIR)/example_full_pipeline.cpp $(SRC_DIR)/team_manager.cpp $(SRC_DIR)/team_manager.hpp $(SRC_DIR)/dag.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(EXAMPLES_DIR)/example_full_pipeline.cpp $(SRC_DIR)/team_manager.cpp $(SRC_DIR)/dag.cpp

execute_plan: $(SCRIPTS_DIR)/execute_plan.cpp $(SRC_DIR)/team_manager.cpp $(SRC_DIR)/team_manager.hpp $(SRC_DIR)/dag.cpp $(SRC_DIR)/parser_json.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(SCRIPTS_DIR)/execute_plan.cpp $(SRC_DIR)/team_manager.cpp $(SRC_DIR)/dag.cpp $(SRC_DIR)/parser_json.cpp

evaluation: $(SCRIPTS_DIR)/evaluation.cpp $(SRC_DIR)/team_manager.cpp $(SRC_DIR)/team_manager.hpp $(SRC_DIR)/dag.cpp $(SRC_DIR)/parser_json.cpp $(SRC_DIR)/bench_registry.cpp $(SRC_DIR)/bench_registry.hpp $(BENCH_OBJS) $(HDRS)
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(SCRIPTS_DIR)/evaluation.cpp $(SRC_DIR)/team_manager.cpp $(SRC_DIR)/dag.cpp $(SRC_DIR)/parser_json.cpp $(SRC_DIR)/bench_registry.cpp $(BENCH_OBJS)

example_bin_packing: $(EXAMPLES_DIR)/example_bin_packing.cpp $(SRC_DIR)/parser_json.cpp $(SRC_DIR)/dag.cpp $(SRC_DIR)/allocator.hpp $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(EXAMPLES_DIR)/example_bin_packing.cpp $(SRC_DIR)/parser_json.cpp $(SRC_DIR)/dag.cpp

allocate: $(SCRIPTS_DIR)/allocate.cpp $(SRC_DIR)/parser_json.cpp $(SRC_DIR)/dag.cpp $(SRC_DIR)/allocator.hpp $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SCRIPTS_DIR)/allocate.cpp $(SRC_DIR)/parser_json.cpp $(SRC_DIR)/dag.cpp

example_saturation: $(EXAMPLES_DIR)/example_saturation.cpp $(SRC_DIR)/allocator.hpp $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(EXAMPLES_DIR)/example_saturation.cpp

example_heuristic_eval: $(EXAMPLES_DIR)/example_heuristic_eval.cpp $(SRC_DIR)/team_manager.cpp $(SRC_DIR)/team_manager.hpp $(SRC_DIR)/dag.cpp $(SRC_DIR)/allocator.hpp $(HDRS)
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(EXAMPLES_DIR)/example_heuristic_eval.cpp $(SRC_DIR)/team_manager.cpp $(SRC_DIR)/dag.cpp

example_eval_preemptive: $(EXAMPLES_DIR)/example_eval_preemptive.cpp $(SRC_DIR)/preemptive_team_manager.cpp $(SRC_DIR)/preemptive_team_manager.hpp $(SRC_DIR)/preemptive_dispatcher.hpp $(SRC_DIR)/dag.cpp $(SRC_DIR)/parser_json.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(EXAMPLES_DIR)/example_eval_preemptive.cpp $(SRC_DIR)/preemptive_team_manager.cpp $(SRC_DIR)/dag.cpp $(SRC_DIR)/parser_json.cpp

example_overhead_pilot: $(EXAMPLES_DIR)/example_overhead_pilot.cpp $(SRC_DIR)/dispatcher.hpp
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(EXAMPLES_DIR)/example_overhead_pilot.cpp

examples: example_ring example_eventfd example_two_threads example_dispatcher example_epoll example_pipeline example_team_manager example_full_pipeline

# -----------------------------------------------------------------------
#  Tooling (scripts/): the programs the experiments are run with, as opposed
#  to the demos in examples/.
# -----------------------------------------------------------------------
harness: allocate execute_plan evaluation

# -----------------------------------------------------------------------
#  Phase 6 — Code generator
# -----------------------------------------------------------------------
TOOLS_DIR = tools

codegen: $(TOOLS_DIR)/codegen.cpp $(SRC_DIR)/dag.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(TOOLS_DIR)/codegen.cpp $(SRC_DIR)/dag.cpp

codegen_run: codegen
	./codegen plans/deployment_plan_6tasks.json

# -----------------------------------------------------------------------
#  Tests
# -----------------------------------------------------------------------
TESTS_DIR  = tests
TESTS_BINS = test_parser test_dag test_ringbuf test_component test_dispatcher test_team_manager

test_parser: $(TESTS_DIR)/test_parser.cpp $(SRC_DIR)/parser_json.cpp $(SRC_DIR)/dag.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(TESTS_DIR)/test_parser.cpp $(SRC_DIR)/parser_json.cpp $(SRC_DIR)/dag.cpp

test_dag: $(TESTS_DIR)/test_dag.cpp $(SRC_DIR)/dag.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(TESTS_DIR)/test_dag.cpp $(SRC_DIR)/dag.cpp

test_ringbuf: $(TESTS_DIR)/test_ringbuf.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(TESTS_DIR)/test_ringbuf.cpp

test_component: $(TESTS_DIR)/test_component.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(TESTS_DIR)/test_component.cpp

test_dispatcher: $(TESTS_DIR)/test_dispatcher.cpp $(SRC_DIR)/dag.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(TESTS_DIR)/test_dispatcher.cpp $(SRC_DIR)/dag.cpp

test_team_manager: $(TESTS_DIR)/test_team_manager.cpp $(SRC_DIR)/team_manager.cpp $(SRC_DIR)/dag.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(TESTS_DIR)/test_team_manager.cpp $(SRC_DIR)/team_manager.cpp $(SRC_DIR)/dag.cpp

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
	      execute_plan evaluation example_bin_packing \
	      example_saturation example_heuristic_eval allocate \
	      example_overhead_pilot codegen
	rm -f overhead_pilot_dispatch_vazio.csv overhead_pilot_aresta_intra.csv overhead_pilot_aresta_inter.csv
	rm -f generated/main_generated.cpp generated/Makefile generated/main_generated
	rm -rf $(BENCH_OBJDIR)

.PHONY: clean examples harness tests test codegen_run benchmarks
