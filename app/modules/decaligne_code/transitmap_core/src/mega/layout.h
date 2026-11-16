#pragma once
#include "json.hpp"
namespace mega {
// Full state-level composite operations. No Python geometry or optimizer
// required.
nlohmann::json layoutRequest(const nlohmann::json &request);
nlohmann::json presentation(const nlohmann::json &state);
} // namespace mega
