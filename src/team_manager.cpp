#include "team_manager.hpp"

TeamManager::TeamManager() : state_(State::CREATED) {}

TeamManager::~TeamManager() { stop(); }

// -----------------------------------------------------------------------

void TeamManager::initialize(const std::vector<SubtaskEntry>& entries,
                              const DAG& dag) {
    std::lock_guard<std::mutex> lk(state_mutex_);
    if (state_ != State::CREATED)
        throw std::runtime_error("TeamManager::initialize: already called");

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
                "TeamManager::initialize: no SubtaskEntry for DAG node "
                + std::to_string(id));
    }

    // --- Dispatcher grouping (paper Section V-B) ---
    // Create at most one Dispatcher per (core, priority) pair.
    // Multiple subtasks on the same core/priority share a single thread,
    // queue and eventfd — this is the partitioned fixed-priority model.
    for (int id : topo_order_) {
        const SubtaskInfo* info = info_map.at(id);

        // A negative core would reach CPU_SET() unchecked inside the Dispatcher.
        // Entries built by hand never went through the allocator, so guard here.
        if (info->core < 0)
            throw std::runtime_error(
                "TeamManager::initialize: subtask " + std::to_string(id) +
                " has no core assigned");

        CorePrio cp{info->core, info->priority};

        if (dispatchers_.find(cp) == dispatchers_.end()) {
            dispatchers_[cp] = std::make_unique<Dispatcher>(info->core, info->priority);
            dispatcher_order_.push_back(cp);  // remember creation order
        }
        subtask_dispatcher_[id] = dispatchers_.at(cp).get();
    }

    // --- Configure each subtask and wire connections ---
    for (const auto& node : dag.nodes()) {
        int id = node.id;
        if (subtasks_.find(id) == subtasks_.end()) continue;

        Subtask*           s    = subtasks_.at(id);
        const SubtaskInfo* info = info_map.at(id);

        // Scheduling metadata from SubtaskInfo
        s->period_ns = info->period_ns;

        // fan_in_total derived from the DAG (min 1 for source nodes)
        s->fan_in_total = static_cast<int>(node.predecessors.size());
        if (s->fan_in_total < 1) s->fan_in_total = 1;
        s->fan_in_received.store(0);

        // Downstream wiring: use each successor's Dispatcher (may be shared)
        s->downstream.clear();
        for (int succ_id : node.successors)
            s->downstream.push_back({subtask_dispatcher_.at(succ_id),
                                     subtasks_.at(succ_id)});

        // Register subtask with its (possibly shared) Dispatcher
        subtask_dispatcher_.at(id)->register_subtask(s);

        // Wrap execute() to catch exceptions without crashing the dispatcher thread
        auto original_fn = s->execute;
        s->execute = [this, id, original_fn]() {
            try {
                original_fn();
            } catch (const std::exception& e) {
                std::cerr << "[TeamManager] subtask " << id
                          << " threw: " << e.what() << "\n";
                on_subtask_exception(id);
            } catch (...) {
                std::cerr << "[TeamManager] subtask " << id
                          << " threw unknown exception\n";
                on_subtask_exception(id);
            }
        };
    }

    // --- Ring buffer sizing (paper Section IV) ---
    // N = max(2, ceil(deadline_downstream / period_upstream) + pipeline_depth)
    // Computed per DAG edge so the user / code-generator can allocate buffers
    // with the right compile-time N.
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

    state_ = State::INITIALIZED;
}

// -----------------------------------------------------------------------

void TeamManager::start() {
    std::lock_guard<std::mutex> lk(state_mutex_);
    if (state_ != State::INITIALIZED)
        throw std::runtime_error("TeamManager::start: not in INITIALIZED state");

    // Start in topological creation order (sources before sinks)
    for (const auto& cp : dispatcher_order_)
        dispatchers_.at(cp)->start();

    state_ = State::RUNNING;
}

// -----------------------------------------------------------------------

void TeamManager::stop() {
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        if (state_ == State::TERMINATED || state_ == State::CREATED) return;
        if (state_ != State::TERMINATING) state_ = State::TERMINATING;
    }
    do_stop();
}

// -----------------------------------------------------------------------

void TeamManager::notify(int subtask_id) {
    std::lock_guard<std::mutex> lk(state_mutex_);
    if (state_ != State::RUNNING)
        throw std::runtime_error("TeamManager::notify: not in RUNNING state");
    subtask_dispatcher_.at(subtask_id)->notify(subtasks_.at(subtask_id));
}

// -----------------------------------------------------------------------

void TeamManager::on_subtask_exception(int subtask_id) {
    std::lock_guard<std::mutex> lk(state_mutex_);
    if (state_ == State::RUNNING) {
        std::cerr << "[TeamManager] emergency stop triggered by subtask "
                  << subtask_id << "\n";
        state_ = State::TERMINATING;
        // The caller (main thread or external monitor) must call stop() to
        // finalize. Calling stop() from a dispatcher thread would deadlock
        // on pthread_join.
    }
}

// -----------------------------------------------------------------------

TeamManager::State TeamManager::state() const {
    std::lock_guard<std::mutex> lk(state_mutex_);
    return state_;
}

// -----------------------------------------------------------------------

std::size_t TeamManager::dispatcher_count() const {
    std::lock_guard<std::mutex> lk(state_mutex_);
    return dispatchers_.size();
}

// -----------------------------------------------------------------------

std::size_t TeamManager::ring_buffer_size(int upstream_id, int downstream_id) const {
    std::lock_guard<std::mutex> lk(state_mutex_);
    auto it = ring_buffer_sizes_.find({upstream_id, downstream_id});
    return (it != ring_buffer_sizes_.end()) ? it->second : 0;
}

// -----------------------------------------------------------------------

void TeamManager::do_stop() {
    // Reverse creation order: Dispatchers created last (sink-side) stop first,
    // so no new notifications arrive at an already-stopped Dispatcher.
    for (auto it = dispatcher_order_.rbegin(); it != dispatcher_order_.rend(); ++it)
        dispatchers_.at(*it)->stop();

    std::lock_guard<std::mutex> lk(state_mutex_);
    state_ = State::TERMINATED;
}
