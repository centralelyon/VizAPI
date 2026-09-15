#pragma once
#include <vector>
#include <limits>

#include "core/geometry.h"

struct RegionBounds {
    Point min{ std::numeric_limits<double>::max(), std::numeric_limits<double>::max() };
    Point max{ std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest() };

    bool isValid() const { return min.x <= max.x && min.y <= max.y; }
    double width() const { return max.x - min.x; }
    double height() const { return max.y - min.y; }
};

class Region {

public:
    struct Segment {
        int a = -1;
        int b = -1;
    };
	// shape nodes
    std::vector<Point> nodes;
    std::vector<Segment> segments;
    // boundaries of region --> Polygon
    std::vector<std::vector<int>> segIdx;

};

bool buildRegionLoopNodeIndices(const Region& region, int polygonIndex, std::vector<int>& outNodeLoop);
bool buildRegionPolygonPoints(const Region& region, int polygonIndex, std::vector<Point>& outPolygon);
bool buildRegionPolygons(const Region& region, std::vector<std::vector<Point>>& outPolygons);
bool isPointOnAnyRegionEdge(const Point& p, const Region& region, double tolerance);

bool pointInsideAnyRegionPolygon(const Point& p, const std::vector<std::vector<Point>>& polygons);
bool segmentIntersectsAnyRegionPolygonInterior(const Point& a, const Point& b, const std::vector<std::vector<Point>>& polygons);
Point movePointOutsideRegionPolygonsAlongDirection(const Point& p, const std::vector<std::vector<Point>>& polygons, Point preferredDir);

bool refineRegionToOctilinear(Region& region);

bool buildRegionBounds(const Region& region, int polygonIndex, RegionBounds& outBounds);
Point regionBoundsTopRight(const RegionBounds& bounds);

