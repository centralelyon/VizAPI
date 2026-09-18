#include <algorithm>
#include <charconv>
#include <cmath>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <limits>

#include "core/shape.h"
#include "obs/region.h"
namespace {
    static constexpr double kSnapStep = 3.1415926535897932384 / 4.0;

    struct PointKey {
        long long x = 0;
        long long y = 0;
    };

    struct PointKeyHash {
        size_t operator()(const PointKey& k) const {
            return std::hash<long long>()(k.x) ^ (std::hash<long long>()(k.y) << 1);
        }
    };

    struct PointKeyEq {
        bool operator()(const PointKey& a, const PointKey& b) const {
            return a.x == b.x && a.y == b.y;
        }
    };

    struct SegmentKey {
        int a = -1;
        int b = -1;
    };

    struct SegmentKeyHash {
        size_t operator()(const SegmentKey& k) const {
            return std::hash<int>()(k.a) ^ (std::hash<int>()(k.b) << 1);
        }
    };

    struct SegmentKeyEq {
        bool operator()(const SegmentKey& a, const SegmentKey& b) const {
            return a.a == b.a && a.b == b.b;
        }
    };

    PointKey pointKey(const Point& p) {
        return {
            (long long)std::llround(p.x * 1e6),
            (long long)std::llround(p.y * 1e6)
        };
    }

    SegmentKey segmentKey(int a, int b) {
        if (a > b) std::swap(a, b);
        return { a, b };
    }

    bool isPseudoStationId(const std::string& stationId) {

        static constexpr const char* kPseudoPrefix = "pseudo_";
        return stationId.rfind(kPseudoPrefix, 0) == 0;
    }

    bool isPseudoStation(const Station& station) { return isPseudoStationId(station.station_id) || isPseudoStationId(station.id); }

    bool parseNumericId(const std::string& id, int& out) {

        if (id.empty()) return false;
        const char* begin = id.data();
        const char* end = id.data() + id.size();
        auto [ptr, ec] = std::from_chars(begin, end, out);
        return ec == std::errc() && ptr == end;
    }

    bool isObstacleRouteId(const std::string& id) {

        static constexpr const char* kObstaclePrefix = "obs";
        return id.rfind(kObstaclePrefix, 0) == 0;
    }

    bool isRegionRouteId(const std::string& id) {

        static constexpr const char* kRegionPrefix = "obsr_";
        return id.rfind(kRegionPrefix, 0) == 0;
    }

    double dist2(const Point& a, const Point& b) {

        const double dx = a.x - b.x;
        const double dy = a.y - b.y;
        return dx * dx + dy * dy;
    }

    double dist(const Point& a, const Point& b) { return std::sqrt(dist2(a, b)); }

    bool intersectInfiniteLines(const Point& p, const Point& r, const Point& q, const Point& s, Point& out, double& outT, double& outU) {

        const double cross = r.x * s.y - r.y * s.x;
        if (std::abs(cross) <= 1e-9) return false;

        const Point qp{ q.x - p.x, q.y - p.y };
        outT = (qp.x * s.y - qp.y * s.x) / cross;
        outU = (qp.x * r.y - qp.y * r.x) / cross;
        out = { p.x + r.x * outT, p.y + r.y * outT };
        return true;
    }

    double octilinearAngleDiffRad(const Point& a, const Point& b) {

        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        if (std::hypot(dx, dy) <= 1e-8) return 0.0;

        double angle = std::atan2(dy, dx);
        if (angle < 0.0) angle += 2.0 * 3.14159265358979323846;
        const double snapped = std::round(angle / kSnapStep) * kSnapStep;
        double diff = std::abs(angle - snapped);
        diff = std::min(diff, 2.0 * 3.14159265358979323846 - diff);
        return diff;
    }

    bool isSegmentOctilinear(const Point& a, const Point& b, double tolRad = 1e-2) {

        return octilinearAngleDiffRad(a, b) <= tolRad;
    }

