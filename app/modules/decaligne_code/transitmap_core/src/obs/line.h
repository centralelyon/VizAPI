#pragma once
#include <string>
#include <vector>

#include "core/shape.h"
#include "render/color.h"

class Line {
public:

    Shape obstacleShape;
    
    std::vector<int> obstacleDraftNodes;
    std::vector<int> obstacleDraftSegments;
    int obstacleNextNodeId = 0;
    int obstacleNextRouteId = 0;
    double obstacleLastClickTime = 0.0;
    int obstacleLastClickNode = -1;
    Point obstacleCursorWorld{ 0.0, 0.0 };
    bool obstacleCursorValid = false;

    std::vector<std::vector<Point>> obstacleNoLoopRoutes;
    std::vector<std::vector<Point>> obstacleLoopRoutes;

    Shape newRouteShape;
    std::vector<int> newRouteDraftNodes;
    std::vector<int> newRouteDraftSegments;
    int newRouteNextNodeId = 0;
    int newRouteNextRouteId = 0;
    double newRouteLastClickTime = 0.0;
    int newRouteLastClickNode = -1;
    Point newRouteCursorWorld{ 0.0, 0.0 };
    bool newRouteCursorValid = false;

    float obstacleRouteColor[3] = {
        Color::obstacleRoute[0],
        Color::obstacleRoute[1],
        Color::obstacleRoute[2]
    };
    float newRouteColor[3] = {
        Color::newRoute[0],
        Color::newRoute[1],
        Color::newRoute[2]
    };
    void syncCounters(const Shape& curShape);
    void addObstaclePoint(const Shape& curShape, const Point& pos, int existingNodeIndex = -1);
    void finalizeObstacleRoute();

    void addNewRoutePoint(const Point& pos);
    void finalizeNewRoute();

private:
    int addObstacleNode(const Point& pos);
    int addNewRouteNode(const Point& pos);
};
