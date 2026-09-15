#pragma once
#include <string>
#include "json.hpp"
struct Shape;
nlohmann::json shapeToLoomGeoJson(const Shape& shape, bool excludeRegions = false);
#include <unordered_set>
#include "core/shape.h"
#include "render/style.h"
#include "core/camera.h"

bool exportShapeToLoomGeoJson(const Shape& shape, const std::string& outPath, bool excludeRegions = false);

bool exportStyledShapeToGeoJson(const Shape& shape, const StyledShape& styledShape,
    const Camera& camera, const std::string& outPath, bool excludeRegions = false);

// save as SVG button include Style
bool exportShapeToSvg(const Shape& shape, const std::string& outPath, bool excludeRegions = false,
    const TransitMapStyle* style = nullptr, const Camera* camera = nullptr,
    const StyledShape* styledShape = nullptr,
    const std::unordered_map<int, Point>* labelOffsets = nullptr,
    const std::unordered_set<int>* visibleLabelIndices = nullptr,
    const std::string* labelFontPath = nullptr,
    double labelBaselineOffset = -1.0);