    bool segmentIntersectsAnyRegionInteriorRobust(const Point& a, const Point& b, const std::vector<std::vector<Point>>& regionPolygons) {

        if (segmentIntersectsAnyRegionPolygonInterior(a, b, regionPolygons)) return true;

        constexpr double kSampleTs[] = { 0.2, 0.4, 0.6, 0.8 };
        for (const auto& polygon : regionPolygons) {
            for (double t : kSampleTs) {
                const Point sample{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
                if (Geometry::pointInPolygon(sample, polygon)) return true;
            }
        }
        return false;
    }

    bool computeRegionAvoidingOctilinearSplitPoint(const Point& a, const Point& b, const std::vector<std::vector<Point>>& regionPolygons, Point& outSplit) {

        const Point midpoint{ (a.x + b.x) * 0.5, (a.y + b.y) * 0.5 };
        outSplit = midpoint;
        if (regionPolygons.empty()) return false;

        std::vector<Point> octDirs;
        octDirs.reserve(8);
        for (int i = 0; i < 8; ++i) {
            const double angle = i * kSnapStep;
            octDirs.push_back({ std::cos(angle), std::sin(angle) });
        }

        bool found = false;
        Point best = midpoint;
        double bestScore = std::numeric_limits<double>::max();

        const double directDist = dist(a, b);
        constexpr double kMinLegLength = 1e-4;

        for (const Point& da : octDirs) {
            for (const Point& db : octDirs) {
                Point candidate{};
                double t = 0.0;
                double u = 0.0;
                if (!intersectInfiniteLines(a, da, b, db, candidate, t, u)) continue;
                if (t <= kMinLegLength || u <= kMinLegLength) continue;

                const double legA = dist(a, candidate);
                const double legB = dist(b, candidate);
                if (legA <= kMinLegLength || legB <= kMinLegLength) continue;

                if (segmentIntersectsAnyRegionInteriorRobust(a, candidate, regionPolygons)) continue;
                if (segmentIntersectsAnyRegionInteriorRobust(candidate, b, regionPolygons)) continue;

                const double detour = legA + legB - directDist;
                const double midpointBias = dist(midpoint, candidate);
                const double score = detour + midpointBias * 0.01;
                if (!found || score < bestScore) {
                    found = true;
                    bestScore = score;
                    best = candidate;
                }
            }
        }

        if (!found) return false;
        outSplit = best;
        return true;
    }

    std::vector<Point> generateStationMoveCandidates(const Shape& shape, int stationNodeIndex, const Point& anchor) {

        std::vector<Point> candidates;
        if (stationNodeIndex < 0 || stationNodeIndex >= (int)shape.nodes.size()) return candidates;
        if (!isStationLike(shape.nodes[stationNodeIndex].type)) return candidates;

        const Point origin = shape.nodes[stationNodeIndex].pos;
        candidates.push_back(origin);

        const double baseLen = std::max(10.0, dist(origin, anchor));
        const double radii[] = {
            std::max(8.0, baseLen * 0.15),
            std::max(14.0, baseLen * 0.30),
            std::max(20.0, baseLen * 0.45),
            std::max(28.0, baseLen * 0.60)
        };

        for (int di = 0; di < 8; ++di) {
            const double ang = di * kSnapStep;
            const Point dir{ std::cos(ang), std::sin(ang) };
            for (double r : radii) {
                Point c{ origin.x + dir.x * r, origin.y + dir.y * r };
                c = snapShapeStationPosition(shape, stationNodeIndex, c, 3.14159265358979323846);
                candidates.push_back(c);
            }
        }

        std::sort(candidates.begin(), candidates.end(), [](const Point& lhs, const Point& rhs) {
            if (std::abs(lhs.x - rhs.x) > 1e-6) return lhs.x < rhs.x;
            return lhs.y < rhs.y;
            });
        candidates.erase(std::unique(candidates.begin(), candidates.end(), [](const Point& lhs, const Point& rhs) {
            return Geometry::samePoint(lhs, rhs);
            }), candidates.end());
        return candidates;
    }

    bool movedStationsKeepLocalSegmentsValidAndOctilinear(
        const Shape& shape,
        int nodeA,
        int nodeB,
        const std::vector<std::vector<Point>>& regionPolygons,
        const std::vector<int>& movedNodes,
        int segmentIndex) {

        auto checkSegment = [&](int segIndex) {
            if (segIndex < 0 || segIndex >= (int)shape.segments.size()) return true;
            const auto& s = shape.segments[segIndex];
            if (s.a < 0 || s.b < 0) return true;
            if (s.a >= (int)shape.nodes.size() || s.b >= (int)shape.nodes.size()) return true;
            const Point& pa = shape.nodes[s.a].pos;
            const Point& pb = shape.nodes[s.b].pos;
            if (segmentIntersectsAnyRegionInteriorRobust(pa, pb, regionPolygons)) return false;
            if (!isSegmentOctilinear(pa, pb)) return false;
            return true;
            };

        if (!checkSegment(segmentIndex)) return false;

        for (int movedNode : movedNodes) {
            for (int si = 0; si < (int)shape.segments.size(); ++si) {
                const auto& s = shape.segments[si];
                if (s.a != movedNode && s.b != movedNode) continue;
                if (!checkSegment(si)) return false;
            }
        }

        (void)nodeA;
        (void)nodeB;
        return true;
    }

    bool tryResolveCrossingByMovingConnectedStations(Shape& shape, int segmentIndex, const std::vector<std::vector<Point>>& regionPolygons) {

        if (segmentIndex < 0 || segmentIndex >= (int)shape.segments.size()) return false;
        const auto seg = shape.segments[segmentIndex];
        if (seg.a < 0 || seg.b < 0) return false;
        if (seg.a >= (int)shape.nodes.size() || seg.b >= (int)shape.nodes.size()) return false;

        const bool movableA = isStationLike(shape.nodes[seg.a].type);
        const bool movableB = isStationLike(shape.nodes[seg.b].type);
        if (!movableA && !movableB) return false;

        const Point originalA = shape.nodes[seg.a].pos;
        const Point originalB = shape.nodes[seg.b].pos;

        const std::vector<Point> candidatesA = movableA ? generateStationMoveCandidates(shape, seg.a, originalB) : std::vector<Point>{ originalA };
        const std::vector<Point> candidatesB = movableB ? generateStationMoveCandidates(shape, seg.b, originalA) : std::vector<Point>{ originalB };

        bool found = false;
        Point bestA = originalA;
        Point bestB = originalB;
        double bestScore = std::numeric_limits<double>::max();

        std::vector<int> movedNodes;
        movedNodes.reserve(2);
        if (movableA) movedNodes.push_back(seg.a);
        if (movableB) movedNodes.push_back(seg.b);

        for (const Point& candA : candidatesA) {
            for (const Point& candB : candidatesB) {
                if (Geometry::samePoint(candA, originalA) && Geometry::samePoint(candB, originalB)) continue;

                shape.nodes[seg.a].pos = candA;
                shape.nodes[seg.b].pos = candB;

                if (!movedStationsKeepLocalSegmentsValidAndOctilinear(shape, seg.a, seg.b, regionPolygons, movedNodes, segmentIndex)) {
                    continue;
                }

                const double score = dist(originalA, candA) + dist(originalB, candB);
                if (!found || score < bestScore) {
                    found = true;
                    bestScore = score;
                    bestA = candA;
                    bestB = candB;
                }
            }
        }

        shape.nodes[seg.a].pos = originalA;
        shape.nodes[seg.b].pos = originalB;

        if (!found) return false;
        shape.nodes[seg.a].pos = bestA;
        shape.nodes[seg.b].pos = bestB;
        return true;
    }

    bool splitShapeSegmentAtPoint(Shape& shape, int segmentIndex, const Point& splitPos) {

        if (segmentIndex < 0 || segmentIndex >= (int)shape.segments.size()) return false;
        const ShapeSegment& originalSeg = shape.segments[segmentIndex];
        if (originalSeg.a < 0 || originalSeg.b < 0) return false;
        if (originalSeg.a >= (int)shape.nodes.size() || originalSeg.b >= (int)shape.nodes.size()) return false;

        auto findExistingNodeAtPoint = [&](const Point& p) -> int {
            for (int ni = 0; ni < (int)shape.nodes.size(); ++ni) {
                if (Geometry::samePoint(shape.nodes[ni].pos, p)) return ni;
            }
            return -1;
            };

        auto pointStrictlyInsideSegment = [&](const Point& p, int segIdx) -> bool {
            if (segIdx < 0 || segIdx >= (int)shape.segments.size()) return false;
            const auto& seg = shape.segments[segIdx];
            if (seg.a < 0 || seg.b < 0) return false;
            if (seg.a >= (int)shape.nodes.size() || seg.b >= (int)shape.nodes.size()) return false;

            const Point& sa = shape.nodes[seg.a].pos;
            const Point& sb = shape.nodes[seg.b].pos;
            if (Geometry::samePoint(p, sa) || Geometry::samePoint(p, sb)) return false;

            const double vx = sb.x - sa.x;
            const double vy = sb.y - sa.y;
            const double wx = p.x - sa.x;
            const double wy = p.y - sa.y;
            const double len2 = vx * vx + vy * vy;
            if (len2 <= 1e-12) return false;

            const double cross = vx * wy - vy * wx;
            const double scale = std::sqrt(len2);
            if (std::abs(cross) > scale * 1e-5) return false;

            const double t = (wx * vx + wy * vy) / len2;
            return t > 1e-6 && t < 1.0 - 1e-6;
            };

        auto splitSegmentUsingNode = [&](int targetSegIndex, int splitNodeIndex) -> bool {
            if (targetSegIndex < 0 || targetSegIndex >= (int)shape.segments.size()) return false;
            if (splitNodeIndex < 0 || splitNodeIndex >= (int)shape.nodes.size()) return false;

            const ShapeSegment segToSplit = shape.segments[targetSegIndex];
            if (segToSplit.a < 0 || segToSplit.b < 0) return false;
            if (segToSplit.a >= (int)shape.nodes.size() || segToSplit.b >= (int)shape.nodes.size()) return false;
            if (segToSplit.a == splitNodeIndex || segToSplit.b == splitNodeIndex) return false;

            shape.segments[targetSegIndex] = { segToSplit.a, splitNodeIndex };
            const int newSegIndex = (int)shape.segments.size();
            shape.segments.push_back({ splitNodeIndex, segToSplit.b });

            auto sharedNodeWithOriginal = [&](int otherSegIndex) -> int {
                if (otherSegIndex < 0 || otherSegIndex >= (int)shape.segments.size()) return -1;
                const auto& other = shape.segments[otherSegIndex];
                if (other.a == segToSplit.a || other.b == segToSplit.a) return segToSplit.a;
                if (other.a == segToSplit.b || other.b == segToSplit.b) return segToSplit.b;
                return -1;
                };

            for (auto& route : shape.routes) {
                for (size_t i = 0; i < route.segmentIndices.size(); ++i) {
                    if (route.segmentIndices[i] != targetSegIndex) continue;
                    int direction = 0; // 1: a->b, -1: b->a
                    if (i > 0) {
                        const int shared = sharedNodeWithOriginal(route.segmentIndices[i - 1]);
                        if (shared == segToSplit.a) direction = 1;
                        else if (shared == segToSplit.b) direction = -1;
                    }
                    if (direction == 0 && i + 1 < route.segmentIndices.size()) {
                        const int shared = sharedNodeWithOriginal(route.segmentIndices[i + 1]);
                        if (shared == segToSplit.a) direction = -1;
                        else if (shared == segToSplit.b) direction = 1;
                    }

                    if (direction >= 0) {
                        route.segmentIndices.insert(route.segmentIndices.begin() + static_cast<long>(i + 1), newSegIndex);
                        ++i;
                    }
                    else {
                        route.segmentIndices[i] = newSegIndex;
                        route.segmentIndices.insert(route.segmentIndices.begin() + static_cast<long>(i + 1), targetSegIndex);
                        ++i;
                    }
                }
            }
            return true;
            };

        int newNodeIndex = findExistingNodeAtPoint(splitPos);
        if (newNodeIndex < 0) {
            ShapeNode node;
            node.id = -1;
            node.type = ShapeNodeType::ShapePoint;
            node.name = "";
            node.station_id = "";
            node.pos = splitPos;
            newNodeIndex = (int)shape.nodes.size();
            shape.nodes.push_back(std::move(node));
        }

        if (!splitSegmentUsingNode(segmentIndex, newNodeIndex)) return false;

        const int segmentCountAfterPrimarySplit = (int)shape.segments.size();
        for (int si = 0; si < segmentCountAfterPrimarySplit; ++si) {
            if (si == segmentIndex) continue;
            if (!pointStrictlyInsideSegment(shape.nodes[newNodeIndex].pos, si)) continue;
            splitSegmentUsingNode(si, newNodeIndex);
        }

        return true;
    }

    double pointLineDistance(const Point& p, const Point& a, const Point& b) {

        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double len2 = dx * dx + dy * dy;
        if (len2 < 1e-12) return dist(p, a);
        const double t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
        const double px = a.x + t * dx;
        const double py = a.y + t * dy;
        return std::sqrt((p.x - px) * (p.x - px) + (p.y - py) * (p.y - py));
    }

    void rdpRecursive(const std::vector<Point>& pts, int start, int end, double eps, std::vector<char>& keep) {

        if (end <= start + 1) return;
        double maxDist = -1.0;
        int idx = -1;
        for (int i = start + 1; i < end; ++i) {
            double d = pointLineDistance(pts[i], pts[start], pts[end]);
            if (d > maxDist) {
                maxDist = d;
                idx = i;
            }
        }
        if (maxDist > eps && idx >= 0) {
            keep[idx] = 1;
            rdpRecursive(pts, start, idx, eps, keep);
            rdpRecursive(pts, idx, end, eps, keep);
        }
    }

    std::vector<Point> rdpSimplify(const std::vector<Point>& pts, double eps) {

        if (pts.size() < 3) return pts;
        std::vector<char> keep(pts.size(), 0);
        keep.front() = 1;
        keep.back() = 1;
        rdpRecursive(pts, 0, (int)pts.size() - 1, eps, keep);
        std::vector<Point> out;
        out.reserve(pts.size());
        for (size_t i = 0; i < pts.size(); ++i) {
            if (keep[i]) out.push_back(pts[i]);
        }
        return out;
    }

    int closestStationIndex(const std::vector<Station>& stations, const Point& p, double threshold) {

        const double threshold2 = threshold * threshold;
        int best = -1;
        double bestDist2 = threshold2;
        for (int i = 0; i < (int)stations.size(); ++i) {
            double d2 = dist2(stations[i].pos, p);
            if (d2 <= bestDist2) {
                best = i;
                bestDist2 = d2;
            }
        }
        return best;
    }

    std::vector<Point> removeNearStations(const std::vector<Point>& pts, const std::vector<Station>& stations, double snapDist) {

        if (pts.size() <= 2) return pts;
        std::vector<Point> out;
        out.reserve(pts.size());
        out.push_back(pts.front());
        for (size_t i = 1; i + 1 < pts.size(); ++i) {
            int sidx = closestStationIndex(stations, pts[i], snapDist);
            if (sidx >= 0) {
                continue;
            }
            out.push_back(pts[i]);
        }
        out.push_back(pts.back());
        return out;
    }

    std::vector<Point> mergeTurnCluster(const std::vector<Point>& pts, double mergeTurnDist) {

        if (pts.size() < 3) return pts;
        std::vector<Point> out;
        out.reserve(pts.size());
        out.push_back(pts.front());
        size_t i = 1;
        while (i + 1 < pts.size()) {
            size_t clusterStart = i;
            size_t clusterEnd = i;
            while (clusterEnd + 1 < pts.size() &&
                dist(pts[clusterEnd], pts[clusterEnd + 1]) < mergeTurnDist) {
                ++clusterEnd;
            }
            if (clusterEnd > clusterStart) {
                out.push_back(pts[(clusterStart + clusterEnd) / 2]);
                i = clusterEnd + 1;
            }
            else {
                out.push_back(pts[i]);
                ++i;
            }
        }
        if (dist(pts.back(), out.back()) > 1e-9) {
            out.push_back(pts.back());
        }
        return out;
    }

    std::vector<Point> simplifySegment(const std::vector<Point>& seg, const std::vector<Station>& stations, double snapDist, double mergeDist, double rdpEps, double mergeTurnDist) {

        if (seg.size() < 2) return seg;

        std::vector<Point> snapped;
        snapped.reserve(seg.size());

        for (const auto& p : seg) {
            int sidx = closestStationIndex(stations, p, snapDist);
            Point out = (sidx >= 0) ? stations[sidx].pos : p;

            if (!snapped.empty() && dist(out, snapped.back()) <= mergeDist) {
                continue;
            }
            snapped.push_back(out);
        }

        if (snapped.size() < 2) return snapped;

        std::vector<Point> simplified = rdpSimplify(snapped, rdpEps);
        if (simplified.size() < 2) return simplified;

        std::vector<Point> stationPruned = removeNearStations(simplified, stations, snapDist);
        if (stationPruned.size() < 2) return stationPruned;

        return mergeTurnCluster(stationPruned, mergeTurnDist);
    }
    /* oclinear */
    //double octilinearAngleDiff(const Point& from, const Point& to) {

    //    const double dx = to.x - from.x;
    //    const double dy = to.y - from.y;
    //    if (dx * dx + dy * dy <= 1e-12) return 0.0;
    //    double angle = std::atan2(dy, dx);
    //    if (angle < 0.0) angle += 2.0 * 3.1415926535897932384;
    //    const double snapped = std::round(angle / kSnapStep) * kSnapStep;
    //    double diff = std::abs(angle - snapped);
    //    return std::min(diff, 2.0 * 3.1415926535897932384 - diff);
    //}

    Point polygonCentroid(const std::vector<Point>& polygon) {

        Point c{ 0.0, 0.0 };
        if (polygon.empty()) return c;
        for (const auto& p : polygon) {
            c.x += p.x;
            c.y += p.y;
        }
        c.x /= (double)polygon.size();
        c.y /= (double)polygon.size();
        return c;
    }

    Point resolveAffectedCandidateNodePosition(
        const Point& currentPos,
        const Point& baselinePos,
        //const std::vector<Point>& neighborPositions,
        const std::vector<std::vector<Point>>& polygons) {

        if (polygons.empty()) return currentPos;
        //std::vector<Point> candidates;
        //candidates.reserve(256);

        //Point dirs[8];
        //for (int i = 0; i < 8; ++i) {
        //    const double a = i * kSnapStep;
        //    dirs[i] = { std::cos(a), std::sin(a) };
        //}

        //constexpr double kStep = 0.5;
        //constexpr int kMaxSteps = 2000;
        //for (const Point& nb : neighborPositions) {
        //    for (const Point& dir : dirs) {
        //        for (int step = 1; step <= kMaxSteps; ++step) {
        //            Point cand{ nb.x + dir.x * kStep * step, nb.y + dir.y * kStep * step };
        //            if (pointInsideAnyRegionPolygon(cand, polygons)) continue;
        //            if (segmentIntersectsAnyRegionPolygonInterior(nb, cand, polygons)) continue;
        //            candidates.push_back(cand);
        //            break;
        //        }
        //    }
        //}

        //candidates.push_back(baselinePos);
        //if (!neighborPositions.empty()) {
        //    candidates.push_back(movePointOutsideRegionPolygonsAlongDirection(currentPos, polygons, { baselinePos.x - currentPos.x, baselinePos.y - currentPos.y }));
        //}

        if (!pointInsideAnyRegionPolygon(currentPos, polygons)) return currentPos;
        const Point stableDir{ baselinePos.x - currentPos.x, baselinePos.y - currentPos.y };
        return movePointOutsideRegionPolygonsAlongDirection(currentPos, polygons, stableDir);

        /* best score */
        //Point best = currentPos;
        //double bestScore = std::numeric_limits<double>::max();
        //for (const auto& cand : candidates) {
        //    if (pointInsideAnyRegionPolygon(cand, polygons)) continue;

        //    bool blocked = false;
        //    double totalLen = 0.0;
        //    double octPenalty = 0.0;
        //    for (const Point& nb : neighborPositions) {
        //        if (segmentIntersectsAnyRegionPolygonInterior(nb, cand, polygons)) {
        //            blocked = true;
        //            break;
        //        }
        //        const double dx = cand.x - nb.x;
        //        const double dy = cand.y - nb.y;
        //        totalLen += std::sqrt(dx * dx + dy * dy);
        //        octPenalty += octilinearAngleDiff(nb, cand);
        //    }
        //    if (blocked) continue;

        //    const double moveDx = cand.x - baselinePos.x;
        //    const double moveDy = cand.y - baselinePos.y;
        //    const double movePenalty = std::sqrt(moveDx * moveDx + moveDy * moveDy);
        //    const double score = totalLen + octPenalty * 500.0 + movePenalty * 0.2;
        //    if (score < bestScore) {
        //        bestScore = score;
        //        best = cand;
        //    }
        //}
        //return best;
    }
} // namespace

std::unordered_map<std::string, int> buildShapeNodeIndexByKey(const GeoData& geo, const Shape& shape, const std::vector<int>& shapeStationNodeById) {

    std::unordered_map<std::string, int> out;
    out.reserve(geo.stations.size());
    for (size_t i = 0; i < geo.stations.size(); ++i) {
        if (i >= shapeStationNodeById.size()) continue;
        const int nodeIndex = shapeStationNodeById[i];
        if (nodeIndex < 0 || nodeIndex >= (int)shape.nodes.size()) continue;
        const std::string key = Geometry::stationKey(geo.stations[i]);
        if (key.empty()) continue;
        out[key] = nodeIndex;
    }
    return out;
}

void preserveObstacleLineStyles(const Shape& sourceShape, GeoData& targetData) {

    struct RouteStyle {
        float color[3];
        float width;
    };

    std::unordered_map<std::string, RouteStyle> obstacleStyleById;
    obstacleStyleById.reserve(sourceShape.routes.size());

    for (const auto& route : sourceShape.routes) {
        const bool isLineObstacle = route.isObstacle
            && (route.obstacleKind == ObstacleKind::Line || route.id.rfind("obs_", 0) == 0);
        if (!isLineObstacle || route.id.empty()) continue;

        RouteStyle style{};
        style.color[0] = route.color[0];
        style.color[1] = route.color[1];
        style.color[2] = route.color[2];
        style.width = route.route_width;
        obstacleStyleById[route.id] = style;
    }

    for (auto& route : targetData.routes) {
        auto it = obstacleStyleById.find(route.id);
        if (it == obstacleStyleById.end()) continue;

        route.color[0] = it->second.color[0];
        route.color[1] = it->second.color[1];
        route.color[2] = it->second.color[2];
        route.route_width = it->second.width;
    }
}

bool buildRouteLoopNodeIndices(const Shape& shape, const ShapeRoute& route, std::vector<int>& outNodeLoop) {

    outNodeLoop.clear();
    if (route.segmentIndices.empty()) return false;

    bool initialized = false;
    int currentNode = -1;
    for (int segIndex : route.segmentIndices) {
        if (segIndex < 0 || segIndex >= (int)shape.segments.size()) return false;
        const auto& seg = shape.segments[segIndex];
        if (seg.a < 0 || seg.b < 0 || seg.a >= (int)shape.nodes.size() || seg.b >= (int)shape.nodes.size()) return false;

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

std::string stableShapeNodeKey(const ShapeNode& node, int nodeIndex) {

    if (!node.station_id.empty()) return node.station_id;
    if (node.id >= 0) return std::to_string(node.id);
    return "node_" + std::to_string(nodeIndex);
}

Point snapPointToOctilinearFromAnchor(const Point& anchor, const Point& p) {

    const double dx = p.x - anchor.x;
    const double dy = p.y - anchor.y;
    const double len = std::sqrt(dx * dx + dy * dy);
    if (len <= 1e-8) return p;

    double angle = std::atan2(dy, dx);
    if (angle < 0.0) angle += 2.0 * 3.1415926535897932384;
    const double snapped = std::round(angle / kSnapStep) * kSnapStep;
    return { anchor.x + len * std::cos(snapped), anchor.y + len * std::sin(snapped) };
}

Point snapShapeStationPosition(const Shape& shape, int nodeIndex, const Point& proposed, double angleThresholdRad) {

    if (nodeIndex < 0 || nodeIndex >= (int)shape.nodes.size()) return proposed;
    if (shape.nodes[nodeIndex].type != ShapeNodeType::Station) return proposed;

    std::vector<char> segmentInAdj(shape.segments.size(), 0);
    for (const auto& route : shape.routes) {
        if (route.isObstacle) continue;
        for (int segIndex : route.segmentIndices) {
            if (segIndex < 0 || segIndex >= (int)shape.segments.size()) continue;
            segmentInAdj[segIndex] = 1;
        }
    }

    std::vector<int> neighbors;
    for (size_t segIndex = 0; segIndex < shape.segments.size(); ++segIndex) {
        if (!segmentInAdj[segIndex]) continue;
        const auto& seg = shape.segments[segIndex];
        if (seg.a == nodeIndex) neighbors.push_back(seg.b);
        else if (seg.b == nodeIndex) neighbors.push_back(seg.a);
    }
    if (neighbors.empty()) return proposed;
    std::sort(neighbors.begin(), neighbors.end());
    neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());

    Point bestPos = proposed;
    double bestDiff = angleThresholdRad;
    bool found = false;
    for (int neighbor : neighbors) {
        if (neighbor < 0 || neighbor >= (int)shape.nodes.size()) continue;
        const Point anchor = shape.nodes[neighbor].pos;
        const double dx = proposed.x - anchor.x;
        const double dy = proposed.y - anchor.y;
        const double len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-6) continue;
        double angle = std::atan2(dy, dx);
        if (angle < 0.0) angle += 2.0 * 3.1415926535897932384;
        const double snapped = std::round(angle / kSnapStep) * kSnapStep;
        double diff = std::abs(angle - snapped);
        diff = std::min(diff, 2.0 * 3.1415926535897932384 - diff);
        if (diff <= bestDiff) {
            bestDiff = diff;
            bestPos = { anchor.x + len * std::cos(snapped), anchor.y + len * std::sin(snapped) };
            found = true;
        }
    }

    return found ? bestPos : proposed;
}

bool resolveShapeRegionCollisions(Shape& shape, const Region& region, const std::unordered_map<int, Point>& baseline) {

    if (shape.nodes.empty() || region.segIdx.empty()) return false;

    std::vector<std::vector<Point>> regionPolygons;
    if (!buildRegionPolygons(region, regionPolygons)) return false;

    bool anyMoved = false;
    std::unordered_set<int> affectedCandidates;
   //std::unordered_set<int> affectedSegments;
   // std::vector<int> shapeNodesInsideRegion;

    for (int nodeIndex = 0; nodeIndex < (int)shape.nodes.size(); ++nodeIndex) {
        const auto& node = shape.nodes[nodeIndex];
        Point currentPos = node.pos;
        for (const auto& polygon : regionPolygons) {
            if (!Geometry::pointInPolygon(currentPos, polygon)) continue;
            //if (!isStationLike(node.type)) {
            //   // shapeNodesInsideRegion.push_back(nodeIndex);
            //    break;
            //}

            Point preferredDir{ 0.0, 0.0 };
            auto it = baseline.find(nodeIndex);
            if (it != baseline.end()) {
                preferredDir = { it->second.x - currentPos.x, it->second.y - currentPos.y };
            }
            if (std::abs(preferredDir.x) <= 1e-8 && std::abs(preferredDir.y) <= 1e-8) {
                const Point centroid = polygonCentroid(polygon);
                preferredDir = { currentPos.x - centroid.x, currentPos.y - centroid.y };
            }

            const Point resolved = movePointOutsideRegionPolygonsAlongDirection(currentPos, regionPolygons, preferredDir);
            if (!Geometry::samePoint(currentPos, resolved)) {
                shape.nodes[nodeIndex].pos = resolved;
                currentPos = resolved;
                anyMoved = true;
                //affectedCandidates.insert(nodeIndex);
                if (isStationLike(node.type)) {
                    affectedCandidates.insert(nodeIndex);
                }
            }
        }
    }

    //for (int segIdx = 0; segIdx < (int)shape.segments.size(); ++segIdx) {
    //    const auto& seg = shape.segments[segIdx];
    for (const auto& seg : shape.segments) {
        if (affectedCandidates.count(seg.a) || affectedCandidates.count(seg.b)) {
            //affectedSegments.insert(segIdx);
            if (seg.a >= 0 && seg.a < (int)shape.nodes.size() && isStationLike(shape.nodes[seg.a].type)) affectedCandidates.insert(seg.a);
            if (seg.b >= 0 && seg.b < (int)shape.nodes.size() && isStationLike(shape.nodes[seg.b].type)) affectedCandidates.insert(seg.b);
        }
    }

    for (int nodeIndex : affectedCandidates) {
        if (nodeIndex < 0 || nodeIndex >= (int)shape.nodes.size()) continue;
        if (!isStationLike(shape.nodes[nodeIndex].type)) continue;

        //std::vector<Point> neighbors;
        //for (int segIdx : affectedSegments) {
        //    if (segIdx < 0 || segIdx >= (int)shape.segments.size()) continue;
        //    const auto& seg = shape.segments[segIdx];
        //    int other = -1;
        //    if (seg.a == nodeIndex) other = seg.b;
        //    else if (seg.b == nodeIndex) other = seg.a;
        //    if (other < 0 || other >= (int)shape.nodes.size()) continue;
        //    neighbors.push_back(shape.nodes[other].pos);
        //}
        //if (neighbors.empty()) continue;

        Point baselinePos = shape.nodes[nodeIndex].pos;
        auto it = baseline.find(nodeIndex);
        if (it != baseline.end()) baselinePos = it->second;

        //Point optimized = resolveAffectedCandidateNodePosition(shape.nodes[nodeIndex].pos, baselinePos, neighbors, regionPolygons);
        //optimized = snapShapeStationPosition(shape, nodeIndex, optimized, 3.1415926535897932384);
        const Point optimized = resolveAffectedCandidateNodePosition(shape.nodes[nodeIndex].pos, baselinePos, regionPolygons);
        if (!Geometry::samePoint(optimized, shape.nodes[nodeIndex].pos)) {
            shape.nodes[nodeIndex].pos = optimized;
            anyMoved = true;
        }
    }

    //if (!shapeNodesInsideRegion.empty()) {
    //    std::sort(shapeNodesInsideRegion.begin(), shapeNodesInsideRegion.end());
    //    shapeNodesInsideRegion.erase(std::unique(shapeNodesInsideRegion.begin(), shapeNodesInsideRegion.end()), shapeNodesInsideRegion.end());
    //    if (deleteShapeNodes(shape, shapeNodesInsideRegion)) {
    //        anyMoved = true;
    //    }
    //}

    return anyMoved;
}

Shape geoData2Shape(const GeoData& data) {

    Shape shape;
    shape.nodes.reserve(data.stations.size());

    // pointIndex --> node index in shape.nodes
    std::unordered_map<PointKey, int, PointKeyHash, PointKeyEq> pointIndex;

    // stationPointIndex --> node index (NOT station index)
    std::unordered_map<PointKey, int, PointKeyHash, PointKeyEq> stationPointIndex;

    pointIndex.reserve(data.stations.size());
    stationPointIndex.reserve(data.stations.size());

    auto stationStableId = [&](int si) -> std::string {
        if (si < 0 || si >= (int)data.stations.size()) return "";
        if (!data.stations[si].station_id.empty()) return data.stations[si].station_id;
        return data.stations[si].id;
        };

    for (int si = 0; si < (int)data.stations.size(); ++si) {
        const PointKey key = pointKey(data.stations[si].pos);
        auto existing = pointIndex.find(key);
        if (existing != pointIndex.end()) {
            ShapeNode& node = shape.nodes[existing->second];
            const bool incomingPseudo = isPseudoStationId(data.stations[si].station_id)
                || isPseudoStationId(data.stations[si].id);
            if (incomingPseudo && node.type == ShapeNodeType::Station) {
                node.type = ShapeNodeType::PseudoStation;
                node.name = data.stations[si].name;
                node.station_id = stationStableId(si);
            }
            continue;
        }
        ShapeNode n;
        n.id = si; // stationIndex
        n.type = (isPseudoStationId(data.stations[si].station_id)
            || isPseudoStationId(data.stations[si].id))
            ? ShapeNodeType::PseudoStation
            : ShapeNodeType::Station;
        n.name = data.stations[si].name;
        n.station_id = stationStableId(si);
        n.pos = data.stations[si].pos;

        const int nodeIdx = (int)shape.nodes.size();
        shape.nodes.push_back(std::move(n));

        pointIndex[key] = nodeIdx;
        stationPointIndex[key] = nodeIdx;
    }

    std::unordered_map<SegmentKey, int, SegmentKeyHash, SegmentKeyEq> segmentIndex;

    std::vector<double> spacingSamples;
    for (const auto& route : data.routes) {
        for (const auto& seg : route.segments) {
            for (size_t i = 1; i < seg.size(); ++i) {
                spacingSamples.push_back(dist(seg[i - 1], seg[i]));
            }
        }
    }

    double baseSpacing = 1.0;
    if (!spacingSamples.empty()) {
        std::sort(spacingSamples.begin(), spacingSamples.end());
        baseSpacing = spacingSamples[spacingSamples.size() / 2];
        if (baseSpacing < 1e-6) baseSpacing = 1.0;
    }

    const double snapDist = baseSpacing * 10;
    const double mergeDist = baseSpacing * 0.1;
    const double rdpEps = baseSpacing * 0.25;
    const double mergeTurnDist = baseSpacing * 20;

    std::vector<int> pseudoRouteIds;
    pseudoRouteIds.reserve(data.routes.size());
    for (const auto& route : data.routes) {
        bool hasPseudoStation = false;
        for (int idx : route.stationIndices) {
            if (idx < 0 || idx >= (int)data.stations.size()) continue;
            if (isPseudoStation(data.stations[idx])) {
                hasPseudoStation = true;
                break;
            }
        }
        if (!hasPseudoStation) continue;

        int numericId = 0;
        if (parseNumericId(route.id, numericId)) {
            pseudoRouteIds.push_back(numericId);
        }
    }

    std::unordered_set<int> obstacleRouteIds;
    if (!pseudoRouteIds.empty()) {
        std::sort(pseudoRouteIds.begin(), pseudoRouteIds.end());
        pseudoRouteIds.erase(std::unique(pseudoRouteIds.begin(), pseudoRouteIds.end()), pseudoRouteIds.end());
        int expectedId = pseudoRouteIds.front();
        for (int id : pseudoRouteIds) {
            if (id != expectedId) break;
            obstacleRouteIds.insert(id);
            ++expectedId;
        }
    }

    for (const auto& route : data.routes) {
        ShapeRoute out;
        out.name = route.name.empty() ? route.id : route.name;
        out.id = route.id;
        out.color[0] = route.color[0];
        out.color[1] = route.color[1];
        out.color[2] = route.color[2];
        out.route_width = route.route_width;
        {
            bool hasPseudoStation = false;
            for (int idx : route.stationIndices) {
                if (idx < 0 || idx >= (int)data.stations.size()) continue;
                if (isPseudoStation(data.stations[idx])) {
                    hasPseudoStation = true;
                    break;
                }
            }
            int numericId = 0;
            out.isObstacle = isObstacleRouteId(route.id)
               /* || (hasPseudoStation
                    && parseNumericId(route.id, numericId)
                    && obstacleRouteIds.count(numericId) > 0)*/;
            if (out.isObstacle) {
                out.obstacleKind = isRegionRouteId(route.id)
                    ? ObstacleKind::Region
                    : ObstacleKind::Line;
            }
        }
        for (const auto& seg : route.segments) {
            if (seg.size() < 2) continue;

            std::vector<Point> cleanSeg =
                simplifySegment(seg, data.stations, snapDist, mergeDist, rdpEps, mergeTurnDist);
            if (cleanSeg.size() < 2) continue;

            int prevNodeIndex = -1;

            for (const auto& p : cleanSeg) {
                const PointKey pk = pointKey(p);

                int curNodeIndex = -1;

                auto stIt = stationPointIndex.find(pk);
                if (stIt != stationPointIndex.end()) {
                    curNodeIndex = stIt->second;
                }
                else {

                    auto it = pointIndex.find(pk);
                    if (it != pointIndex.end()) {
                        curNodeIndex = it->second;
                    }
                    else {
                        ShapeNode n;
                        n.id = -1;
                        n.type = ShapeNodeType::ShapePoint;
                        n.name = "";
                        n.station_id = "";
                        n.pos = p;

                        curNodeIndex = (int)shape.nodes.size();
                        shape.nodes.push_back(std::move(n));
                        pointIndex[pk] = curNodeIndex;
                    }
                }

                // Create segment
                if (prevNodeIndex >= 0 && curNodeIndex != prevNodeIndex) {
                    SegmentKey sk = segmentKey(prevNodeIndex, curNodeIndex);
                    auto segIt = segmentIndex.find(sk);
                    if (segIt == segmentIndex.end()) {
                        int idx = (int)shape.segments.size();
                        shape.segments.push_back({ sk.a, sk.b });
                        segmentIndex[sk] = idx;
                        segIt = segmentIndex.find(sk);
                    }
                    out.segmentIndices.push_back(segIt->second);
                }

                prevNodeIndex = curNodeIndex;
            }
        }

        shape.routes.push_back(std::move(out));
    }

    /* test station_id */
    //for (int i = 0; i < 5; ++i) {
    //     std::cout << "name: " << shape.nodes[i].name << std::endl;
    //     std::cout << "station_id: " << shape.nodes[i].station_id << std::endl;
    //}

    return shape;
}

/////////////////////////////////////////////////////////////////////////////bugs here
StyledShape Shape2StyleShape(const Shape& shape) {

    StyledShape styled;

    // Default StyledShape values are set here so future style-transfer defaults
    // can be changed in one place.
    styled.background_color[0] = 1.0f;
    styled.background_color[1] = 1.0f;
    styled.background_color[2] = 1.0f;
    styled.background_color[3] = 1.0f;
    constexpr float kDefaultRouteSpacing = 7.0f;
    styled.normalStationShape = StationShapeStyle{};
    styled.transferStationShape = StationShapeStyle{};
    styled.transferStationShape = RoundedRectangleStationStyle{ 12.0f, 12.0f, 3.0f };

    const int routeCount = static_cast<int>(shape.routes.size());
    styled.routes_geometry_vertices.resize(routeCount);
    styled.route_path_topology.resize(routeCount);
    styled.routes_width.resize(routeCount, 6.0f);
    styled.routes_colors.resize(routeCount, { 0.0f, 0.0f, 0.0f });
    styled.routes_is_obstacle.resize(routeCount, 0);
    styled.routes_spacing.assign(routeCount, std::vector<float>(routeCount, kDefaultRouteSpacing));
    for (int i = 0; i < routeCount; ++i) styled.routes_spacing[i][i] = 0.0f;

    //testing: default bend style for 90-degree
    //StyledShape::BendStyle default90;
    //default90.enabled = true;
    //default90.angleCos = 0.0f;
    //default90.angleCosTolerance = 0.03f;
    //default90.radiusX = 12.0f;
    //default90.radiusY = 12.0f;
    //styled.routes_bends.push_back(default90);

    std::unordered_map<int, int> globalSegmentUseCount;
    for (const auto& route : shape.routes) {
        for (int segIndex : route.segmentIndices) {
            if (segIndex < 0 || segIndex >= static_cast<int>(shape.segments.size())) continue;
            const ShapeSegment& seg = shape.segments[segIndex];
            if (seg.a < 0 || seg.b < 0 || seg.a >= static_cast<int>(shape.nodes.size()) || seg.b >= static_cast<int>(shape.nodes.size())) continue;
            globalSegmentUseCount[segIndex]++;
        }
    }

    struct RouteEdge {
        int globalSegmentIndex = -1;
        int a = -1;
        int b = -1;
        bool consumed = false;
    };
    struct ExtractedPath {
        std::vector<int> nodes;
        std::vector<int> globalSegments;
    };

    auto warn = [](const std::string& routeId, const std::string& message) {
        std::cerr << "[Shape2StyleShape] route '" << routeId << "': " << message << std::endl;
        };

    auto pointNearlyEqual = [](const Point& a, const Point& b) {
        return dist2(a, b) <= 1e-16;
        };

    auto collinearNoReverse = [&](int nodeA, int nodeB, int nodeC) {
        const Point& a = shape.nodes[nodeA].pos;
        const Point& b = shape.nodes[nodeB].pos;
        const Point& c = shape.nodes[nodeC].pos;
        const double abx = b.x - a.x;
        const double aby = b.y - a.y;
        const double bcx = c.x - b.x;
        const double bcy = c.y - b.y;
        const double abLen = std::sqrt(abx * abx + aby * aby);
        const double bcLen = std::sqrt(bcx * bcx + bcy * bcy);
        if (abLen <= 1e-9 || bcLen <= 1e-9) return false;
        const double cross = abx * bcy - aby * bcx;
        const double scale = std::max(1.0, abLen * bcLen);
        if (std::abs(cross) > 1e-8 * scale) return false;
        const double dot = abx * bcx + aby * bcy;
        return dot > 0.0;
        };

    auto segmentSharedStatus = [&](int globalSegmentIndex) {
        auto it = globalSegmentUseCount.find(globalSegmentIndex);
        return it != globalSegmentUseCount.end() && it->second > 1;
        };

    std::unordered_map<int, std::vector<StyledShape::StyledRouteSegmentRef>> refsByGlobalSegment;
    std::unordered_map<int, std::vector<int>> stationRoutesByNode;

    for (int ri = 0; ri < routeCount; ++ri) {
        const ShapeRoute& route = shape.routes[ri];
        styled.routes_width[ri] = route.route_width;
        styled.routes_colors[ri] = { route.color[0], route.color[1], route.color[2] };
        styled.routes_is_obstacle[ri] = route.isObstacle ? 1 : 0;

        std::vector<RouteEdge> edges;
        edges.reserve(route.segmentIndices.size());
        std::unordered_map<int, std::vector<int>> adjacency;
        std::unordered_set<int> seenSegments;
        if (route.orderedNodes.empty()) for (int segIndex : route.segmentIndices) {
            if (segIndex < 0 || segIndex >= static_cast<int>(shape.segments.size())) {
                warn(route.id, "invalid segment index " + std::to_string(segIndex));
                continue;
            }
            if (!seenSegments.insert(segIndex).second) {
                warn(route.id, "duplicate segment index " + std::to_string(segIndex));
                continue;
            }
            const ShapeSegment& seg = shape.segments[segIndex];
            if (seg.a < 0 || seg.b < 0 || seg.a >= static_cast<int>(shape.nodes.size()) || seg.b >= static_cast<int>(shape.nodes.size())) {
                warn(route.id, "segment " + std::to_string(segIndex) + " has invalid node indices");
                continue;
            }
            if (seg.a == seg.b || pointNearlyEqual(shape.nodes[seg.a].pos, shape.nodes[seg.b].pos)) {
                warn(route.id, "segment " + std::to_string(segIndex) + " is zero-length");
                continue;
            }
            const int edgeIndex = static_cast<int>(edges.size());
            edges.push_back({ segIndex, seg.a, seg.b, false });
            adjacency[seg.a].push_back(edgeIndex);
            adjacency[seg.b].push_back(edgeIndex);
            if (isStationLike(shape.nodes[seg.a].type)) stationRoutesByNode[seg.a].push_back(ri);
            if (isStationLike(shape.nodes[seg.b].type)) stationRoutesByNode[seg.b].push_back(ri);
        }

        for (auto& entry : adjacency) {
            std::sort(entry.second.begin(), entry.second.end(), [&](int lhs, int rhs) {
                const RouteEdge& a = edges[lhs];
                const RouteEdge& b = edges[rhs];
                if (a.globalSegmentIndex != b.globalSegmentIndex) return a.globalSegmentIndex < b.globalSegmentIndex;
                return lhs < rhs;
                });
        }

        auto unconsumedDegree = [&](int node) {
            int degree = 0;
            auto it = adjacency.find(node);
            if (it == adjacency.end()) return degree;
            for (int edgeIndex : it->second) if (!edges[edgeIndex].consumed) ++degree;
            return degree;
            };

        auto chooseStartNode = [&]() {
            std::vector<int> candidates;
            for (const auto& edge : edges) {
                if (edge.consumed) continue;
                candidates.push_back(edge.a);
                candidates.push_back(edge.b);
            }
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
            for (int node : candidates) if (unconsumedDegree(node) == 1) return node;
            for (int node : candidates) if (unconsumedDegree(node) > 2) return node;
            return candidates.empty() ? -1 : candidates.front();
            };

        auto chooseNextEdge = [&](int node, int previousNode) {
            auto it = adjacency.find(node);
            if (it == adjacency.end()) return -1;
            int fallback = -1;
            for (int edgeIndex : it->second) {
                if (edges[edgeIndex].consumed) continue;
                const int other = edges[edgeIndex].a == node ? edges[edgeIndex].b : edges[edgeIndex].a;
                if (fallback < 0) fallback = edgeIndex;
                if (other != previousNode) return edgeIndex;
            }
            return fallback;
            };

        std::vector<ExtractedPath> extractedPaths;
        if (!route.orderedNodes.empty()) extractedPaths.push_back({route.orderedNodes, route.segmentIndices});
        int consumedCount = 0;
        while (consumedCount < static_cast<int>(edges.size())) {
            int startNode = chooseStartNode();
            if (startNode < 0) break;

            ExtractedPath path;
            path.nodes.push_back(startNode);
            int current = startNode;
            int previous = -1;
            while (true) {
                const int edgeIndex = chooseNextEdge(current, previous);
                if (edgeIndex < 0) break;
                RouteEdge& edge = edges[edgeIndex];
                edge.consumed = true;
                ++consumedCount;
                const int next = edge.a == current ? edge.b : edge.a;
                path.globalSegments.push_back(edge.globalSegmentIndex);
                path.nodes.push_back(next);
                previous = current;
                current = next;

                const int degree = unconsumedDegree(current);
                if (degree == 0) break;
                if (current == startNode) break;
                if (degree != 1) break;
            }

            if (path.nodes.size() >= 2 && path.globalSegments.size() + 1 == path.nodes.size()) {
                extractedPaths.push_back(std::move(path));
            }
            else {
                warn(route.id, "discarded invalid extracted path from node " + std::to_string(startNode));
            }
        }

        if (consumedCount != static_cast<int>(edges.size())) {
            for (const auto& edge : edges) {
                if (!edge.consumed) warn(route.id, "unconsumed segment index " + std::to_string(edge.globalSegmentIndex));
            }
        }

        auto shouldKeepNode = [&](const ExtractedPath& path, int chainIndex) {
            if (!route.orderedNodes.empty()) return true;
            if (chainIndex <= 0 || chainIndex >= static_cast<int>(path.nodes.size()) - 1) return true;
            const int node = path.nodes[chainIndex];
            if (unconsumedDegree(node) > 0) return true;
            const int originalDegree = static_cast<int>(adjacency[node].size());
            if (originalDegree != 2) return true;
            const int prevSeg = path.globalSegments[chainIndex - 1];
            const int nextSeg = path.globalSegments[chainIndex];
            if (segmentSharedStatus(prevSeg) != segmentSharedStatus(nextSeg)) return true;
            return !collinearNoReverse(path.nodes[chainIndex - 1], node, path.nodes[chainIndex + 1]);
            };

        for (const ExtractedPath& path : extractedPaths) {
            std::vector<int> keptChainPositions;
            keptChainPositions.reserve(path.nodes.size());
            for (int i = 0; i < static_cast<int>(path.nodes.size()); ++i) {
                if (shouldKeepNode(path, i)) keptChainPositions.push_back(i);
            }
            if (keptChainPositions.size() < 2) {
                warn(route.id, "path collapsed below two geometry vertices");
                continue;
            }

            const int pathIndex = static_cast<int>(styled.routes_geometry_vertices[ri].size());
            std::vector<Point> geometryPath;
            geometryPath.reserve(keptChainPositions.size());
            for (int chainPos : keptChainPositions) geometryPath.push_back(shape.nodes[path.nodes[chainPos]].pos);

            for (size_t pi = 1; pi < geometryPath.size(); ++pi) {
                if (pointNearlyEqual(geometryPath[pi - 1], geometryPath[pi])) {
                    warn(route.id, "path " + std::to_string(pathIndex) + " contains consecutive duplicate geometry points");
                }
            }

            for (int segPos = 0; segPos < static_cast<int>(path.globalSegments.size()); ++segPos) {
                const int chainA = segPos;
                const int chainB = segPos + 1;
                int styledSegmentStart = -1;
                for (int k = 1; k < static_cast<int>(keptChainPositions.size()); ++k) {
                    if (keptChainPositions[k - 1] <= chainA && chainB <= keptChainPositions[k]) {
                        styledSegmentStart = k - 1;
                        break;
                    }
                }
                if (styledSegmentStart < 0) {
                    warn(route.id, "path " + std::to_string(pathIndex) + " cannot map segment " + std::to_string(path.globalSegments[segPos]));
                    continue;
                }
                if (segmentSharedStatus(path.globalSegments[segPos])) {
                    refsByGlobalSegment[path.globalSegments[segPos]].push_back({ ri, pathIndex, styledSegmentStart, styledSegmentStart + 1 });
                }
            }

            styled.routes_geometry_vertices[ri].push_back(std::move(geometryPath));
            styled.route_path_topology[ri].push_back({path.nodes, path.globalSegments});
        }

    }

    for (const auto& entry : refsByGlobalSegment) {
        if (entry.second.size() > 1) styled.shared_indices.push_back(entry.second);
    }

    // Explicit visits use their own arc-length occurrence, including return visits
    // to an earlier coordinate. Nearest-point matching would choose the first visit.
    for (int ri=0;ri<routeCount;++ri) {
        const auto& nodes=shape.routes[ri].orderedNodes;
        double total=0,along=0;
        for(size_t i=1;i<nodes.size();++i) total+=std::sqrt(dist2(shape.nodes[nodes[i-1]].pos,shape.nodes[nodes[i]].pos));
        for(size_t i=0;i<nodes.size();++i) {
            const auto& n=shape.nodes[nodes[i]];
            if(i)along+=std::sqrt(dist2(shape.nodes[nodes[i-1]].pos,n.pos));
            if(!isStationLike(n.type))continue;
            StyledShape::NormalStation station;station.id=std::to_string(n.id);station.station_id=n.station_id;
            station.name=n.name;station.pos=n.pos;station.routeIndex=ri;station.pathIndex=0;
            station.pathT=total>0?along/total:0;styled.normal_stations.push_back(station);
        }
    }
    for (auto& entry : stationRoutesByNode) {
        auto& routes = entry.second;
        std::sort(routes.begin(), routes.end());
        routes.erase(std::unique(routes.begin(), routes.end()), routes.end());
        const int nodeIndex = entry.first;
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(shape.nodes.size())) continue;
        const ShapeNode& node = shape.nodes[nodeIndex];
        if (routes.size() > 1) {
            StyledShape::TransferStation station;
            station.id = std::to_string(node.id);
            station.station_id = node.station_id;
            station.name = node.name;
            station.pos = node.pos;
            for (const int routeIndex : routes) {
                double bestDistance2 = std::numeric_limits<double>::infinity();
                StyledShape::TransferStation::Position position;
                position.routeIndex = routeIndex;
                for (int pathIndex = 0; pathIndex < static_cast<int>(styled.routes_geometry_vertices[routeIndex].size()); ++pathIndex) {
                    const auto& path = styled.routes_geometry_vertices[routeIndex][pathIndex];
                    double totalLength = 0.0;
                    for (size_t i = 1; i < path.size(); ++i) totalLength += std::sqrt(dist2(path[i - 1], path[i]));
                    if (path.size() < 2 || totalLength <= 1e-9) continue;
                    double lengthBefore = 0.0;
                    for (size_t i = 1; i < path.size(); ++i) {
                        const Point a = path[i - 1], b = path[i];
                        const double dx = b.x - a.x, dy = b.y - a.y;
                        const double segmentLength2 = dx * dx + dy * dy;
                        if (segmentLength2 <= 1e-18) continue;
                        const double segmentLength = std::sqrt(segmentLength2);
                        const double t = std::clamp(((node.pos.x - a.x) * dx + (node.pos.y - a.y) * dy) / segmentLength2, 0.0, 1.0);
                        const Point projected{ a.x + t * dx, a.y + t * dy };
                        const double d2 = dist2(node.pos, projected);
                        if (d2 < bestDistance2) {
                            bestDistance2 = d2;
                            position.pathIndex = pathIndex;
                            position.pathT = static_cast<float>(std::clamp((lengthBefore + t * segmentLength) / totalLength, 0.0, 1.0));
                        }
                        lengthBefore += segmentLength;
                    }
                }
                constexpr double kStationRouteMatchEpsilon = 1e-5;
                if (position.pathIndex < 0 || bestDistance2 > kStationRouteMatchEpsilon * kStationRouteMatchEpsilon) {
                    std::cerr << "[Shape2StyleShape] transfer station '" << station.id
                        << "' could not be matched to route " << routeIndex << " within epsilon\n";
                }
                else station.positions.push_back(position);
            }
            styled.transfer_stations.push_back(std::move(station));
        }
        else if (!routes.empty()) {
            StyledShape::NormalStation station;
            station.id = std::to_string(node.id);
            station.station_id = node.station_id;
            station.name = node.name;
            station.pos = node.pos;
            station.routeIndex = routes.front();
            double bestDistance2 = std::numeric_limits<double>::infinity();
            for (int pathIndex = 0; pathIndex < static_cast<int>(styled.routes_geometry_vertices[station.routeIndex].size()); ++pathIndex) {
                const auto& path = styled.routes_geometry_vertices[station.routeIndex][pathIndex];
                double totalLength = 0.0;
                for (size_t i = 1; i < path.size(); ++i) totalLength += std::sqrt(dist2(path[i - 1], path[i]));
                if (path.size() < 2 || totalLength <= 1e-9) continue;
                double lengthBefore = 0.0;
                for (size_t i = 1; i < path.size(); ++i) {
                    const Point a = path[i - 1], b = path[i];
                    const double dx = b.x - a.x, dy = b.y - a.y;
                    const double segmentLength2 = dx * dx + dy * dy;
                    if (segmentLength2 <= 1e-18) continue;
                    const double segmentLength = std::sqrt(segmentLength2);
                    const double t = std::clamp(((node.pos.x - a.x) * dx + (node.pos.y - a.y) * dy) / segmentLength2, 0.0, 1.0);
                    const Point projected{ a.x + t * dx, a.y + t * dy };
                    const double d2 = dist2(node.pos, projected);
                    if (d2 < bestDistance2) {
                        bestDistance2 = d2;
                        station.pathIndex = pathIndex;
                        station.pathT = static_cast<float>(std::clamp((lengthBefore + t * segmentLength) / totalLength, 0.0, 1.0));
                    }
                    lengthBefore += segmentLength;
                }
            }
            constexpr double kStationRouteMatchEpsilon = 1e-5;
            if (station.pathIndex < 0 || bestDistance2 > kStationRouteMatchEpsilon * kStationRouteMatchEpsilon) {
                std::cerr << "[Shape2StyleShape] normal station '" << station.id
                    << "' could not be matched to route " << station.routeIndex << " within epsilon\n";
            }
            styled.normal_stations.push_back(std::move(station));
        }
    }


