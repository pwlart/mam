#pragma once

// Offline-only native PCM stem arrangement. JSON paths resolve relative to
// the job file; all timeline positions and lengths are integer source frames.
int runStemRenderer(int argc, char** argv);
