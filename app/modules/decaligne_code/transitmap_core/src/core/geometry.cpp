#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

#include "core/geometry.h"

#include <unordered_set>

GeoData Geometry::filterRoutes(const GeoData& source, const std::unordered_set<std::string>& routeIds) {
    GeoData filtered;
    filtered.obstacles = source.obstacles;

    std::vector<char> stationUsed(source.stations.size(), 0);
    for (const Route& route : source.routes) {
        if (routeIds.count(route.id) == 0) continue;
        filtered.routes.push_back(route);
        for (int stationIndex : route.stationIndices) {
            if (stationIndex >= 0 && stationIndex < static_cast<int>(stationUsed.size())) {
                stationUsed[stationIndex] = 1;
            }
        }
    }

    std::vector<int> oldToNew(source.stations.size(), -1);
    for (int oldIndex = 0; oldIndex < static_cast<int>(source.stations.size()); ++oldIndex) {
        if (!stationUsed[oldIndex]) continue;
        oldToNew[oldIndex] = static_cast<int>(filtered.stations.size());
        filtered.stations.push_back(source.stations[oldIndex]);
    }

    for (Route& route : filtered.routes) {
        std::vector<int> remapped;
        remapped.reserve(route.stationIndices.size());
        for (int oldIndex : route.stationIndices) {
            if (oldIndex < 0 || oldIndex >= static_cast<int>(oldToNew.size())) continue;
            const int newIndex = oldToNew[oldIndex];
            if (newIndex >= 0) remapped.push_back(newIndex);
        }
        route.stationIndices = std::move(remapped);
    }

    filtered.stationAdj.assign(filtered.stations.size(), {});
    for (int oldIndex = 0; oldIndex < static_cast<int>(oldToNew.size()); ++oldIndex) {
        const int newIndex = oldToNew[oldIndex];
        if (newIndex < 0 || oldIndex >= static_cast<int>(source.stationAdj.size())) continue;
        auto& neighbors = filtered.stationAdj[newIndex];
        for (int oldNeighbor : source.stationAdj[oldIndex]) {
            if (oldNeighbor < 0 || oldNeighbor >= static_cast<int>(oldToNew.size())) continue;
            const int newNeighbor = oldToNew[oldNeighbor];
            if (newNeighbor >= 0) neighbors.push_back(newNeighbor);
        }
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    }
    return filtered;
}


// helper functions
namespace Geometry {

    std::string formatPoint(const Point& p) {

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << "(" << p.x << ", " << p.y << ")";
        return oss.str();
    }

    std::string stationKey(const Station& station) { return !station.station_id.empty() ? station.station_id : station.id; }

    bool parsePrefixedId(const std::string& value, const std::string& prefix, int& out) {

        if (value.rfind(prefix, 0) != 0) return false;
        const char* begin = value.data() + prefix.size();
        const char* end = value.data() + value.size();
        if (begin == end) return false;
        auto [ptr, ec] = std::from_chars(begin, end, out);
        return ec == std::errc() && ptr == end;
    }

    double pointSegmentDist2(double px, double py, double ax, double ay, double bx, double by) {

        const double abx = bx - ax;
        const double aby = by - ay;
        const double apx = px - ax;
        const double apy = py - ay;
        const double ab2 = abx * abx + aby * aby;
        double t = 0.0;
        if (ab2 > 1e-12) {
            t = (apx * abx + apy * aby) / ab2;
            t = std::clamp(t, 0.0, 1.0);
        }
        const double cx = ax + abx * t;
        const double cy = ay + aby * t;
        const double dx = px - cx;
        const double dy = py - cy;
        return dx * dx + dy * dy;
    }

    bool pointInPolygon(const Point& p, const std::vector<Point>& polygon) {

        if (polygon.size() < 3) return false;

        bool inside = false;
        size_t j = polygon.size() - 1;
        for (size_t i = 0; i < polygon.size(); ++i) {
            const Point& pi = polygon[i];
            const Point& pj = polygon[j];
            const bool intersects = ((pi.y > p.y) != (pj.y > p.y))
                && (p.x < (pj.x - pi.x) * (p.y - pi.y) / ((pj.y - pi.y) + 1e-12) + pi.x);
            if (intersects) inside = !inside;
            j = i;
        }
        return inside;
    }

    bool samePoint(const Point& a, const Point& b, double eps) {

        const double dx = a.x - b.x;
        const double dy = a.y - b.y;
        return (dx * dx + dy * dy) <= eps * eps;
    }
} // namespace Geometry

namespace {
    double dist(const Point& a, const Point& b) {
        const double dx = a.x - b.x;
        const double dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    std::vector<Point> flattenSegments(const std::vector<std::vector<Point>>& segments) {
        std::vector<Point> points;
        for (const auto& seg : segments) {
            for (const Point& p : seg) {
                if (points.empty() || !Geometry::samePoint(points.back(), p)) points.push_back(p);
            }
        }
        return points;
    }

    double polylineLength(const std::vector<Point>& points) {
        double len = 0.0;
        for (size_t i = 1; i < points.size(); ++i) len += dist(points[i - 1], points[i]);
        return len;
    }
}

namespace Geometry {

    Point pointAtRelativePosition(const std::vector<std::vector<Point>>& segments, double t) {
        const std::vector<Point> points = flattenSegments(segments);
        if (points.empty()) return Point{};
        if (points.size() == 1) return points.front();
        t = std::clamp(t, 0.0, 1.0);
        const double total = polylineLength(points);
        if (total <= 1e-12) return points.front();
        double target = t * total;
        for (size_t i = 1; i < points.size(); ++i) {
            const double len = dist(points[i - 1], points[i]);
            if (target <= len || i + 1 == points.size()) {
                const double u = len > 1e-12 ? target / len : 0.0;
                return { points[i - 1].x + (points[i].x - points[i - 1].x) * u,
                         points[i - 1].y + (points[i].y - points[i - 1].y) * u };
            }
            target -= len;
        }
        return points.back();
    }

    double relativePositionOnSegments(const std::vector<std::vector<Point>>& segments, const Point& point) {
        const std::vector<Point> points = flattenSegments(segments);
        const double total = polylineLength(points);
        if (points.size() < 2 || total <= 1e-12) return 0.0;
        double bestDist2 = std::numeric_limits<double>::max();
        double bestAlong = 0.0;
        double along = 0.0;
        for (size_t i = 1; i < points.size(); ++i) {
            const Point& a = points[i - 1];
            const Point& b = points[i];
            const double abx = b.x - a.x, aby = b.y - a.y;
            const double len2 = abx * abx + aby * aby;
            double u = len2 > 1e-12 ? ((point.x - a.x) * abx + (point.y - a.y) * aby) / len2 : 0.0;
            u = std::clamp(u, 0.0, 1.0);
            const double d2 = pointSegmentDist2(point.x, point.y, a.x, a.y, b.x, b.y);
            if (d2 < bestDist2) { bestDist2 = d2; bestAlong = along + std::sqrt(len2) * u; }
            along += std::sqrt(len2);
        }
        return std::clamp(bestAlong / total, 0.0, 1.0);
    }
}