    //debug:
    //std::cerr
    //    << "[Shape2StyleShape] route count = "
    //    << styled.routes_geometry_vertices.size()
    //    << ", shared bundle count = "
    //    << styled.shared_indices.size()
    //    << std::endl;

    //for (size_t bi = 0; bi < styled.shared_indices.size(); ++bi) {
    //    std::cerr
    //        << "  bundle " << bi
    //        << ": refs = "
    //        << styled.shared_indices[bi].size()
    //        << std::endl;

    //    for (const auto& ref : styled.shared_indices[bi]) {
    //        std::cerr
    //            << "    route=" << ref.routeIndex
    //            << " path=" << ref.pathIndex
    //            << " points=" << ref.pointIndexA
    //            << "->" << ref.pointIndexB
    //            << std::endl;
    //    }
    //}
    return styled;
}

//Shape StyleShape2Shape(const StyledShape& styledShape) {
//
//    Shape shape;
//    std::unordered_map<PointKey, int, PointKeyHash, PointKeyEq> stationNodeByPoint;
//
//    auto addStationNode = [&](const std::string& id, const std::string& stationId, const std::string& name, const Point& pos) {
//        const PointKey key = pointKey(pos);
//        auto existing = stationNodeByPoint.find(key);
//        if (existing != stationNodeByPoint.end()) {
//            ShapeNode& node = shape.nodes[existing->second];
//            if (node.name.empty()) node.name = name;
//            if (node.station_id.empty()) node.station_id = stationId;
//            return existing->second;
//        }
//
//        ShapeNode node;
//        int numericId = -1;
//        node.id = parseNumericId(id, numericId) ? numericId : static_cast<int>(shape.nodes.size());
//        node.type = ShapeNodeType::Station;
//        node.name = name;
//        node.station_id = stationId;
//        node.pos = pos;
//        const int nodeIndex = static_cast<int>(shape.nodes.size());
//        shape.nodes.push_back(std::move(node));
//        stationNodeByPoint[key] = nodeIndex;
//        return nodeIndex;
//        };
//
//    for (const auto& station : styledShape.normal_stations) {
//        addStationNode(station.id, station.station_id, station.name, station.pos);
//    }
//    for (const auto& station : styledShape.transfer_stations) {
//        addStationNode(station.id, station.station_id, station.name, station.pos);
//    }
//
//    for (size_t ri = 0; ri < styledShape.routes_geometry_vertices.size(); ++ri) {
//        ShapeRoute route;
//        route.id = std::to_string(ri);
//        route.name = route.id;
//        if (ri < styledShape.routes_width.size()) route.route_width = styledShape.routes_width[ri];
//        if (ri < styledShape.routes_colors.size()) {
//            route.color[0] = styledShape.routes_colors[ri][0];
//            route.color[1] = styledShape.routes_colors[ri][1];
//            route.color[2] = styledShape.routes_colors[ri][2];
//        }
//
//        for (const auto& vertices : styledShape.routes_geometry_vertices[ri]) {
//            int prevNode = -1;
//            for (const Point& pnt : vertices) {
//                int curNode = -1;
//                const PointKey key = pointKey(pnt);
//                auto stationIt = stationNodeByPoint.find(key);
//                if (stationIt != stationNodeByPoint.end()) {
//                    curNode = stationIt->second;
//                }
//                else {
//                    ShapeNode node;
//                    node.id = -1;
//                    node.type = ShapeNodeType::ShapePoint;
//                    node.pos = pnt;
//                    curNode = static_cast<int>(shape.nodes.size());
//                    shape.nodes.push_back(std::move(node));
//                }
//
//                if (prevNode >= 0 && prevNode != curNode) {
//                    route.segmentIndices.push_back(static_cast<int>(shape.segments.size()));
//                    shape.segments.push_back({ prevNode, curNode });
//                }
//                prevNode = curNode;
//            }
//        }
//        shape.routes.push_back(std::move(route));
//    }
//    return shape;
//}
/////////////////////////////////////////////////////////////////////////////bugs here

