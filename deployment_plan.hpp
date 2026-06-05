#pragma once
#ifndef DEPLOYMENT_PLAN_HPP
#define DEPLOYMENT_PLAN_HPP

/**
 * @file deployment_plan.hpp
 * @brief Plain-old-data structs that mirror the deployment_plan.json schema.
 *
 * These structs are populated by JsonParser and consumed by the runtime to
 * instantiate Dispatchers, pin threads to cores, and wire up the component graph.
 */

#include <string>
#include <vector>


/** @brief Physical or virtual machine that hosts one or more subtasks. */
struct HostInfo {
    std::string name;    ///< Logical name used by SubtaskInfo::host to refer to this machine.
    std::string address; ///< IP address or hostname of the machine.
};


/** @brief Deployment descriptor for a single subtask (one DAG node). */
struct SubtaskInfo {
    std::string name;      ///< Unique name of the subtask within its task (e.g. "T1").
    std::string component; ///< Name of the Component class to instantiate.
    std::string host;      ///< Name of the HostInfo entry where this subtask runs.
    int         core;      ///< CPU core index for thread affinity (0-based).
    int         priority;  ///< POSIX real-time priority for SCHED_FIFO scheduling.
    long        period_ns; ///< If > 0, the subtask is periodic with this period in nanoseconds.
    long        deadline_ns; ///< Relative deadline in nanoseconds.
};

/** @brief Directed data-flow connection between two subtasks. */
struct ConnectionInfo {
    std::string upstream;   ///< Name of the producer subtask.
    std::string downstream; ///< Name of the consumer subtask.
};

/** @brief A named real-time task composed of an ordered set of subtasks. */
struct TaskInfo {
    std::string              name;     ///< Logical name of the task (e.g. "main_task").
    std::vector<SubtaskInfo> subtasks; ///< Subtasks belonging to this task.
};

/**
 * @brief Complete deployment description parsed from a JSON configuration file.
 *
 * Populated by JsonParser::parse() and passed to the middleware runtime to
 * create and configure all Dispatchers, threads, and component instances.
 */
struct DeploymentPlan {
    std::vector<HostInfo>       hosts;       ///< All machines in the deployment.
    std::vector<TaskInfo>       tasks;       ///< All real-time tasks to be deployed.
    std::vector<ConnectionInfo> connections; ///< Data-flow edges between subtasks.
};

#endif // DEPLOYMENT_PLAN_HPP