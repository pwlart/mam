#include "SchemaValidate.hpp"

int validateJsonWithDraft2020(const std::string& jsonPath,
                              const std::string& schemaPath,
                              std::string& outDiagnostics) {
  // Strict validation is unavailable. Never substitute a no-op validator.
  // Semantic CLI checks remain available via --validate.
  (void)jsonPath;
  (void)schemaPath;
  outDiagnostics = "Full JSON Schema validation is unavailable in this build; "
                   "--validate provides structural/semantic checks only.";
  return 3;
}