//Shape StyleShape2Shape(const StyledShape& styledShape) {
//
//    Shape shape;
//    for (const auto& station : styledShape.normal_stations) {
//        ShapeNode node;
//        node.id = static_cast<int>(shape.nodes.size());
//        node.type = ShapeNodeType::Station;
//        node.name = station.name;
//        node.station_id = station.station_id;
//        node.pos = station.pos;
//        shape.nodes.push_back(std::move(node));
//    }
//    for (const auto& station : styledShape.transfer_stations) {
//        ShapeNode node;
//        node.id = static_cast<int>(shape.nodes.size());
//        node.type = ShapeNodeType::Station;
//        node.name = station.name;
//        node.station_id = station.station_id;
//        node.pos = station.pos;
//        shape.nodes.push_back(std::move(node));
//    }
//
//    for (size_t ri = 0; ri < styledShape.routes_geometry_vertices.size(); ++ri) {
//        ShapeRoute route;
//        route.id = std::to_string(ri);
//        route.name = route.id;
//        if (ri < styledShape.routes_width.size()) route.route_width = styledShape.routes_width[ri];
//        if (ri < styledShape.routes_colors.size()) {
//            route.color[0] = styledShape.routes_colors[ri][0];
//            route.color[1] = styledShape.routes_colors[ri][1];
//            route.color[2] = styledShape.routes_colors[ri][2];
//        }
//        const auto& vertices = styledShape.routes_geometry_vertices[ri];
//        int prevNode = -1;
//        for (const Point& pnt : vertices) {
//            ShapeNode node;
//            node.id = -1;
//            node.type = ShapeNodeType::ShapePoint;
//            node.pos = pnt;
//            const int curNode = static_cast<int>(shape.nodes.size());
//            shape.nodes.push_back(std::move(node));
//            if (prevNode >= 0) {
//                route.segmentIndices.push_back(static_cast<int>(shape.segments.size()));
//                shape.segments.push_back({ prevNode, curNode });
//            }
//            prevNode = curNode;
//        }
//        shape.routes.push_back(std::move(route));
//    }
//    return shape;
//}

