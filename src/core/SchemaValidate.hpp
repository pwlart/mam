#pragma once

#include <string>

// Returns 3 (unavailable) until a real Draft 2020 validator is integrated.
// Never report successful strict validation through the old no-op shim.
int validateJsonWithDraft2020(const std::string& jsonPath,
                              const std::string& schemaPath,
                              std::string& outDiagnostics);
