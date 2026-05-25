#pragma once
#ifndef TEAM_MANAGER_HPP
#define TEAM_MANAGER_HPP

#include "component.hpp"
#include "deployment_plan.hpp"
#include <vector>
#include <string>

enum TeamState {
    TEAM_CREATED,      // team foi criado mas nao inicializado
    TEAM_INITIALIZED,  // subtasks inicializadas e prontas
    TEAM_RUNNING,      // subtasks em execucao
    TEAM_TERMINATING,  // protocolo de terminacao em andamento
    TEAM_TERMINATED    // todas subtasks finalizadas
};