GeoData getGeoDataFromShape(const Shape& shape) {

    GeoData out;

    int maxStationId = -1;
    for (const auto& node : shape.nodes) {
        if (node.type == ShapeNodeType::Station && node.id >= 0) {
            maxStationId = std::max(maxStationId, node.id);
        }
    }
    if (maxStationId >= 0) {
        out.stations.assign(maxStationId + 1, Station{});
    }

    for (const auto& node : shape.nodes) {
        if (node.type != ShapeNodeType::Station || node.id < 0) continue;
        if (node.id >= (int)out.stations.size()) continue;
        Station s;
        s.id = std::to_string(node.id);
        s.name = node.name;
        s.station_id = node.station_id;
        s.pos = node.pos;
        out.stations[node.id] = std::move(s);
    }

    out.routes.reserve(shape.routes.size());

    for (const auto& route : shape.routes) {
        Route converted;
        converted.name = route.name;
        converted.id = route.id;
        converted.route_width = route.route_width;

        converted.color[0] = route.color[0];
        converted.color[1] = route.color[1];
        converted.color[2] = route.color[2];

        for (int segIndex : route.segmentIndices) {
            if (segIndex < 0 || segIndex >= (int)shape.segments.size()) continue;
            const auto& seg = shape.segments[segIndex];
            if (seg.a < 0 || seg.b < 0) continue;
            if (seg.a >= (int)shape.nodes.size() || seg.b >= (int)shape.nodes.size()) continue;
            converted.segments.push_back({ shape.nodes[seg.a].pos, shape.nodes[seg.b].pos });

            auto addStationOnce = [&](int nodeIndex) {
                const auto& node = shape.nodes[nodeIndex];
                if (node.type != ShapeNodeType::Station || node.id < 0) return;
                if (std::find(converted.stationIndices.begin(), converted.stationIndices.end(), node.id) == converted.stationIndices.end()) {
                    converted.stationIndices.push_back(node.id);
                }
                };
            addStationOnce(seg.a);
            addStationOnce(seg.b);
        }

        out.routes.push_back(std::move(converted));
    }
    return out;
}

