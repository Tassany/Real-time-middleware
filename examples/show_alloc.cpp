/*
 * show_alloc — inspects the core assignment produced by the plan's own
 * "allocation" block.
 *
 * JsonParser::parse() already calls allocator::apply_auto_allocation() for any
 * plan that leaves a subtask without "core", so parsing is enough: this tool
 * only reports what the configured strategy/sort_by/weight decided. It does not
 * override wcet_ns, unlike example_bin_packing, which injects fixed Heptane
 * values and therefore ignores the WCETs written in the plan.
 *
 * Usage: ./show_alloc <plans/deployment_plan.json>
 */
#include "deployment_plan.hpp"
#include "parser_json.hpp"
#include "allocator.hpp"

#include <algorithm>
#include <iostream>
#include <iomanip>
#include <map>

int main(int argc, char* argv[]) {
    const std::string file = (argc > 1) ? argv[1] : "plans/deployment_plan.json";

    DeploymentPlan plan;
    try {
        plan = JsonParser{}.parse(file);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    const auto& cfg = plan.allocation;
    std::cout << "plan      : " << file << "\n"
              << "strategy  : " << cfg.strategy << "\n"
              << "sort_by   : " << cfg.sort_by << "\n"
              << "weight    : " << cfg.weight << "\n"
              << "num_cores : " << cfg.num_cores << "\n"
              << "capacity  : " << cfg.capacity << "\n\n";

    std::cout << std::left
              << std::setw(5) << "ID" << std::setw(14) << "Type"
              << std::setw(11) << "Period(ms)" << std::setw(11) << "WCET(us)"
              << std::setw(10) << "Util" << "Core\n"
              << std::string(56, '-') << "\n";

    std::map<int, double> util_per_core;
    double total = 0.0;

    for (const auto& t : plan.tasks)
        for (const auto& s : t.subtasks) {
            const double u = allocator::utilization(s);
            util_per_core[s.core] += u;
            total += u;
            std::cout << std::left
                      << std::setw(5)  << s.id
                      << std::setw(14) << s.component_type
                      << std::setw(11) << std::fixed << std::setprecision(2) << s.period_ns / 1e6
                      << std::setw(11) << std::fixed << std::setprecision(2) << s.wcet_ns / 1e3
                      << std::setw(10) << std::fixed << std::setprecision(5) << u
                      << s.core << "\n";
        }

    std::cout << "\nload per core (utilization):\n";
    // Bar is scaled against a full core (u = 1.0) and clamped, so an overloaded
    // core saturates the bar instead of underflowing the padding count.
    constexpr int BAR_WIDTH = 40;
    for (const auto& [core, u] : util_per_core) {
        const int bar = std::clamp(static_cast<int>(u * BAR_WIDTH), 0, BAR_WIDTH);
        std::cout << "  core " << core << "  ["
                  << std::string(bar, '#') << std::string(BAR_WIDTH - bar, '.')
                  << "]  " << std::fixed << std::setprecision(5) << u << "\n";
    }
    std::cout << "\ntotal utilization: " << std::fixed << std::setprecision(5) << total << "\n";
    return 0;
}
