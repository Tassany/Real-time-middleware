#pragma once

#include <map>
#include <string>
#include <functional>
#include <stdexcept>
#include <iostream>

#include "deployment_plan.hpp"
#include "host_manager.hpp"
#include "dag.hpp"

// ================================================================
//  Runtime
//
//  Reads a DeploymentPlan and automatically builds:
//    - One Demultiplexer per unique (core, priority) pair
//    - One SubtaskDescriptor per subtask entry in the plan
//    - Connections wired as specified in plan.connections
//
//  The execute function of each subtask is provided by the user
//  via register_component() — mapping a component name (string
//  from the JSON) to an actual std::function<void()> or a
//  ComponentBase*.
//
//  Typical usage:
//
//    Runtime rt("server");
//
//    rt.register_component("Component1", []() { ... });
//    rt.register_component("Component2", my_component_ptr);
//
//    DeploymentPlan plan = JsonParser().parse("deployment_plan.json");
//    rt.build(plan);
//
//    rt.start();
//    rt.trigger("T1");
//    ...
//    rt.stop();
// ================================================================

class Runtime {
public:

    explicit Runtime(const std::string& host_name)
        : host_name_(host_name), host_manager_(host_name) {}

    // -------------------------------------------------------
    //  register_component(name, fn)
    //  Register a plain execute function.
    // -------------------------------------------------------
    void register_component(const std::string&    component_name,
                            std::function<void()> fn)
    {
        component_registry_[component_name] = fn;
    }

    // -------------------------------------------------------
    //  register_component(name, component)
    //  Register a ComponentBase* — wraps execute() and wires
    //  has_pending_input() for the Demultiplexer Step 5 drain.
    // -------------------------------------------------------
    void register_component(const std::string& component_name,
                            ComponentBase*     component)
    {
        component_registry_[component_name] = [component]() { component->execute(); };
        component_objects_[component_name]  = component;
    }

    // -------------------------------------------------------
    //  register_adapter(upstream, downstream, transfer_fn)
    //  Called after upstream finishes, before downstream is notified.
    //  Use this to copy data between ComponentPorts or plain members.
    // -------------------------------------------------------
    void register_adapter(const std::string&    upstream_name,
                          const std::string&    downstream_name,
                          std::function<void()> transfer_fn)
    {
        adapter_registry_[upstream_name + "->" + downstream_name] = transfer_fn;
    }

    const DAG& dag() const { return dag_; }

    // -------------------------------------------------------
    //  build(plan)
    //  Constructs all middleware objects for this host from the plan.
    // -------------------------------------------------------
    void build(const DeploymentPlan& plan) {
        team_ = host_manager_.add_team(host_name_ + "_team");

        // Step 1-3: create one Demultiplexer per (core,priority) pair
        //           and one SubtaskDescriptor per subtask on this host.
        for (const auto& task : plan.tasks) {
            for (const auto& info : task.subtasks) {

                if (info.host != host_name_) continue;

                Demultiplexer* d = get_or_create_demultiplexer(info.core,
                                                                info.priority);

                auto it = component_registry_.find(info.component);
                if (it == component_registry_.end())
                    throw std::runtime_error(
                        "No component registered for: " + info.component);

                SubtaskDescriptor* desc =
                    team_->add_subtask(info.name, d, it->second);
                subtask_map_[info.name] = desc;

                // Activate release guard if the subtask is periodic.
                if (info.period_ns > 0)
                    team_->set_period(desc, info.period_ns);

                // Wire has_pending_input for the Step 5 drain (§V-C).
                // Only set when a ComponentBase* was registered so the
                // Demultiplexer can check the input_port ring buffer.
                auto comp_it = component_objects_.find(info.component);
                if (comp_it != component_objects_.end()) {
                    ComponentBase* comp = comp_it->second;
                    desc->has_pending_input =
                        [comp]() { return comp->has_pending_input(); };
                }

                // Build DAG node.
                int node_id = next_node_id_++;
                subtask_id_map_[info.name] = node_id;
                ComponentBase* comp = nullptr;
                if (auto ci = component_objects_.find(info.component);
                        ci != component_objects_.end())
                    comp = ci->second;
                dag_.addNode(node_id, comp);

                std::cout << "[Runtime] registered subtask '" << info.name
                          << "'  component=" << info.component
                          << "  core="       << info.core
                          << "  priority="   << info.priority << "\n";
            }
        }

        // Step 4: wire connections (DAG edges + optional data transfer).
        for (const auto& conn : plan.connections) {
            auto up = subtask_map_.find(conn.upstream);
            auto dn = subtask_map_.find(conn.downstream);

            // Skip cross-host connections (Phase 4 territory).
            if (up == subtask_map_.end() || dn == subtask_map_.end())
                continue;

            // DAG edge.
            auto up_id = subtask_id_map_.find(conn.upstream);
            auto dn_id = subtask_id_map_.find(conn.downstream);
            if (up_id != subtask_id_map_.end() && dn_id != subtask_id_map_.end())
                dag_.addEdge(up_id->second, dn_id->second);

            // Optional adapter/transfer function.
            std::function<void()> transfer_fn;
            auto adapter_it =
                adapter_registry_.find(conn.upstream + "->" + conn.downstream);
            if (adapter_it != adapter_registry_.end())
                transfer_fn = adapter_it->second;

            team_->add_connection(up->second, dn->second, transfer_fn);

            std::cout << "[Runtime] wired " << conn.upstream
                      << " → " << conn.downstream << "\n";
        }
    }

    void start()  { host_manager_.start(); }
    void stop()   { host_manager_.stop();  }

    void trigger(const std::string& subtask_name) {
        auto it = subtask_map_.find(subtask_name);
        if (it == subtask_map_.end())
            throw std::runtime_error("Unknown subtask: " + subtask_name);
        team_->trigger(it->second);
    }

private:

    Demultiplexer* get_or_create_demultiplexer(int core, int priority) {
        std::string key = std::to_string(core) + ":" + std::to_string(priority);
        auto it = demux_map_.find(key);
        if (it != demux_map_.end()) return it->second;

        Demultiplexer* d = team_->add_demultiplexer(core, priority);
        demux_map_[key] = d;
        return d;
    }

    std::string  host_name_;
    HostManager  host_manager_;
    TeamManager* team_ = nullptr;

    std::map<std::string, std::function<void()>> component_registry_;
    std::map<std::string, SubtaskDescriptor*>    subtask_map_;
    std::map<std::string, Demultiplexer*>        demux_map_;

    std::map<std::string, ComponentBase*>        component_objects_;
    std::map<std::string, std::function<void()>> adapter_registry_;
    std::map<std::string, int>                   subtask_id_map_;
    DAG                                          dag_;
    int                                          next_node_id_ = 0;
};
