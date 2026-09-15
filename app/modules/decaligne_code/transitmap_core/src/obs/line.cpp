#include <algorithm>
#include <cmath>

#include "line.h"



static bool segmentIntersection(const Point& a, const Point& b, const Point& c, const Point& d, Point& out, double& outT) {

    const double rdx = b.x - a.x;
    const double rdy = b.y - a.y;
    const double sdx = d.x - c.x;
    const double sdy = d.y - c.y;
    const double denom = rdx * sdy - rdy * sdx;
    if (std::abs(denom) < 1e-12) return false;
    const double qpx = c.x - a.x;
    const double qpy = c.y - a.y;
    const double t = (qpx * sdy - qpy * sdx) / denom;
    const double u = (qpx * rdy - qpy * rdx) / denom;
    if (t < 0.0 || t > 1.0 || u < 0.0 || u > 1.0) return false;
    out = { a.x + t * rdx, a.y + t * rdy };
    outT = t;
    return true;

}

void Line::syncCounters(const Shape& curShape) {

    int nextNodeId = obstacleNextNodeId;
    int nextRouteId = obstacleNextRouteId;

    auto considerNode = [&](const ShapeNode& node) {
        int numericId = -1;
        if (Geometry::parsePrefixedId(node.station_id, "pseudo_", numericId)) {
            nextNodeId = std::max(nextNodeId, numericId + 1);
        }
    };

    for (const auto& node : curShape.nodes) {
        considerNode(node);
    }
    for (const auto& node : obstacleShape.nodes) {
        considerNode(node);
    }

    auto considerRoute = [&](const ShapeRoute& route) {
        int numericId = -1;
        if (Geometry::parsePrefixedId(route.id, "obsr_", numericId)
            || Geometry::parsePrefixedId(route.id, "obs_", numericId)
            || Geometry::parsePrefixedId(route.id, "obs", numericId)) {
            nextRouteId = std::max(nextRouteId, numericId + 1);
        }
    };

    for (const auto& route : curShape.routes) {
        considerRoute(route);
    }
    for (const auto& route : obstacleShape.routes) {
        considerRoute(route);
    }

    obstacleNextNodeId = nextNodeId;
    obstacleNextRouteId = nextRouteId;
}

//add a new node to the obstacle shape and return its index
int Line::addObstacleNode(const Point& pos) {

    ShapeNode node;
    node.id = obstacleNextNodeId++;
    node.type = ShapeNodeType::PseudoStation;
    node.name = "";
    node.station_id = "pseudo_" + std::to_string(node.id);
    node.pos = pos;
    int idx = (int)obstacleShape.nodes.size();
    obstacleShape.nodes.push_back(std::move(node));
    return idx;
}