// adj graph
struct RouteGraph {
    std::unordered_map<int, std::vector<int>> adj; // node --> neighbors
    std::unordered_map<int, int> deg;
    std::unordered_set<long long> edgeSet; // undirected edge set for quick visited
};

static long long edgeId(int a, int b) {

    if (a > b) std::swap(a, b);
    return ((long long)a << 32) ^ (unsigned long long)b;
}

static bool buildRouteGraph(const Shape& s, const ShapeRoute& r, RouteGraph& g) {

    g.adj.clear();
    g.deg.clear();
    g.edgeSet.clear();

    for (int segIdx : r.segmentIndices) {
        if (segIdx < 0 || segIdx >= (int)s.segments.size()) continue;
        const auto& e = s.segments[segIdx];
        int a = e.a, b = e.b;
        if (a < 0 || b < 0 || a == b) continue;

        g.adj[a].push_back(b);
        g.adj[b].push_back(a);
        g.deg[a]++; g.deg[b]++;
        g.edgeSet.insert(edgeId(a, b));
    }

    for (auto& kv : g.adj) {
        auto& v = kv.second;
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }

    return !g.edgeSet.empty();
}

static void extractChains(const RouteGraph& g, std::vector<std::vector<int>>& chains) {

    chains.clear();
    std::unordered_set<long long> visitedEdges;

    auto walkPath = [&](int start, int next) {
        std::vector<int> chain;
        chain.push_back(start);
        int prev = start;
        int cur = next;

        visitedEdges.insert(edgeId(prev, cur));
        chain.push_back(cur);

        while (true) {
            auto it = g.adj.find(cur);
            if (it == g.adj.end()) break;

            int pick = -1;
            for (int nb : it->second) {
                if (nb == prev) continue;
                long long eid = edgeId(cur, nb);
                if (g.edgeSet.count(eid) && !visitedEdges.count(eid)) {
                    pick = nb;
                    break;
                }
            }
            if (pick < 0) break;

            prev = cur;
            cur = pick;
            visitedEdges.insert(edgeId(prev, cur));
            chain.push_back(cur);
        }
        chains.push_back(std::move(chain));
        };

    // degree = 1 --> start
    for (const auto& kv : g.deg) {
        int node = kv.first;
        if (kv.second != 1) continue;

        auto it = g.adj.find(node);
        if (it == g.adj.end() || it->second.empty()) continue;

        int nb = it->second[0];
        long long eid = edgeId(node, nb);
        if (visitedEdges.count(eid)) continue;
        walkPath(node, nb);
    }

    for (long long eid : g.edgeSet) {
        if (visitedEdges.count(eid)) continue;

        int a = (int)(eid >> 32);
        int b = (int)(eid & 0xffffffffu);
        walkPath(a, b);
    }

    for (auto& c : chains) {
        c.erase(std::unique(c.begin(), c.end()), c.end());
    }
}

