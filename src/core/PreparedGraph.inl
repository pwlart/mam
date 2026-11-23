// Included by Graph.hpp after the class definition; not a standalone header.
// Routing and summation order intentionally match Graph::process. This opt-in
// path does not promise that arbitrary Node implementations are allocation-free.

inline void Graph::prepareRealtime(double sampleRate, uint32_t maxFrames, uint32_t channels) {
  preparedValid_ = false;
  if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || maxFrames == 0)
    throw std::invalid_argument("Prepared graph requires a positive finite rate and frame capacity");
  if (channels != 1 && channels != 2)
    throw std::invalid_argument("Prepared graph currently supports mono or stereo");
  if (traceEnabled_)
    throw std::invalid_argument("Prepared graph does not support allocating performance traces");

  PreparedGraphState plan;
  plan.sampleRate = sampleRate;
  plan.maxFrames = maxFrames;
  plan.channels = channels;
  const size_t capacity = static_cast<size_t>(maxFrames) * channels;
  plan.silence.assign(capacity, 0.0f);
  plan.nodes.resize(nodes_.size());
  plan.targets.reserve(nodes_.size());
  for (size_t i = 0; i < nodes_.size(); ++i) {
    if (!nodes_[i].node || nodes_[i].id.empty() || !plan.targets.emplace(nodes_[i].id, i).second)
      throw std::invalid_argument("Prepared graph requires unique nonempty IDs and valid nodes");
  }
  for (const auto& edge : connections_) {
    if (!plan.targets.contains(edge.from) || !plan.targets.contains(edge.to))
      throw std::invalid_argument("Prepared graph connection references an unknown node");
    if (!std::isfinite(edge.gainPercent) || !std::isfinite(edge.dryPercent))
      throw std::invalid_argument("Prepared graph connection gains must be finite");
  }
  if (mixer_) {
    if (!std::isfinite(mixer_->masterGain()))
      throw std::invalid_argument("Prepared graph mixer gain must be finite");
    for (const auto& channel : mixer_->channels()) {
      if (!plan.targets.contains(channel.id) || !std::isfinite(channel.gain))
        throw std::invalid_argument("Prepared graph mixer input is unknown or nonfinite");
    }
  }

  rebuildTopology();
  if (topoOrder_.size() != nodes_.size())
    throw std::invalid_argument("Prepared graph does not support cyclic connections");
  plan.order = topoOrder_;
  for (size_t index = 0; index < nodes_.size(); ++index) {
    auto& item = plan.nodes[index];
    item.node = nodes_[index].node.get();
    item.delay = dynamic_cast<DelayNode*>(item.node);
    item.compressor = dynamic_cast<CompressorNode*>(item.node);
    item.meter = dynamic_cast<MeterNode*>(item.node);
    item.output.assign(capacity, 0.0f);
    std::unordered_map<uint32_t, size_t> portIndexes;
    const auto upstream = upstream_.find(index);
    if (upstream != upstream_.end()) {
      item.routes.reserve(upstream->second.size());
      for (const auto& edge : upstream->second) {
        const auto [port, inserted] = portIndexes.emplace(edge.toPort, item.ports.size());
        if (inserted) item.ports.emplace_back(capacity, 0.0f);
        if (edge.toPort == 0) item.mainPort = port->second;
        if (edge.toPort == 1) item.sidechainPort = port->second;
        item.routes.push_back({edge.fromIndex, port->second,
            getDeclaredOutChannels(edge.fromIndex, edge.fromPort),
            getDeclaredInChannels(index, edge.toPort), edge.gain});
      }
    }
    // Keep the legacy sink fallback, including its behavior for an explicit
    // zero mixer input, until a separately reviewed mixing-policy change.
    item.mixGain = mixerGainForId(nodes_[index].id);
    if (item.mixGain == 0.0f && !downstream_.contains(index)) item.mixGain = 1.0f;
  }
  for (const auto& edge : connections_) {
    const auto index = plan.targets.at(edge.from);
    if (mixerInputIds_.contains(nodes_[index].id)) continue;
    const float gain = edge.dryPercent * (1.0f / 100.0f);
    if (gain > 0.0f) plan.drySends.push_back({index, gain});
  }

  // Node allocation/preparation occurs before the plan becomes processable.
  for (auto& item : plan.nodes) {
    if (item.delay) item.delay->prepareRealtime(sampleRate, maxFrames, channels);
    else if (auto* spectral = dynamic_cast<SpectralDuckerNode*>(item.node))
      spectral->prepareRealtime(sampleRate, maxFrames, channels);
    else item.node->prepare(sampleRate, maxFrames);
  }
  if (statsEnabled_) initStats();
  if (cpuStatsEnabled_) {
    nodeNsSum_.assign(nodes_.size(), 0.0L);
    nodeNsMax_.assign(nodes_.size(), 0.0);
    nodeCalls_.assign(nodes_.size(), 0u);
  }
  prepared_ = std::move(plan);
  preparedValid_ = true;
}

