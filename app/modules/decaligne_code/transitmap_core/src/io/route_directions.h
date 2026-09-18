#pragma once
#include "core/shape.h"
#include "json.hpp"

// Authoritative traversal records use stable node IDs, never screen coordinates.
bool hasRouteDirections(const nlohmann::json& map);
Shape loadDirectedTopology(const nlohmann::json& map);
void retainMapNodeIds(Shape& shape, const nlohmann::json& map);
nlohmann::json importRouteDirections(const Shape& shape, const nlohmann::json& map);
nlohmann::json mapGtfsDirections(const Shape& shape, const nlohmann::json& data);
nlohmann::json editRouteDirections(const Shape& before, const Shape& after,
    const nlohmann::json& records, const std::string& op, const nlohmann::json& request);
nlohmann::json remapRouteDirections(const Shape& before, const Shape& after,
    const nlohmann::json& records);
void exportRouteDirections(nlohmann::json& graph, const nlohmann::json& records);
