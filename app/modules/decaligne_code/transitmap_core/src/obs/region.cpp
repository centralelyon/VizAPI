#include <algorithm>
#include <cmath>
#include <limits>

#include "region.h"

namespace {

    bool segmentIntersectionPoint(const Point& a, const Point& b, const Point& c, const Point& d, Point& out) {

        const double rX = b.x - a.x;
        const double rY = b.y - a.y;
        const double sX = d.x - c.x;
        const double sY = d.y - c.y;
        const double den = rX * sY - rY * sX;
        if (std::abs(den) <= 1e-12) return false;

        const double qpx = c.x - a.x;
        const double qpy = c.y - a.y;
        const double t = (qpx * sY - qpy * sX) / den;
        const double u = (qpx * rY - qpy * rX) / den;
        if (t < -1e-9 || t > 1.0 + 1e-9 || u < -1e-9 || u > 1.0 + 1e-9) return false;

        out = { a.x + rX * t, a.y + rY * t };
        return true;
    }

    bool segmentIntersectsPolygonInterior(const Point& a, const Point& b, const std::vector<Point>& polygon) {

        if (polygon.size() < 3) return false;
        if (Geometry::pointInPolygon(a, polygon) || Geometry::pointInPolygon(b, polygon)) return true;

        for (size_t i = 0; i < polygon.size(); ++i) {
            Point inter{};
            if (!segmentIntersectionPoint(a, b, polygon[i], polygon[(i + 1) % polygon.size()], inter)) continue;
            if (Geometry::samePoint(inter, a) || Geometry::samePoint(inter, b)) continue;
            return true;
        }
        return false;
    }

    Point nearestPointOnPolygonBoundary(const Point& p, const std::vector<Point>& polygon) {

        Point nearest = p;
        if (polygon.empty()) return nearest;

        double bestDist2 = std::numeric_limits<double>::max();
        for (size_t i = 0; i < polygon.size(); ++i) {
            const Point& a = polygon[i];
            const Point& b = polygon[(i + 1) % polygon.size()];
            const double abx = b.x - a.x;
            const double aby = b.y - a.y;
            const double ab2 = abx * abx + aby * aby;
            double t = 0.0;
            if (ab2 > 1e-12) {
                t = ((p.x - a.x) * abx + (p.y - a.y) * aby) / ab2;
                t = std::clamp(t, 0.0, 1.0);
            }
            const Point cand{ a.x + abx * t, a.y + aby * t };
            const double dx = p.x - cand.x;
            const double dy = p.y - cand.y;
            const double d2 = dx * dx + dy * dy;
            if (d2 < bestDist2) {
                bestDist2 = d2;
                nearest = cand;
            }
        }
        return nearest;
    }

    bool raySegmentIntersection(const Point& origin, const Point& dir, const Point& a, const Point& b, double& outT) {

        const double ex = b.x - a.x;
        const double ey = b.y - a.y;
        const double den = dir.x * ey - dir.y * ex;
        if (std::abs(den) <= 1e-12) return false;

        const double ax = a.x - origin.x;
        const double ay = a.y - origin.y;
        const double t = (ax * ey - ay * ex) / den;
        const double u = (ax * dir.y - ay * dir.x) / den;
        if (t < 0.0 || u < 0.0 || u > 1.0) return false;

        outT = t;
        return true;
    }

    Point movePointOutsidePolygonAlongDirection(const Point& p, const std::vector<Point>& polygon, Point preferredDir) {

        if (polygon.empty()) return p;
        double len = std::sqrt(preferredDir.x * preferredDir.x + preferredDir.y * preferredDir.y);
        if (len <= 1e-8) {
            const Point boundary = nearestPointOnPolygonBoundary(p, polygon);
            preferredDir = { boundary.x - p.x, boundary.y - p.y };
            len = std::sqrt(preferredDir.x * preferredDir.x + preferredDir.y * preferredDir.y);
        }
        if (len <= 1e-8) {
            preferredDir = { 1.0, 0.0 };
            len = 1.0;
        }

        const Point dir{ preferredDir.x / len, preferredDir.y / len };
        double minT = std::numeric_limits<double>::max();
        bool found = false;
        for (size_t i = 0; i < polygon.size(); ++i) {
            const Point& a = polygon[i];
            const Point& b = polygon[(i + 1) % polygon.size()];
            double t = 0.0;
            if (!raySegmentIntersection(p, dir, a, b, t)) continue;
            if (t < minT) {
                minT = t;
                found = true;
            }
        }

        constexpr double kPushOutDistance = 0.5;
        if (found) return { p.x + dir.x * (minT + kPushOutDistance), p.y + dir.y * (minT + kPushOutDistance) };

        const Point boundary = nearestPointOnPolygonBoundary(p, polygon);
        return { boundary.x + dir.x * kPushOutDistance, boundary.y + dir.y * kPushOutDistance };
    }