// ordered route.segmentIndices
static int addSegment(std::vector<ShapeSegment>& segs, std::unordered_map<SegmentKey, int, SegmentKeyHash, SegmentKeyEq>& segIndex, int a, int b) {

    if (a < 0 || b < 0 || a == b) return -1;
    SegmentKey k = segmentKey(a, b);
    auto it = segIndex.find(k);
    if (it != segIndex.end()) return it->second;

    int idx = (int)segs.size();
    segs.push_back({ k.a, k.b });
    segIndex[k] = idx;
    return idx;
}

// rebuild segments and segmentsIndices
static void rebuildSeg( Shape& s, const std::vector<std::vector<std::vector<int>>>& routeChainsMulti) {

    std::vector<ShapeSegment> newSegs;
    std::unordered_map<SegmentKey, int, SegmentKeyHash, SegmentKeyEq> segIndex;

    for (auto& r : s.routes) r.segmentIndices.clear();

    for (int ri = 0; ri < (int)s.routes.size(); ++ri) {
        auto& r = s.routes[ri];
        const auto& chains = routeChainsMulti[ri];

        for (const auto& chain : chains) {
            if (chain.size() < 2) continue;
            for (size_t i = 1; i < chain.size(); ++i) {
                int u = chain[i - 1];
                int v = chain[i];
                int segIdx = addSegment(newSegs, segIndex, u, v);
                if (segIdx >= 0) r.segmentIndices.push_back(segIdx);
            }
        }
    }
    s.segments.swap(newSegs);
}

