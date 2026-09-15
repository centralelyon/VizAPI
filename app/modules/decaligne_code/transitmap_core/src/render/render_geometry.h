#pragma once
#include "core/shape.h"
#include "core/camera.h"

struct RenderCommand {
    enum class Type { Move, Line, Quadratic };
    Type type = Type::Line;
    Point end{}, control{};
};

struct StyledShapeDisplayData {
    std::vector<std::vector<std::vector<RenderCommand>>> commands;
    std::vector<std::vector<std::vector<Point>>> screenRoutePaths;
    std::vector<std::vector<std::vector<Point>>> worldRoutePaths;
    std::vector<StyledShape::NormalStation> normalStations;
    std::vector<StyledShape::TransferStation> transferStations;
};

// Computes exactly the same spacing- and bend-adjusted geometry used by drawStyledShape.
StyledShapeDisplayData buildStyledShapeDisplayData(
    const StyledShape& styledShape, const Camera& camera);

