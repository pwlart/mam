#include "BuildInfo.hpp"
#include "BuildIdentity.hpp"

const char* mamBuildInfoJson() { return mamBuildIdentity; }
const char* mamVersion() { return mamBuildVersion; }
const char* mamGitRevision() { return mamBuildRevision; }