    double pointToSegmentDistanceSquared(const Point& p, const Point& a, const Point& b) {

        const double abx = b.x - a.x;
        const double aby = b.y - a.y;
        const double apx = p.x - a.x;
        const double apy = p.y - a.y;
        const double ab2 = abx * abx + aby * aby;
        if (ab2 <= 1e-12) {
            const double dx = p.x - a.x;
            const double dy = p.y - a.y;
            return dx * dx + dy * dy;
        }
        double t = (apx * abx + apy * aby) / ab2;
        t = std::max(0.0, std::min(1.0, t));
        const double cx = a.x + t * abx;
        const double cy = a.y + t * aby;
        const double dx = p.x - cx;
        const double dy = p.y - cy;
        return dx * dx + dy * dy;
    }

    static constexpr double kOctilinearStep = 3.14159265358979323846 / 4.0;

    Point snapVectorToOctilinear(const Point& v) {

        const double len = std::hypot(v.x, v.y);
        if (len <= 1e-9) return { 0.0, 0.0 };

        const double angle = std::atan2(v.y, v.x);
        const double snapped = std::round(angle / kOctilinearStep) * kOctilinearStep;
        return { std::cos(snapped), std::sin(snapped) };
    }

    bool intersectInfiniteLines(const Point& p, const Point& r, const Point& q, const Point& s, Point& out) {

        const double cross = r.x * s.y - r.y * s.x;
        if (std::abs(cross) <= 1e-9) return false;

        const Point qp{ q.x - p.x, q.y - p.y };
        const double t = (qp.x * s.y - qp.y * s.x) / cross;
        out = { p.x + r.x * t, p.y + r.y * t };
        return true;
    }



} // namespace

bool buildRegionLoopNodeIndices(const Region& region, int polygonIndex, std::vector<int>& outNodeLoop) {

    outNodeLoop.clear();
    if (polygonIndex < 0 || polygonIndex >= (int)region.segIdx.size()) return false;
    const auto& segIndices = region.segIdx[polygonIndex];
    if (segIndices.empty()) return false;

    bool initialized = false;
    int currentNode = -1;
    for (int segIndex : segIndices) {
        if (segIndex < 0 || segIndex >= (int)region.segments.size()) return false;
        const auto& seg = region.segments[segIndex];
        if (seg.a < 0 || seg.b < 0 || seg.a >= (int)region.nodes.size() || seg.b >= (int)region.nodes.size()) return false;

        if (!initialized) {
            outNodeLoop.push_back(seg.a);
            outNodeLoop.push_back(seg.b);
            currentNode = seg.b;
            initialized = true;
            continue;
        }

        if (seg.a == currentNode) {
            outNodeLoop.push_back(seg.b);
            currentNode = seg.b;
        }
        else if (seg.b == currentNode) {
            outNodeLoop.push_back(seg.a);
            currentNode = seg.a;
        }
        else {
            return false;
        }
    }

    if (outNodeLoop.size() < 3) return false;
    if (outNodeLoop.front() != outNodeLoop.back()) outNodeLoop.push_back(outNodeLoop.front());
    return outNodeLoop.size() >= 4;
}

bool buildRegionPolygonPoints(const Region& region, int polygonIndex, std::vector<Point>& outPolygon) {

    std::vector<int> loopNodeIndices;
    if (!buildRegionLoopNodeIndices(region, polygonIndex, loopNodeIndices)) return false;
    outPolygon.clear();
    outPolygon.reserve(loopNodeIndices.size());
    for (int nodeIndex : loopNodeIndices) {
        if (nodeIndex < 0 || nodeIndex >= (int)region.nodes.size()) {
            outPolygon.clear();
            return false;
        }
        outPolygon.push_back(region.nodes[nodeIndex]);
    }
    if (outPolygon.size() < 4) {
        outPolygon.clear();
        return false;
    }
    if (Geometry::samePoint(outPolygon.front(), outPolygon.back())) {
        outPolygon.pop_back();
    }
    return outPolygon.size() >= 3;
}

bool buildRegionPolygons(const Region& region, std::vector<std::vector<Point>>& outPolygons) {

    outPolygons.clear();
    outPolygons.reserve(region.segIdx.size());
    for (int polyIdx = 0; polyIdx < (int)region.segIdx.size(); ++polyIdx) {
        std::vector<Point> polygon;
        if (!buildRegionPolygonPoints(region, polyIdx, polygon)) continue;
        outPolygons.push_back(std::move(polygon));
    }
    return !outPolygons.empty();
}

