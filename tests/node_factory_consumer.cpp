#include "core/NodeFactory.hpp"

#include <iostream>
#include <string_view>

// This executable links only mam_nodes. The broad mam_render/mam_app aggregate
// must not hide missing DSP/I/O dependencies or incomplete public header types.
int main() {
  for (const auto* type : {"kick", "clap", "wiretap"}) {
    NodeSpec spec;
    spec.id = type;
    spec.type = type;
    spec.paramsJson = std::string_view(type) == "wiretap"
        ? R"({"enabled":false,"path":""})" : "{}";
    auto node = createNodeFromSpec(spec);
    if (!node) {
      std::cerr << "Factory did not construct " << type << '\n';
      return 1;
    }
    node->prepare(48000, 16);
    node->reset();
  }
  NodeSpec unknown;
  unknown.id = "unknown";
  unknown.type = "unknown";
  unknown.paramsJson = "{}";
  if (createNodeFromSpec(unknown)) {
    std::cerr << "Factory changed unknown-type behavior\n";
    return 1;
  }
}
