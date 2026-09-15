#pragma once
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>

// WebMercator system
struct Point {
    double x, y;
};

struct Station {
    std::string id;
    std::string station_id;//IDFM
    std::string name;

    // WebMerc system
    Point pos;
};

struct Route {
    std::string name;
    std::string id;
    float color[3];
    float route_width = 6.0f;

    // which stations are included in the route(unordered)
    std::vector<int> stationIndices;

    // shape(WebMerc system)
    std::vector<std::vector<Point>> segments;
};

// WebMerc system
struct Obstacle {
    std::vector<Point> outer;
    std::vector<std::vector<Point>> holes;
};

// WebMerc system
struct GeoData {
    std::vector<Route> routes;
    std::vector<Station> stations;
    std::vector<Obstacle> obstacles;
    std::vector<std::vector<int>> stationAdj;
};

//stations displacement
struct LineSegment {
    Point a;
    Point b;
};

// helper functions
namespace Geometry {

    GeoData filterRoutes(const GeoData& source, const std::unordered_set<std::string>& routeIds);

    std::string formatPoint(const Point& p);
    std::string stationKey(const Station& station);
    bool parsePrefixedId(const std::string& value, const std::string& prefix, int& out);
    double pointSegmentDist2(double px, double py, double ax, double ay, double bx, double by);
    bool pointInPolygon(const Point& p, const std::vector<Point>& polygon);
    bool samePoint(const Point& a, const Point& b, double eps = 1e-6);

    Point pointAtRelativePosition(const std::vector<std::vector<Point>>& segments, double t);
    double relativePositionOnSegments(const std::vector<std::vector<Point>>& segments, const Point& point);
}