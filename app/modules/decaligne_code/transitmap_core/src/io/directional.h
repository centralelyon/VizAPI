#pragma once
#include "json.hpp"
#include "core/shape.h"

// GTFS tables are decoded by Python; ordering and pattern identity belong here.
nlohmann::json gtfsDirectionalPatterns(const nlohmann::json& tables);
void validateDirectionalData(const nlohmann::json& data);
// Render-only expansion. The authoritative editable Shape is never modified.
Shape directionalRenderShape(const Shape& shape, const nlohmann::json& data,
                             const nlohmann::json& selected);
void directionalSharedGeometry(const Shape& shape, StyledShape& styled);