//compress and remap the nodes
static void compressRemap( Shape& s, const std::vector<char>& delNode, std::vector<std::vector<std::vector<int>>>& routeChainsMulti) {

    std::vector<int> mapOldToNew(s.nodes.size(), -1);
    std::vector<ShapeNode> newNodes;
    newNodes.reserve(s.nodes.size());

    for (int i = 0; i < (int)s.nodes.size(); ++i) {
        if (delNode[i]) continue;
        mapOldToNew[i] = (int)newNodes.size();
        newNodes.push_back(s.nodes[i]);
    }

    for (auto& chains : routeChainsMulti) {
        for (auto& chain : chains) {
            for (int& n : chain) n = mapOldToNew[n];
            chain.erase(std::remove(chain.begin(), chain.end(), -1), chain.end());
            chain.erase(std::unique(chain.begin(), chain.end()), chain.end());
        }
        chains.erase(std::remove_if(chains.begin(), chains.end(),
            [](const std::vector<int>& c) { return c.size() < 2; }), chains.end());
    }

    s.nodes.swap(newNodes);
}

// route contains this node?
//static bool routeContainsNode(const Shape& s, const ShapeRoute& r, int nodeIdx) {
//
//    for (int segIdx : r.segmentIndices) {
//        if (segIdx < 0 || segIdx >= (int)s.segments.size()) continue;
//        const auto& e = s.segments[segIdx];
//        if (e.a == nodeIdx || e.b == nodeIdx) return true;
//    }
//    return false;
//}

/* shape editing*/
bool deleteShapeNodes(Shape& s, const std::vector<int>& nodesToDelete) {

    if (nodesToDelete.empty()) return true;

    std::vector<char> del(s.nodes.size(), 0);
    for (int idx : nodesToDelete) {
        if (0 <= idx && idx < (int)s.nodes.size()) del[idx] = 1;
    }

    // segmentIndices --> route graph -->extract chains
    std::vector<std::vector<std::vector<int>>> routeChainsMulti(s.routes.size());
    for (int ri = 0; ri < (int)s.routes.size(); ++ri) {
        RouteGraph g;
        if (!buildRouteGraph(s, s.routes[ri], g)) continue;

        std::vector<std::vector<int>> chains;
        extractChains(g, chains);

        // filter the chain
        for (auto& chain : chains) {
            std::vector<int> filtered;
            filtered.reserve(chain.size());
            for (int n : chain) {
                if (n < 0 || n >= (int)del.size()) continue;
                if (!del[n]) filtered.push_back(n);
            }
            filtered.erase(std::unique(filtered.begin(), filtered.end()), filtered.end());
            chain.swap(filtered);
        }
        chains.erase(std::remove_if(chains.begin(), chains.end(),
            [](const std::vector<int>& c) { return c.size() < 2; }), chains.end());

        routeChainsMulti[ri] = std::move(chains);
    }

    // compress nodes indices, rebuild the segments and segmentsIndices
    compressRemap(s, del, routeChainsMulti);
    rebuildSeg(s, routeChainsMulti);
    return true;
}

bool updateStationMeta(Shape& s, int stationNodeIdx, const std::string& newName, const std::string& newStationId) {

    if (stationNodeIdx < 0 || stationNodeIdx >= (int)s.nodes.size()) return false;
    auto& n = s.nodes[stationNodeIdx];
    if (!isStationLike(n.type)) return false;
    n.name = newName;
    n.station_id = newStationId;
    return true;
}

bool mergeStations(Shape& s, const std::vector<int>& stationNodeIndices, const std::string& mergedName, const std::string& mergedStationId) {

    std::vector<int> sts;
    for (int idx : stationNodeIndices) {
        if (0 <= idx && idx < (int)s.nodes.size() &&
            isStationLike(s.nodes[idx].type))
            sts.push_back(idx);
    }
    std::sort(sts.begin(), sts.end());
    sts.erase(std::unique(sts.begin(), sts.end()), sts.end());
    if (sts.size() < 2) return false;

    int keep = sts[0];

    std::vector<std::vector<std::vector<int>>> routeChainsMulti(s.routes.size());
    for (int ri = 0; ri < (int)s.routes.size(); ++ri) {
        RouteGraph g;
        if (!buildRouteGraph(s, s.routes[ri], g)) continue;

        std::vector<std::vector<int>> chains;
        extractChains(g, chains);

        // merge
        for (auto& chain : chains) {
            for (int& n : chain) {
                if (std::binary_search(sts.begin(), sts.end(), n)) n = keep;
            }
            chain.erase(std::unique(chain.begin(), chain.end()), chain.end());
        }
        chains.erase(std::remove_if(chains.begin(), chains.end(),
            [](const std::vector<int>& c) { return c.size() < 2; }), chains.end());

        routeChainsMulti[ri] = std::move(chains);
    }

    s.nodes[keep].name = mergedName;
    s.nodes[keep].station_id = mergedStationId;
    std::vector<char> del(s.nodes.size(), 0);
    for (size_t i = 1; i < sts.size(); ++i) del[sts[i]] = 1;

    // compress nodes indices, rebuild the segments and segmentsIndices
    compressRemap(s, del, routeChainsMulti);
    rebuildSeg(s, routeChainsMulti);

    return true;
}

bool splitStation(Shape& s, int stationNodeIdx, const std::string& nameA, const std::string& idA, const std::string& nameB, const std::string& idB, const std::vector<char>& routeToB, double offsetWorld) {

    if (stationNodeIdx < 0 || stationNodeIdx >= (int)s.nodes.size()) return false;
    if (!isStationLike(s.nodes[stationNodeIdx].type)) return false;
    if ((int)routeToB.size() != (int)s.routes.size()) return false;

    const ShapeNodeType originalType = s.nodes[stationNodeIdx].type;
    s.nodes[stationNodeIdx].name = nameA;
    s.nodes[stationNodeIdx].station_id = idA;

    // create nodes B
    ShapeNode nb = s.nodes[stationNodeIdx];
    nb.uid.clear();
    nb.name = nameB;
    int maxId = -1;
    for (const auto& n : s.nodes) maxId = std::max(maxId, n.id);
    nb.id = maxId + 1;
    nb.type = originalType;
    nb.station_id = idB;
    nb.pos.x += offsetWorld;
    int nodeB = (int)s.nodes.size();
    s.nodes.push_back(nb);

    // update chainsMulti
    std::vector<std::vector<std::vector<int>>> routeChainsMulti(s.routes.size());
    for (int ri = 0; ri < (int)s.routes.size(); ++ri) {
        RouteGraph g;
        if (!buildRouteGraph(s, s.routes[ri], g)) continue;

        std::vector<std::vector<int>> chains;
        extractChains(g, chains);

        if (routeToB[ri]) {
            for (auto& chain : chains) {
                for (int& n : chain) if (n == stationNodeIdx) n = nodeB;
                chain.erase(std::unique(chain.begin(), chain.end()), chain.end());
            }
        }

        chains.erase(std::remove_if(chains.begin(), chains.end(),
            [](const std::vector<int>& c) { return c.size() < 2; }), chains.end());

        routeChainsMulti[ri] = std::move(chains);
    }

    //rebuild segments and segmentsIndices
    rebuildSeg(s, routeChainsMulti);
    return true;
}

bool splitShapeSegmentAtMidpoint(Shape& shape, int segmentIndex) {

    if (segmentIndex < 0 || segmentIndex >= (int)shape.segments.size()) return false;
    const ShapeSegment originalSeg = shape.segments[segmentIndex];
    if (originalSeg.a < 0 || originalSeg.b < 0) return false;
    if (originalSeg.a >= (int)shape.nodes.size() || originalSeg.b >= (int)shape.nodes.size()) return false;

    const Point& a = shape.nodes[originalSeg.a].pos;
    const Point& b = shape.nodes[originalSeg.b].pos;
    const Point midpoint{ (a.x + b.x) * 0.5, (a.y + b.y) * 0.5 };
    return splitShapeSegmentAtPoint(shape, segmentIndex, midpoint);
}

int refineShapeSegmentsCrossingRegions(Shape& shape, const Region& region) {

    if (shape.nodes.empty() || shape.segments.empty()) return 0;
    if (region.segIdx.empty() || region.nodes.empty() || region.segments.empty()) return 0;

    std::vector<std::vector<Point>> regionPolygons;
    if (!buildRegionPolygons(region, regionPolygons) || regionPolygons.empty()) return 0;

    std::vector<int> intersectingSegments;
    const int initialSegmentCount = (int)shape.segments.size();
    intersectingSegments.reserve(initialSegmentCount);
    for (int segIndex = 0; segIndex < initialSegmentCount; ++segIndex) {
        const auto& seg = shape.segments[segIndex];
        if (seg.a < 0 || seg.b < 0) continue;
        if (seg.a >= (int)shape.nodes.size() || seg.b >= (int)shape.nodes.size()) continue;

        const Point& a = shape.nodes[seg.a].pos;
        const Point& b = shape.nodes[seg.b].pos;
        if (segmentIntersectsAnyRegionInteriorRobust(a, b, regionPolygons)) {
            intersectingSegments.push_back(segIndex);
        }
    }

    int refinedCount = 0;
    for (const int segIndex : intersectingSegments) {
        if (segIndex < 0 || segIndex >= (int)shape.segments.size()) continue;
        const auto& seg = shape.segments[segIndex];
        if (seg.a < 0 || seg.b < 0) continue;
        if (seg.a >= (int)shape.nodes.size() || seg.b >= (int)shape.nodes.size()) continue;

        const Point& a = shape.nodes[seg.a].pos;
        const Point& b = shape.nodes[seg.b].pos;
        if (!segmentIntersectsAnyRegionInteriorRobust(a, b, regionPolygons)) continue;

        if (tryResolveCrossingByMovingConnectedStations(shape, segIndex, regionPolygons)) {
            ++refinedCount;
            continue;
        }

        Point splitPos{};
        if (!computeRegionAvoidingOctilinearSplitPoint(a, b, regionPolygons, splitPos)) continue;
        if (segmentIntersectsAnyRegionInteriorRobust(a, splitPos, regionPolygons)) continue;
        if (segmentIntersectsAnyRegionInteriorRobust(splitPos, b, regionPolygons)) continue;

        if (splitShapeSegmentAtPoint(shape, segIndex, splitPos)) {
            ++refinedCount;
        }
    }

    return refinedCount;
}
