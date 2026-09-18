#pragma once
#include "json.hpp"
#include "core/shape.h"

// GTFS tables are decoded by Python; ordering and pattern identity belong here.
nlohmann::json gtfsDirectionalPatterns(const nlohmann::json& tables);
void validateDirectionalData(const nlohmann::json& data);
// Non-owning, validated view over transitMapDirections. Never stored separately.
// Samples combine shape points and every stop occurrence in GTFS sequence order.
struct OrderedTraversal {
    std::string routeId;
    const nlohmann::json* pattern;
    struct Sample { Point position; int stopIndex = -1; };
    std::vector<Sample> samples;
};
std::vector<OrderedTraversal> orderedTraversals(const nlohmann::json& data);
// Render-only expansion. The authoritative editable Shape is never modified.
Shape directionalRenderShape(const Shape& shape, const nlohmann::json& data,
                             const nlohmann::json& selected);
void directionalSharedGeometry(const Shape& shape, StyledShape& styled);
