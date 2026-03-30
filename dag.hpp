#ifndef DAG_HPP
#define DAG_HPP

#include <vector>
#include "component.hpp"

class DAG {
public:
    struct Node {
        int id;
        int last_task;
        int next_task;
    };

    DAG() {}

private:
    std::vector<Node> nodes_;
};

#endif // DAG_HPP