void Line::addObstaclePoint(const Shape& curShape, const Point& pos, int existingNodeIndex) {

    if (obstacleDraftNodes.empty()) {
        int nodeIndex = existingNodeIndex;
        if (nodeIndex < 0 || nodeIndex >= (int)obstacleShape.nodes.size()) {
            nodeIndex = addObstacleNode(pos);
        }
        obstacleDraftNodes.push_back(nodeIndex);
        return;
    }

    const int startNode = obstacleDraftNodes.back();
    if (startNode < 0 || startNode >= (int)obstacleShape.nodes.size()) return;
    const Point startPos = obstacleShape.nodes[startNode].pos;

    int endNode = existingNodeIndex;
    Point endPos = pos;
    if (endNode >= 0 && endNode < (int)obstacleShape.nodes.size()) {
        endPos = obstacleShape.nodes[endNode].pos;
    }
    else {
        endNode = -1;
    }
    if (Geometry::samePoint(startPos, endPos)) return;

    if (endNode < 0) {
        endNode = addObstacleNode(endPos);
    }

    std::vector<std::pair<double, Point>> intersections;
    intersections.reserve(curShape.segments.size());

    const double endpointEps = 1e-4;
    for (const auto& seg : curShape.segments) {
        if (seg.a < 0 || seg.b < 0) continue;
        if (seg.a >= (int)curShape.nodes.size() || seg.b >= (int)curShape.nodes.size()) continue;
        const Point& a = curShape.nodes[seg.a].pos;
        const Point& b = curShape.nodes[seg.b].pos;
        Point hit;
        double t = 0.0;
        if (!segmentIntersection(startPos, endPos, a, b, hit, t)) continue;
        if (t <= endpointEps || t >= 1.0 - endpointEps) continue;
        bool exists = false;
        for (const auto& entry : intersections) {
            if (Geometry::samePoint(entry.second, hit)) {
                exists = true;
                break;
            }
        }
        if (!exists) intersections.push_back({ t, hit });
    }

    std::sort(intersections.begin(), intersections.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<int> chainNodes;
    chainNodes.reserve(2 + intersections.size());
    chainNodes.push_back(startNode);

    for (const auto& entry : intersections) {
        int nodeIndex = addObstacleNode(entry.second);
        chainNodes.push_back(nodeIndex);
    }

    chainNodes.push_back(endNode);
    obstacleDraftNodes.push_back(endNode);

    for (size_t i = 1; i < chainNodes.size(); ++i) {
        int a = chainNodes[i - 1];
        int b = chainNodes[i];
        if (a == b) continue;
        int segIndex = (int)obstacleShape.segments.size();
        obstacleShape.segments.push_back({ a, b });
        obstacleDraftSegments.push_back(segIndex);
    }
}

void Line::finalizeObstacleRoute() {

    if (obstacleDraftNodes.size() < 2) return;
    const bool isLoop = obstacleDraftNodes.size() >= 4
        && obstacleDraftNodes.front() == obstacleDraftNodes.back();

    auto collectPointsFromNodeIndices = [&](const std::vector<int>& nodeIndices) {
        std::vector<Point> points;
        points.reserve(nodeIndices.size());
        for (int nodeIndex : nodeIndices) {
            if (nodeIndex < 0 || nodeIndex >= (int)obstacleShape.nodes.size()) continue;
            const Point& p = obstacleShape.nodes[nodeIndex].pos;
            if (points.empty() || !Geometry::samePoint(points.back(), p)) {
                points.push_back(p);
            }
        }
        return points;
        };


    std::vector<Point> routePoints;
    if (isLoop) {
        routePoints = collectPointsFromNodeIndices(obstacleDraftNodes);
    }
    else {
        std::vector<int> chainNodeIndices;
        chainNodeIndices.reserve(obstacleDraftSegments.size() + 1);
        int currentNode = -1;
        for (int segIndex : obstacleDraftSegments) {
            if (segIndex < 0 || segIndex >= (int)obstacleShape.segments.size()) continue;
            const auto& seg = obstacleShape.segments[segIndex];
            if (seg.a < 0 || seg.b < 0) continue;
            if (seg.a >= (int)obstacleShape.nodes.size() || seg.b >= (int)obstacleShape.nodes.size()) continue;

            if (chainNodeIndices.empty()) {
                chainNodeIndices.push_back(seg.a);
                chainNodeIndices.push_back(seg.b);
                currentNode = seg.b;
                continue;
            }

            if (seg.a == currentNode) {
                chainNodeIndices.push_back(seg.b);
                currentNode = seg.b;
            }
            else if (seg.b == currentNode) {
                chainNodeIndices.push_back(seg.a);
                currentNode = seg.a;
            }
        }
        routePoints = collectPointsFromNodeIndices(chainNodeIndices);
    }

    if (isLoop) {
        if (routePoints.size() >= 3 && !Geometry::samePoint(routePoints.front(), routePoints.back())) {
            routePoints.push_back(routePoints.front());
        }
        if (routePoints.size() >= 4) {
            obstacleLoopRoutes.push_back(std::move(routePoints));
        }
    }
    else if (routePoints.size() >= 2) {
        obstacleNoLoopRoutes.push_back(std::move(routePoints));
    }

    obstacleNextRouteId++;
    obstacleDraftNodes.clear();
    obstacleDraftSegments.clear();
}

// add a new node to the new route shape and return its index
int Line::addNewRouteNode(const Point& pos) {

    ShapeNode node;
    node.id = newRouteNextNodeId++;
    node.type = ShapeNodeType::Station;
    node.name = "route_" + std::to_string(node.id);
    node.station_id = "";
    node.pos = pos;
    int idx = (int)newRouteShape.nodes.size();
    newRouteShape.nodes.push_back(std::move(node));
    return idx;
}

void Line::addNewRoutePoint(const Point& pos) {

    if (newRouteDraftNodes.empty()) {
        int nodeIndex = addNewRouteNode(pos);
        newRouteDraftNodes.push_back(nodeIndex);
        return;
    }

    const int startNode = newRouteDraftNodes.back();
    if (startNode < 0 || startNode >= (int)newRouteShape.nodes.size()) return;
    const Point startPos = newRouteShape.nodes[startNode].pos;

    if (Geometry::samePoint(startPos, pos)) return;

    int endNode = addNewRouteNode(pos);
    newRouteDraftNodes.push_back(endNode);

    int segIndex = (int)newRouteShape.segments.size();
    newRouteShape.segments.push_back({ startNode, endNode });
    newRouteDraftSegments.push_back(segIndex);
}

void Line::finalizeNewRoute() {

    if (newRouteDraftSegments.empty()) return;

    ShapeRoute route;
    route.name = "route_" + std::to_string(newRouteNextRouteId);
    route.id = std::to_string(newRouteNextRouteId++);
    route.color[0] = newRouteColor[0];
    route.color[1] = newRouteColor[1];
    route.color[2] = newRouteColor[2];
    route.route_width = 5.0f;
    route.isObstacle = false;
    route.obstacleKind = ObstacleKind::Line;
    route.segmentIndices = newRouteDraftSegments;
    newRouteShape.routes.push_back(std::move(route));
    newRouteDraftNodes.clear();
    newRouteDraftSegments.clear();
}
