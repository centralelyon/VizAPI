#pragma once
// Headless port of Application::updateShapeFromRegionCollisionBaseline and its
// face/constraint helpers in the supplied desktop app/interaction.cpp.
#include "core/shape.h"
#include "obs/region.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>
namespace RegionDeformation {
static constexpr double kSnapStep = 3.14159265358979323846 / 4.0;
    struct FaceInfo {
        std::vector<int> nodeLoop;
        std::vector<Point> polygon;
        Point centroid{ 0.0, 0.0 };
        double area = 0.0;
        int activeRegionCount = 0;
        double activeRegionArea = 0.0;
        double scale = 1.0;
    };

    static double polygonAreaSigned(const std::vector<Point>& poly) {
        if (poly.size() < 3) return 0.0;
        double accum = 0.0;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            accum += (poly[j].x * poly[i].y - poly[i].x * poly[j].y);
        }
        return 0.5 * accum;
    }

    static Point polygonCentroid(const std::vector<Point>& poly) {
        Point c{ 0.0, 0.0 };
        if (poly.empty()) return c;
        for (const auto& p : poly) {
            c.x += p.x;
            c.y += p.y;
        }
        c.x /= (double)poly.size();
        c.y /= (double)poly.size();
        return c;
    }

    static std::vector<FaceInfo> buildRouteFaces(const Shape& shape) {
        std::vector<FaceInfo> faces;
        const int n = (int)shape.nodes.size();
        if (n <= 2) return faces;

        std::vector<std::vector<int>> adj(n);
        std::unordered_set<long long> seenEdge;
        for (const auto& route : shape.routes) {
            if (route.isObstacle) continue;
            for (int segIndex : route.segmentIndices) {
                if (segIndex < 0 || segIndex >= (int)shape.segments.size()) continue;
                const auto& seg = shape.segments[segIndex];
                if (seg.a < 0 || seg.b < 0 || seg.a >= n || seg.b >= n || seg.a == seg.b) continue;
                const int lo = std::min(seg.a, seg.b);
                const int hi = std::max(seg.a, seg.b);
                const long long key = ((long long)lo << 32) | (unsigned int)hi;
                if (seenEdge.count(key)) continue;
                seenEdge.insert(key);
                adj[seg.a].push_back(seg.b);
                adj[seg.b].push_back(seg.a);
            }
        }

        std::vector<std::unordered_map<int, int>> nextCw(n);
        for (int i = 0; i < n; ++i) {
            auto& nb = adj[i];
            if (nb.size() < 2) continue;
            std::sort(nb.begin(), nb.end(), [&](int lhs, int rhs) {
                const Point& p = shape.nodes[i].pos;
                const Point& a = shape.nodes[lhs].pos;
                const Point& b = shape.nodes[rhs].pos;
                return std::atan2(a.y - p.y, a.x - p.x) < std::atan2(b.y - p.y, b.x - p.x);
                });
            for (int k = 0; k < (int)nb.size(); ++k) {
                const int prev = nb[(k - 1 + (int)nb.size()) % (int)nb.size()];
                const int curr = nb[k];
                nextCw[i][curr] = prev;
            }
        }

        std::unordered_set<long long> usedHalfEdges;
        auto halfEdgeKey = [](int u, int v) -> long long {
            return ((long long)u << 32) | (unsigned int)v;
            };

        for (int u = 0; u < n; ++u) {
            for (int v : adj[u]) {
                const long long startKey = halfEdgeKey(u, v);
                if (usedHalfEdges.count(startKey)) continue;
                if (!nextCw[v].count(u)) continue;

                std::vector<int> faceNodes;
                int a = u;
                int b = v;
                bool closed = false;
                for (int guard = 0; guard < n * 8; ++guard) {
                    const long long hk = halfEdgeKey(a, b);
                    if (usedHalfEdges.count(hk)) break;
                    usedHalfEdges.insert(hk);
                    faceNodes.push_back(a);

                    auto it = nextCw[b].find(a);
                    if (it == nextCw[b].end()) break;
                    const int c = it->second;
                    a = b;
                    b = c;
                    if (a == u && b == v) {
                        closed = true;
                        break;
                    }
                }
                if (!closed || faceNodes.size() < 3) continue;

                FaceInfo face;
                face.nodeLoop = std::move(faceNodes);
                face.polygon.reserve(face.nodeLoop.size());
                for (int nodeIndex : face.nodeLoop) {
                    if (nodeIndex < 0 || nodeIndex >= n) continue;
                    face.polygon.push_back(shape.nodes[nodeIndex].pos);
                }
                if (face.polygon.size() < 3) continue;
                const double signedArea = polygonAreaSigned(face.polygon);
                face.area = std::abs(signedArea);
                if (face.area <= 1e-6) continue;
                if (signedArea < 0.0) continue; // skip exterior face

                face.centroid = polygonCentroid(face.polygon);
                faces.push_back(std::move(face));
            }
        }
        return faces;
    }

    struct DirectionConstraint {
        Point baseDelta{ 0.0, 0.0 };
        Point octiUnit{ 1.0, 0.0 };
        double length = 0.0;
    };

    static Point quantizedOctiUnit(const Point& delta) {
        const double len = std::hypot(delta.x, delta.y);
        if (len <= 1e-9) return { 1.0, 0.0 };
        const double angle = std::atan2(delta.y, delta.x);
        const double snapped = std::round(angle / kSnapStep) * kSnapStep;
        return { std::cos(snapped), std::sin(snapped) };
    }

    static void computeNodeBBox(const std::vector<Point>& pts, Point& outMin, Point& outMax) {
        outMin = { std::numeric_limits<double>::max(), std::numeric_limits<double>::max() };
        outMax = { std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest() };
        for (const auto& p : pts) {
            outMin.x = std::min(outMin.x, p.x);
            outMin.y = std::min(outMin.y, p.y);
            outMax.x = std::max(outMax.x, p.x);
            outMax.y = std::max(outMax.y, p.y);
        }
    }


    static void propagateOctilinearConstraints(
        Shape& shape,
        const std::unordered_map<long long, DirectionConstraint>& directionByEdge,
        const std::vector<std::vector<int>>& nodeAdj,
        const std::vector<char>& preferredNodes) {

        if (shape.nodes.empty()) return;
        const std::vector<ShapeNode> targets = shape.nodes;
        auto edgeKey = [](int a, int b) -> long long {
            const int lo = std::min(a, b);
            const int hi = std::max(a, b);
            return ((long long)lo << 32) | (unsigned int)hi;
            };



        constexpr int kRelaxIterations = 480;
        constexpr int kConstraintOnlyIterations = 160;
        for (int iter = 0; iter < kRelaxIterations + kConstraintOnlyIterations; ++iter) {
            const bool attractTargets = iter < kRelaxIterations;
            if (attractTargets) {
                for (int i = 0; i < (int)shape.nodes.size(); ++i) {
                    const double attraction = i < (int)preferredNodes.size() && preferredNodes[i] ? 0.025 : 0.004;
                    shape.nodes[i].pos.x += (targets[i].pos.x - shape.nodes[i].pos.x) * attraction;
                    shape.nodes[i].pos.y += (targets[i].pos.y - shape.nodes[i].pos.y) * attraction;
                }
            }

            for (int a = 0; a < (int)nodeAdj.size(); ++a) {
                for (int b : nodeAdj[a]) {
                    if (b <= a || b >= (int)shape.nodes.size()) continue;
                    const auto constraintIt = directionByEdge.find(edgeKey(a, b));
                    if (constraintIt == directionByEdge.end()) continue;

                    Point& pa = shape.nodes[a].pos;
                    Point& pb = shape.nodes[b].pos;
                    const Point& unit = constraintIt->second.octiUnit;

                    const Point normal{ -unit.y, unit.x };
                    const double error = (pb.x - pa.x) * normal.x + (pb.y - pa.y) * normal.y;
                    pa.x += normal.x * error * 0.5;
                    pa.y += normal.y * error * 0.5;
                    pb.x -= normal.x * error * 0.5;
                    pb.y -= normal.y * error * 0.5;
                }
            }
        }
    }
