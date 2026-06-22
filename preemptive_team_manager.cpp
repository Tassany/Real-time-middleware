#include "preemptive_team_manager.hpp"

PreemptiveTeamManager::PreemptiveTeamManager() : state_val_(State::CREATED) {}

PreemptiveTeamManager::~PreemptiveTeamManager() { stop(); }

// -----------------------------------------------------------------------

void PreemptiveTeamManager::initialize(const std::vector<SubtaskEntry>& entries,
                                        const DAG& dag) {
    std::lock_guard<std::mutex> lk(state_);
    if (state_val_ != State::CREATED)
        throw std::runtime_error("PreemptiveTeamManager::initialize: already called");

    // Build ID → subtask / info maps
    std::map<int, const SubtaskInfo*> info_map;
    for (const auto& e : entries) {
        subtasks_[e.info.id] = e.subtask;
        info_map[e.info.id]  = &e.info;
    }

    // Topological order from DAG (throws if there is a cycle)
    topo_order_ = dag.topological_sort();

    // Validate: every DAG node must have a corresponding SubtaskEntry
    for (int id : topo_order_) {
        if (info_map.find(id) == info_map.end())
            throw std::runtime_error(
                "PreemptiveTeamManager::initialize: no SubtaskEntry for DAG node "
                + std::to_string(id));
    }

    // --- One PreemptiveDispatcher per subtask (no grouping by priority) ---
    // This is the key difference from TeamManager: every subtask gets its own
    // thread at its own SCHED_FIFO priority, enabling OS-level preemption.
    for (int id : topo_order_) {
        const SubtaskInfo* info = info_map.at(id);
        dispatchers_[id] = std::make_unique<PreemptiveDispatcher>(
            subtasks_.at(id), info->core, info->priority);
        dispatcher_order_.push_back(id);
        subtask_dispatcher_[id] = dispatchers_.at(id).get();
    }

    // --- Configure each subtask and wire connections ---
    for (const auto& node : dag.nodes()) {
        int id = node.id;
        if (subtasks_.find(id) == subtasks_.end()) continue;

        Subtask*           s    = subtasks_.at(id);
        const SubtaskInfo* info = info_map.at(id);

        s->period_ns = info->period_ns;

        s->fan_in_total = static_cast<int>(node.predecessors.size());
        if (s->fan_in_total < 1) s->fan_in_total = 1;
        s->fan_in_received.store(0);

        s->downstream.clear();
        for (int succ_id : node.successors)
            s->downstream.push_back({subtask_dispatcher_.at(succ_id),
                                     subtasks_.at(succ_id)});

        auto original_fn = s->execute;
        s->execute = [this, id, original_fn]() {
            try {
                original_fn();
            } catch (const std::exception& e) {
                std::cerr << "[PreemptiveTeamManager] subtask " << id
                          << " threw: " << e.what() << "\n";
                on_subtask_exception(id);
            } catch (...) {
                std::cerr << "[PreemptiveTeamManager] subtask " << id
                          << " threw unknown exception\n";
                on_subtask_exception(id);
            }
        };
    }

    // --- Ring buffer sizing (same formula as TeamManager) ---
    int depth = dag.pipeline_depth();

    for (const auto& node : dag.nodes()) {
        int up_id = node.id;
        if (info_map.find(up_id) == info_map.end()) continue;
        const SubtaskInfo* up_info = info_map.at(up_id);

        for (int down_id : node.successors) {
            if (info_map.find(down_id) == info_map.end()) continue;
            const SubtaskInfo* down_info = info_map.at(down_id);

            ring_buffer_sizes_[{up_id, down_id}] = ring_buffer_n(
                up_info->period_ns, down_info->deadline_ns, depth);
        }
    }

    state_val_ = State::INITIALIZED;
}

// -----------------------------------------------------------------------

void PreemptiveTeamManager::start() {
    std::lock_guard<std::mutex> lk(state_);
    if (state_val_ != State::INITIALIZED)
        throw std::runtime_error(
            "PreemptiveTeamManager::start: not in INITIALIZED state");

    for (int id : dispatcher_order_)
        dispatchers_.at(id)->start();

    state_val_ = State::RUNNING;
}

// -----------------------------------------------------------------------

void PreemptiveTeamManager::stop() {
    {
        std::lock_guard<std::mutex> lk(state_);
        if (state_val_ == State::TERMINATED || state_val_ == State::CREATED) return;
        if (state_val_ != State::TERMINATING) state_val_ = State::TERMINATING;
    }
    do_stop();
}

// -----------------------------------------------------------------------

void PreemptiveTeamManager::notify(int subtask_id) {
    std::lock_guard<std::mutex> lk(state_);
    if (state_val_ != State::RUNNING)
        throw std::runtime_error(
            "PreemptiveTeamManager::notify: not in RUNNING state");
    subtask_dispatcher_.at(subtask_id)->notify(subtasks_.at(subtask_id));
}

// -----------------------------------------------------------------------

void PreemptiveTeamManager::on_subtask_exception(int subtask_id) {
    std::lock_guard<std::mutex> lk(state_);
    if (state_val_ == State::RUNNING) {
        std::cerr << "[PreemptiveTeamManager] emergency stop triggered by subtask "
                  << subtask_id << "\n";
        state_val_ = State::TERMINATING;
    }
}

// -----------------------------------------------------------------------

PreemptiveTeamManager::State PreemptiveTeamManager::state() const {
    std::lock_guard<std::mutex> lk(state_);
    return state_val_;
}

// -----------------------------------------------------------------------

std::size_t PreemptiveTeamManager::dispatcher_count() const {
    std::lock_guard<std::mutex> lk(state_);
    return dispatchers_.size();
}

// -----------------------------------------------------------------------

std::size_t PreemptiveTeamManager::ring_buffer_size(int upstream_id,
                                                      int downstream_id) const {
    std::lock_guard<std::mutex> lk(state_);
    auto it = ring_buffer_sizes_.find({upstream_id, downstream_id});
    return (it != ring_buffer_sizes_.end()) ? it->second : 0;
}

// -----------------------------------------------------------------------

void PreemptiveTeamManager::do_stop() {
    // Reverse creation order: sinks stop first so no new notifications arrive
    // at an already-stopped dispatcher.
    for (auto it = dispatcher_order_.rbegin(); it != dispatcher_order_.rend(); ++it)
        dispatchers_.at(*it)->stop();

    std::lock_guard<std::mutex> lk(state_);
    state_val_ = State::TERMINATED;
}
