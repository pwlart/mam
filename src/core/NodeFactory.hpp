#pragma once

#include "Node.hpp"
#include "GraphConfig.hpp"
#include <memory>

// Node construction belongs to the assembly implementation. Mixer creation is
// separate: Graph consumes GraphSpec::mixer directly.
std::unique_ptr<Node> createNodeFromSpec(const NodeSpec& spec);