bool isPointOnAnyRegionEdge(const Point& p, const Region& region, double tolerance) {

    if (region.segments.empty() || region.nodes.empty()) return false;
    const double tol2 = tolerance * tolerance;
    for (const auto& seg : region.segments) {
        if (seg.a < 0 || seg.b < 0) continue;
        if (seg.a >= (int)region.nodes.size() || seg.b >= (int)region.nodes.size()) continue;
        if (pointToSegmentDistanceSquared(p, region.nodes[seg.a], region.nodes[seg.b]) <= tol2) {
            return true;
        }
    }
    return false;
}

bool pointInsideAnyRegionPolygon(const Point& p, const std::vector<std::vector<Point>>& polygons) {

    for (const auto& poly : polygons) {
        if (Geometry::pointInPolygon(p, poly)) return true;
    }
    return false;
}

bool segmentIntersectsAnyRegionPolygonInterior(const Point& a, const Point& b, const std::vector<std::vector<Point>>& polygons) {

    for (const auto& poly : polygons) {
        if (segmentIntersectsPolygonInterior(a, b, poly)) return true;
    }
    return false;
}

Point movePointOutsideRegionPolygonsAlongDirection(const Point& p, const std::vector<std::vector<Point>>& polygons, Point preferredDir) {

    if (polygons.empty()) return p;

    for (const auto& polygon : polygons) {
        if (!Geometry::pointInPolygon(p, polygon)) continue;
        return movePointOutsidePolygonAlongDirection(p, polygon, preferredDir);
    }

    return movePointOutsidePolygonAlongDirection(p, polygons.front(), preferredDir);
}

bool refineRegionToOctilinear(Region& region) {

    if (region.segIdx.empty() || region.nodes.empty() || region.segments.empty()) return false;

    bool regionChanged = false;

    for (int polyIdx = 0; polyIdx < (int)region.segIdx.size(); ++polyIdx) {
        std::vector<int> loopNodeIndices;
        if (!buildRegionLoopNodeIndices(region, polyIdx, loopNodeIndices)) continue;
        if (loopNodeIndices.size() < 4) continue;

        loopNodeIndices.pop_back();
        if (loopNodeIndices.size() < 3) continue;

        std::vector<Point> original;
        original.reserve(loopNodeIndices.size());
        bool valid = true;
        for (int nodeIndex : loopNodeIndices) {
            if (nodeIndex < 0 || nodeIndex >= (int)region.nodes.size()) {
                valid = false;
                break;
            }
            original.push_back(region.nodes[nodeIndex]);
        }
        if (!valid) continue;

        std::vector<Point> refined(loopNodeIndices.size());
        refined[0] = original[0];

        for (size_t i = 1; i < original.size(); ++i) {
            const Point vec{ original[i].x - original[i - 1].x, original[i].y - original[i - 1].y };
            const double segLen = std::hypot(vec.x, vec.y);
            if (segLen <= 1e-9) {
                refined[i] = refined[i - 1];
                continue;
            }
            const Point snappedDir = snapVectorToOctilinear(vec);
            refined[i] = { refined[i - 1].x + snappedDir.x * segLen, refined[i - 1].y + snappedDir.y * segLen };
        }

        if (refined.size() >= 3) {
            const size_t last = refined.size() - 1;
            const Point incoming = snapVectorToOctilinear({
                original[last].x - original[last - 1].x,
                original[last].y - original[last - 1].y
                });
            const Point closing = snapVectorToOctilinear({
                original[0].x - original[last].x,
                original[0].y - original[last].y
                });

            Point closurePoint{};
            if (intersectInfiniteLines(
                refined[last - 1], incoming,
                refined[0], { -closing.x, -closing.y },
                closurePoint)) {
                refined[last] = closurePoint;
            }
        }

        for (size_t i = 0; i < loopNodeIndices.size(); ++i) {
            const int nodeIndex = loopNodeIndices[i];
            if (!Geometry::samePoint(region.nodes[nodeIndex], refined[i])) {
                region.nodes[nodeIndex] = refined[i];
                regionChanged = true;
            }
        }
    }

    return regionChanged;
}

bool buildRegionBounds(const Region& region, int polygonIndex, RegionBounds& outBounds) {

    outBounds = {};
    std::vector<Point> polygon;
    if (!buildRegionPolygonPoints(region, polygonIndex, polygon) || polygon.empty()) return false;

    outBounds.min = polygon.front();
    outBounds.max = polygon.front();
    for (const auto& point : polygon) {
        outBounds.min.x = std::min(outBounds.min.x, point.x);
        outBounds.min.y = std::min(outBounds.min.y, point.y);
        outBounds.max.x = std::max(outBounds.max.x, point.x);
        outBounds.max.y = std::max(outBounds.max.y, point.y);
    }
    return outBounds.isValid();
}

Point regionBoundsTopRight(const RegionBounds& bounds) {

    if (!bounds.isValid()) return { 0.0, 0.0 };
    return { bounds.max.x, bounds.max.y };
}