inline Graph::PreparedProcessStatus Graph::processPrepared(
    ProcessContext ctx, std::span<float> output, uint32_t channels) noexcept {
  const auto fail = [&](PreparedProcessStatus result) {
    std::fill(output.begin(), output.end(), 0.0f);
    return result;
  };
  if (!preparedValid_) return fail(PreparedProcessStatus::NotPrepared);
  if (channels != prepared_.channels) return fail(PreparedProcessStatus::InvalidLayout);
  if (ctx.frames > prepared_.maxFrames) return fail(PreparedProcessStatus::CapacityExceeded);
  if (ctx.sampleRate != prepared_.sampleRate || !std::isfinite(ctx.sampleRate) ||
      output.size() < static_cast<size_t>(ctx.frames) * channels)
    return fail(PreparedProcessStatus::InvalidContext);
  try {
    processPreparedValid(ctx, output.data(), channels);
    return PreparedProcessStatus::Ok;
  } catch (...) {
    // Processing state may be partially advanced; explicit preparation is
    // required before retrying after a node failure.
    preparedValid_ = false;
    return fail(PreparedProcessStatus::NodeFailure);
  }
}

inline bool Graph::handlePreparedEvent(const Command& command) noexcept {
  if (!preparedValid_ || !command.nodeId) return false;
  const auto found = prepared_.targets.find(command.nodeId);
  if (found == prepared_.targets.end()) return false;
  try {
    prepared_.nodes[found->second].node->handleEvent(command);
    return true;
  } catch (...) {
    preparedValid_ = false;
    return false;
  }
}

inline void Graph::processPreparedValid(ProcessContext ctx, float* output, uint32_t channels) {
  const size_t total = static_cast<size_t>(ctx.frames) * channels;
  if (total == 0) return;
  const auto blockStart = cpuStatsEnabled_ ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};
  for (auto& item : prepared_.nodes) {
    std::fill_n(item.output.begin(), total, 0.0f);
    for (auto& port : item.ports) std::fill_n(port.begin(), total, 0.0f);
  }
  for (const size_t index : prepared_.order) {
    auto& item = prepared_.nodes[index];
    const auto nodeStart = cpuStatsEnabled_ ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
    for (const auto& route : item.routes) {
      adaptAndAccumulate(prepared_.nodes[route.fromIndex].output.data(),
          item.ports[route.portIndex], ctx.frames, channels,
          route.sourceChannels, route.destinationChannels, route.gain);
    }
    if (item.delay || item.compressor || item.meter) {
      if (item.mainPort != std::numeric_limits<size_t>::max())
        std::copy_n(item.ports[item.mainPort].begin(), total, item.output.begin());
      if (item.delay) {
        item.delay->processInPlace(ctx, item.output.data(), channels);
      } else if (item.compressor) {
        const float* sidechain = item.sidechainPort == std::numeric_limits<size_t>::max()
            ? prepared_.silence.data() : item.ports[item.sidechainPort].data();
        item.compressor->applySidechain(ctx, item.output.data(), sidechain, channels);
      } else {
        item.meter->updateFromBuffer(item.output.data(), ctx.frames, channels);
      }
    } else {
      item.node->process(ctx, item.output.data(), channels);
    }
    if (statsEnabled_) accumulateStats(index, item.output.data(), ctx.frames, channels);
    if (cpuStatsEnabled_) {
      const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - nodeStart).count());
      nodeNsSum_[index] += static_cast<long double>(ns);
      nodeNsMax_[index] = std::max(nodeNsMax_[index], ns);
      ++nodeCalls_[index];
    }
  }
  std::fill_n(output, total, 0.0f);
  for (const auto& send : prepared_.drySends) {
    const auto& source = prepared_.nodes[send.fromIndex].output;
    for (size_t i = 0; i < total; ++i) output[i] += source[i] * send.gain;
  }
  for (const auto& item : prepared_.nodes) {
    if (item.mixGain == 0.0f) continue;
    for (size_t i = 0; i < total; ++i) output[i] += item.output[i] * item.mixGain;
  }
  if (mixer_) mixer_->process(ctx, output, channels);
  if (cpuStatsEnabled_) {
    const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - blockStart).count());
    const double budget = 1e9 * static_cast<double>(ctx.frames) / ctx.sampleRate;
    const double percentage = ns / budget * 100.0;
    cpuNsSum_ += static_cast<long double>(ns);
    cpuPctSum_ += percentage;
    ++cpuBlocks_;
    cpuNsMax_ = std::max(cpuNsMax_, ns);
    cpuPctMax_ = std::max(cpuPctMax_, percentage);
    if (ns > budget) ++cpuOverruns_;
  }
}
