#include "deployment_plan.hpp"
#include "parser_json.hpp"
#include "allocator.hpp"

#include <iostream>
#include <iomanip>
#include <map>
#include <string>
#include <vector>

// WCET values from Heptane static analysis (MIPS, 100 MHz → 10 ns/cycle).
// Source: docs/wcet_heptane.md
static const std::map<std::string, uint64_t> WCET_BY_TYPE = {
    { "source",       240840 },   // 24084 cycles * 10 ns
    { "intermediate", 354240 },   // 35424 cycles * 10 ns
    { "sink",         352160 },   // 35216 cycles * 10 ns
};

static void inject_wcet(std::vector<SubtaskInfo>& subtasks) {
    for (auto& s : subtasks) {
        auto it = WCET_BY_TYPE.find(s.component_type);
        if (it != WCET_BY_TYPE.end())
            s.wcet_ns = it->second;
    }
}

static std::vector<SubtaskInfo> flatten(const DeploymentPlan& plan) {
    std::vector<SubtaskInfo> all;
    for (const auto& task : plan.tasks)
        for (const auto& st : task.subtasks)
            all.push_back(st);
    return all;
}

static void print_result(const std::string& label,
                         const AllocationResult& r,
                         int num_cores) {
    std::cout << "\n=== " << label << " ===\n";
    std::cout << std::left
              << std::setw(6)  << "ID"
              << std::setw(14) << "Type"
              << std::setw(10) << "Period(ms)"
              << std::setw(11) << "WCET(µs)"
              << std::setw(8)  << "Util"
              << std::setw(6)  << "Core"
              << "\n";
    std::cout << std::string(55, '-') << "\n";

    for (const auto& s : r.subtasks) {
        double u = allocator::utilization(s);
        std::cout << std::left
                  << std::setw(6)  << s.id
                  << std::setw(14) << s.component_type
                  << std::setw(10) << std::fixed << std::setprecision(1)
                                   << s.period_ns / 1e6
                  << std::setw(11) << std::fixed << std::setprecision(1)
                                   << s.wcet_ns / 1e3
                  << std::setw(8)  << std::fixed << std::setprecision(4) << u
                  << std::setw(6)  << (s.core >= 0 ? std::to_string(s.core) : "FAIL")
                  << "\n";
    }

    std::cout << "\nUtilization per core:\n";
    for (int c = 0; c < num_cores; ++c) {
        double u = r.core_util[c];
        int bar = static_cast<int>(u * 40);
        std::cout << "  Core " << c << "  ["
                  << std::string(bar, '#')
                  << std::string(40 - bar, '.')
                  << "] "
                  << std::fixed << std::setprecision(4) << u << "\n";
    }

    std::cout << (r.feasible ? "  FEASIBLE\n" : "  INFEASIBLE — subtask(s) not placed\n");
}

int main(int argc, char* argv[]) {
    std::string plan_file = (argc > 1) ? argv[1] : "deployment_plan.json";
    int num_cores         = (argc > 2) ? std::stoi(argv[2]) : 3;

    DeploymentPlan plan;
    try {
        plan = JsonParser{}.parse(plan_file);
    } catch (const std::exception& e) {
        std::cerr << "Error loading plan: " << e.what() << "\n";
        return 1;
    }

    auto subtasks = flatten(plan);
    inject_wcet(subtasks);

    std::cout << "Deployment plan: " << plan_file
              << " | subtasks: " << subtasks.size()
              << " | cores: " << num_cores << "\n";

    double total_util = 0.0;
    for (const auto& s : subtasks) total_util += allocator::utilization(s);
    std::cout << std::fixed << std::setprecision(4)
              << "Total utilization: " << total_util
              << " | capacity: " << num_cores << ".0000\n";

    print_result("First Fit",  allocator::first_fit(subtasks,  num_cores), num_cores);
    print_result("Best Fit",   allocator::best_fit(subtasks,   num_cores), num_cores);
    print_result("Worst Fit",  allocator::worst_fit(subtasks,  num_cores), num_cores);

    return 0;
}
