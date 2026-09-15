#pragma once
#include "json.hpp"
// Stateless transport adapter. VizAPI owns sessions/revisions and sends the
// previous C++ state back; all topology and geometry operations stay in C++.
nlohmann::json transitCoreRequest(const nlohmann::json& request);