inline void apply(Shape& curShape, const Shape& baseline, const Region& curRegion) {
    if (baseline.nodes.empty() || baseline.segments.empty()) return;
    curShape = baseline;
    std::vector<std::vector<int>> nodeAdj(curShape.nodes.size());
    std::unordered_map<long long, DirectionConstraint> directionByEdge;
    auto edgeKey = [](int a, int b) -> long long {
        const int lo = std::min(a, b);
        const int hi = std::max(a, b);
        return ((long long)lo << 32) | (unsigned int)hi;
        };
    for (const auto& route : curShape.routes) {
        if (route.isObstacle && route.obstacleKind != ObstacleKind::Line) continue;
        for (int segIndex : route.segmentIndices) {
            if (segIndex < 0 || segIndex >= (int)curShape.segments.size()) continue;
            const auto& seg = curShape.segments[segIndex];
            if (seg.a < 0 || seg.b < 0) continue;
            if (seg.a >= (int)curShape.nodes.size() || seg.b >= (int)curShape.nodes.size()) continue;
            nodeAdj[seg.a].push_back(seg.b);
            nodeAdj[seg.b].push_back(seg.a);

            const Point& pa = baseline.nodes[seg.a].pos;
            const Point& pb = baseline.nodes[seg.b].pos;
            const Point d{ pb.x - pa.x, pb.y - pa.y };
            DirectionConstraint c;
            c.baseDelta = d;
            c.length = std::hypot(d.x, d.y);
            c.octiUnit = quantizedOctiUnit(seg.a < seg.b ? d : Point{-d.x,-d.y});
            directionByEdge[edgeKey(seg.a, seg.b)] = c;
        }
    }
    for (auto& neighbors : nodeAdj) {
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    }

    std::vector<std::vector<Point>> regionPolygons;
    buildRegionPolygons(curRegion, regionPolygons);

    std::vector<Point> baselinePts;
    baselinePts.reserve(baseline.nodes.size());
    for (const auto& node : baseline.nodes) baselinePts.push_back(node.pos);
    Point bboxMin, bboxMax;
    computeNodeBBox(baselinePts, bboxMin, bboxMax);
    const double bboxDiag = std::hypot(bboxMax.x - bboxMin.x, bboxMax.y - bboxMin.y);
    const double bboxTol = std::max(1e-6, bboxDiag * 1e-5);

    std::vector<FaceInfo> faces = buildRouteFaces(baseline);
    std::vector<char> solverFixed(curShape.nodes.size(), 0);
    std::vector<char> renderFixed(curShape.nodes.size(), 0);

    for (int i = 0; i < (int)curShape.nodes.size(); ++i) {
        const Point& p = baseline.nodes[i].pos;
        const bool onBoundary =
            std::abs(p.x - bboxMin.x) <= bboxTol ||
            std::abs(p.x - bboxMax.x) <= bboxTol ||
            std::abs(p.y - bboxMin.y) <= bboxTol ||
            std::abs(p.y - bboxMax.y) <= bboxTol;
        if (!onBoundary) continue;
        solverFixed[i] = 1;
    }

    // for each face with active regions, scale/translate contour and
    // fix station nodes that are redistributed along contour edges.
    std::vector<char> regionActive(regionPolygons.size(), 1);
    for (auto& face : faces) {
        std::vector<int> activeRegionIdx;
        for (size_t ri = 0; ri < regionPolygons.size(); ++ri) {
            if (!regionActive[ri]) continue;
            if (!Geometry::pointInPolygon(polygonCentroid(regionPolygons[ri]), face.polygon)) continue;
            activeRegionIdx.push_back((int)ri);
        }
        if (activeRegionIdx.empty()) continue;

        std::vector<Point> loopPts;
        loopPts.reserve(face.nodeLoop.size());
        for (int ni : face.nodeLoop) {
            if (ni < 0 || ni >= (int)curShape.nodes.size()) continue;
            loopPts.push_back(curShape.nodes[ni].pos);
        }
        if (loopPts.size() < 3) continue;

        std::vector<Point> activePts;
        for (int ri : activeRegionIdx) {
            for (const auto& p : regionPolygons[ri]) activePts.push_back(p);
        }
        if (activePts.empty()) continue;

        Point faceMin, faceMax, regionMin, regionMax;
        computeNodeBBox(loopPts, faceMin, faceMax);
        computeNodeBBox(activePts, regionMin, regionMax);


        const Point faceCenter{ 0.5 * (faceMin.x + faceMax.x), 0.5 * (faceMin.y + faceMax.y) };
        const double fw = std::max(1e-6, faceMax.x - faceMin.x);
        const double fh = std::max(1e-6, faceMax.y - faceMin.y);
        const double rw = std::max(1e-6, regionMax.x - regionMin.x);
        const double rh = std::max(1e-6, regionMax.y - regionMin.y);
        const double margin = 1.16;
        const double scale = std::clamp(std::max((rw * margin) / fw, (rh * margin) / fh), 0.60, 2.40);

        for (int ni : face.nodeLoop) {
            if (ni < 0 || ni >= (int)curShape.nodes.size()) continue;
            Point rel{
                curShape.nodes[ni].pos.x - faceCenter.x,
                curShape.nodes[ni].pos.y - faceCenter.y
            };
            curShape.nodes[ni].pos = { faceCenter.x + rel.x * scale, faceCenter.y + rel.y * scale };
        }

        // translate contour so it fully contains all active regions in that face.
        Point newFaceMin{ std::numeric_limits<double>::max(), std::numeric_limits<double>::max() };
        Point newFaceMax{ std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest() };
        for (int ni : face.nodeLoop) {
            if (ni < 0 || ni >= (int)curShape.nodes.size()) continue;
            const Point& p = curShape.nodes[ni].pos;
            newFaceMin.x = std::min(newFaceMin.x, p.x);
            newFaceMin.y = std::min(newFaceMin.y, p.y);
            newFaceMax.x = std::max(newFaceMax.x, p.x);
            newFaceMax.y = std::max(newFaceMax.y, p.y);
        }
        Point shift{ 0.0, 0.0 };
        if (newFaceMin.x > regionMin.x) shift.x -= (newFaceMin.x - regionMin.x);
        if (newFaceMax.x < regionMax.x) shift.x += (regionMax.x - newFaceMax.x);
        if (newFaceMin.y > regionMin.y) shift.y -= (newFaceMin.y - regionMin.y);
        if (newFaceMax.y < regionMax.y) shift.y += (regionMax.y - newFaceMax.y);
        for (int ni : face.nodeLoop) {
            if (ni < 0 || ni >= (int)curShape.nodes.size()) continue;
            curShape.nodes[ni].pos.x += shift.x;
            curShape.nodes[ni].pos.y += shift.y;
            curShape.nodes[ni].pos.x = std::clamp(curShape.nodes[ni].pos.x, bboxMin.x, bboxMax.x);
            curShape.nodes[ni].pos.y = std::clamp(curShape.nodes[ni].pos.y, bboxMin.y, bboxMax.y);
            if (isStationLike(curShape.nodes[ni].type)) {
                renderFixed[ni] = 1;
                solverFixed[ni] = 1;
            }
        }

        // Evenly redistribute stations on each contour edge and mark them fixed.
        const int loopN = (int)face.nodeLoop.size();
        for (int i = 0; i < loopN; ++i) {
            const int a = face.nodeLoop[i];
            const int b = face.nodeLoop[(i + 1) % loopN];
            if (a < 0 || b < 0 || a >= (int)curShape.nodes.size() || b >= (int)curShape.nodes.size()) continue;
            std::vector<int> stationChain;
            if (isStationLike(curShape.nodes[a].type)) stationChain.push_back(a);
            if (isStationLike(curShape.nodes[b].type) && b != a) stationChain.push_back(b);
            if (stationChain.size() < 2) continue;

            const Point pa = curShape.nodes[a].pos;
            const Point pb = curShape.nodes[b].pos;
            const int segCount = (int)stationChain.size() - 1;
            for (int k = 0; k < (int)stationChain.size(); ++k) {
                const double t = (segCount <= 0) ? 0.0 : (double)k / (double)segCount;
                const int si = stationChain[k];
                curShape.nodes[si].pos = { pa.x + (pb.x - pa.x) * t, pa.y + (pb.y - pa.y) * t };
                solverFixed[si] = 1;
                renderFixed[si] = 1;
            }
        }
    }

    // BFS from fixed nodes; keep historical edge direction (octilinear).
    std::queue<int> q;
    std::vector<char> visited(curShape.nodes.size(), 0);
    for (int i = 0; i < (int)curShape.nodes.size(); ++i) {
        if (!solverFixed[i]) continue;
        q.push(i);
        visited[i] = 1;
    }

    while (!q.empty()) {
        const int u = q.front();
        q.pop();
        for (int v : nodeAdj[u]) {
            if (v < 0 || v >= (int)curShape.nodes.size()) continue;
            const long long ek = edgeKey(u, v);
            auto dcIt = directionByEdge.find(ek);
            if (dcIt == directionByEdge.end()) continue;
            const DirectionConstraint& dc = dcIt->second;
            if (!solverFixed[v]) {
                const double signedDir = ((u < v) ? 1.0 : -1.0);
                const Point step{
                    dc.octiUnit.x * dc.length * signedDir,
                    dc.octiUnit.y * dc.length * signedDir
                };
                curShape.nodes[v].pos = {
                    std::clamp(curShape.nodes[u].pos.x + step.x, bboxMin.x, bboxMax.x),
                    std::clamp(curShape.nodes[u].pos.y + step.y, bboxMin.y, bboxMax.y)
                };
                solverFixed[v] = 1;
            }
            if (!visited[v]) {
                visited[v] = 1;
                q.push(v);
            }
        }
    }
    // Any disconnected component without seed: deterministic first node as anchor.
    for (int i = 0; i < (int)curShape.nodes.size(); ++i) {
        if (solverFixed[i]) continue;
        solverFixed[i] = 1;
        q.push(i);
        while (!q.empty()) {
            const int u = q.front();
            q.pop();
            for (int v : nodeAdj[u]) {
                if (v < 0 || v >= (int)curShape.nodes.size() || solverFixed[v]) continue;
                const long long ek = edgeKey(u, v);
                auto dcIt = directionByEdge.find(ek);
                if (dcIt == directionByEdge.end()) continue;
                const DirectionConstraint& dc = dcIt->second;
                const double signedDir = ((u < v) ? 1.0 : -1.0);
                const Point step{
                    dc.octiUnit.x * dc.length * signedDir,
                    dc.octiUnit.y * dc.length * signedDir
                };
                curShape.nodes[v].pos = {
                    std::clamp(curShape.nodes[u].pos.x + step.x, bboxMin.x, bboxMax.x),
                    std::clamp(curShape.nodes[u].pos.y + step.y, bboxMin.y, bboxMax.y)
                };
                solverFixed[v] = 1;
                q.push(v);
            }
        }
    }

    for (int i = 0; i < (int)curShape.nodes.size(); ++i) {
        const bool shouldPushOutside =
            isStationLike(curShape.nodes[i].type) ||
            curShape.nodes[i].type == ShapeNodeType::ShapePoint;
        if (!shouldPushOutside) continue;
        if (!pointInsideAnyRegionPolygon(curShape.nodes[i].pos, regionPolygons)) continue;
        Point preferredDir{
            curShape.nodes[i].pos.x - baseline.nodes[i].pos.x,
            curShape.nodes[i].pos.y - baseline.nodes[i].pos.y
        };
        curShape.nodes[i].pos = movePointOutsideRegionPolygonsAlongDirection(curShape.nodes[i].pos, regionPolygons, preferredDir);
        curShape.nodes[i].pos.x = std::clamp(curShape.nodes[i].pos.x, bboxMin.x, bboxMax.x);
        curShape.nodes[i].pos.y = std::clamp(curShape.nodes[i].pos.y, bboxMin.y, bboxMax.y);
    }

    auto countCrossingSegments = [&]() -> int {
        int crossing = 0;
        for (const auto& seg : curShape.segments) {
            if (seg.a < 0 || seg.b < 0) continue;
            if (seg.a >= (int)curShape.nodes.size() || seg.b >= (int)curShape.nodes.size()) continue;
            const Point& a = curShape.nodes[seg.a].pos;
            const Point& b = curShape.nodes[seg.b].pos;
            if (segmentIntersectsAnyRegionPolygonInterior(a, b, regionPolygons)) {
                ++crossing;
            }
        }
        return crossing;
        };

    for (int iter = 0; iter < 6; ++iter) {
        const int crossingBefore = countCrossingSegments();
        if (crossingBefore <= 0) break;

        const int splitCount = ::refineShapeSegmentsCrossingRegions(curShape, curRegion);

        for (int i = 0; i < (int)curShape.nodes.size(); ++i) {
            if (curShape.nodes[i].type != ShapeNodeType::ShapePoint) continue;
            if (!pointInsideAnyRegionPolygon(curShape.nodes[i].pos, regionPolygons)) continue;
            Point preferredDir{ 1.0, 0.0 };
            if (i < (int)baseline.nodes.size()) {
                preferredDir = {
                    curShape.nodes[i].pos.x - baseline.nodes[i].pos.x,
                    curShape.nodes[i].pos.y - baseline.nodes[i].pos.y
                };
            }
            curShape.nodes[i].pos = movePointOutsideRegionPolygonsAlongDirection(curShape.nodes[i].pos, regionPolygons, preferredDir);
            curShape.nodes[i].pos.x = std::clamp(curShape.nodes[i].pos.x, bboxMin.x, bboxMax.x);
            curShape.nodes[i].pos.y = std::clamp(curShape.nodes[i].pos.y, bboxMin.y, bboxMax.y);
        }

        const int crossingAfter = countCrossingSegments();
        if (splitCount <= 0 || crossingAfter >= crossingBefore) break;
    }

    std::vector<std::vector<int>> finalAdj(curShape.nodes.size());
    std::unordered_map<long long, DirectionConstraint> finalDirections;
    for (const auto& route : curShape.routes) {
        if (route.isObstacle && route.obstacleKind != ObstacleKind::Line) continue;
        for (int segIndex : route.segmentIndices) {
            if (segIndex < 0 || segIndex >= (int)curShape.segments.size()) continue;
            const auto& seg = curShape.segments[segIndex];
            if (seg.a < 0 || seg.b < 0 || seg.a >= (int)curShape.nodes.size() || seg.b >= (int)curShape.nodes.size()) continue;
            finalAdj[seg.a].push_back(seg.b);
            finalAdj[seg.b].push_back(seg.a);
            const Point delta{
                curShape.nodes[seg.b].pos.x - curShape.nodes[seg.a].pos.x,
                curShape.nodes[seg.b].pos.y - curShape.nodes[seg.a].pos.y
            };
            DirectionConstraint constraint;
            constraint.baseDelta = delta;
            constraint.length = std::hypot(delta.x, delta.y);
            constraint.octiUnit = quantizedOctiUnit(seg.a < seg.b ? delta : Point{-delta.x,-delta.y});
            finalDirections[edgeKey(seg.a, seg.b)] = constraint;
        }
    }
    for (auto& neighbors : finalAdj) {
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    }
    propagateOctilinearConstraints(curShape, finalDirections, finalAdj, renderFixed);


}
} // namespace RegionDeformation
