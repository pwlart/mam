#pragma once

// CLI-only worker launch. Never call from an audio callback.
int runPythonTools(int argc, char** argv);
