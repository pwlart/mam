#include "session/SessionRuntime.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

// Link only mam_session_runtime: exercise its public defaults and every moved
// method without depending on the application's aggregate link libraries.
int main() {
  SessionSpec spec;
  spec.sampleRate = 48000;
  spec.channels = 2;
  SessionRuntime runtime;
  runtime.loadFromSpec(spec);
  runtime.setPerRackMeters(true);
  runtime.setPerRackCpu(false);
  if (runtime.planTotalFrames(1.0) != 48 || runtime.planTotalFrames(0.0, true, 2) != 0) {
    std::cerr << "Empty-session frame planning changed\n";
    return 1;
  }
  const auto audio = runtime.renderOffline(8);
  const auto loop = runtime.renderOfflineWithLoop(8);
  if (audio.size() != 16 || loop != audio ||
      !std::all_of(audio.begin(), audio.end(), [](float sample) { return sample == 0; })) {
    std::cerr << "Empty session must render the requested stereo silence\n";
    return 1;
  }
  double peak = 1, rms = 1;
  SessionRuntime::computePeakAndRmsSimple(std::vector<float>{1, -1, 1, -1}, 2, peak, rms);
  if (peak != 0 || rms != 0 || !std::isfinite(peak) || !std::isfinite(rms)) {
    std::cerr << "Unit-magnitude fixture must meter at zero dB\n";
    return 1;
  }
}
