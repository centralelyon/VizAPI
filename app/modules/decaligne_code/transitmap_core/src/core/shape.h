#pragma once

#include <string>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <variant>
#include "core/geometry.h"
#include "render/style.h"

// name ==NULL --> shape point  name!=NULL --> station  ugly code
// change to class{shape node type}
enum class ShapeNodeType : uint8_t { Station = 0, ShapePoint = 1, PseudoStation = 2 };

enum class ObstacleKind : uint8_t { Line = 0, Region = 1 };

struct ShapeNode {
    int id = -1;
    ShapeNodeType type = ShapeNodeType::ShapePoint;
    // for text
    std::string name;
    std::string station_id; // IDFM for stations
    Point pos;
    std::string uid; // Stable external identity; never a vector index.
};


inline bool isStationLike(ShapeNodeType type) { return type == ShapeNodeType::Station || type == ShapeNodeType::PseudoStation; }

struct ShapeSegment {
    int a = -1;
    int b = -1;
    std::string uid;
};

struct ShapeRoute {
    std::string name;
    std::string id;
    // TransitMapStyle --> route Width? 
    float route_width = 6.0f;
    float color[3];
    std::vector<int> segmentIndices;
    // Render-only directional instance; ordinary/editable routes leave these empty.
    std::vector<int> orderedNodes;
    std::string logicalRouteId, directionId, patternId;
    bool isObstacle = false;
    ObstacleKind obstacleKind = ObstacleKind::Line;
};

struct Shape {
    std::vector<ShapeNode> nodes;
    std::vector<ShapeSegment> segments;
    std::vector<ShapeRoute> routes;
};

class StyledShape {
    
public:
    // layer 0: background layer
    // style --> color
    float background_color[4];

    // layer 1: route layer - Geometry
    std::vector<std::vector<std::vector<Point>>> routes_geometry_vertices;
    // Ordered topology from the same traversal used to construct geometry.
    struct RoutePathTopology { std::vector<int> nodes, segments; };
    std::vector<std::vector<RoutePathTopology>> route_path_topology;
    //example
    //Route 0(path 0(point 0, point 1), path 1(point 2, point 3))
    //Route 1(path 0(point 4, point 5, point 6, point 7))
    //Route 2(path 0(point 8, point 9, point 10))

    struct StyledRouteSegmentRef {
        int routeIndex = -1;
        int pathIndex = -1;
        int pointIndexA = -1;
        int pointIndexB = -1;
    };
    std::vector<std::vector<StyledRouteSegmentRef>> shared_indices;
    //example:
    //shared 0 = (route 0(path 1(point 1, point 2)), route 1(path 1(point 5, point 6)), route 3(path 0(point 8, point 9)))
    //shared 1 = (route 0(path 2(point 2, point 3)), route 2(path 1(point 9, point 10)))

    std::vector<std::vector<float>> routes_spacing;
    //spacing matrix example:
	//(spacing(Route 0, Route 0)=0.0, spacing(Route 0, Route 1)=6.0, spacing(Route 0, Route 2)=3.0)
	//(spacing(Route 1, Route 0)=6.0, spacing(Route 1, Route 1)=0.0, spacing(Route 1, Route 2)=4.0)
	//(spacing(Route 2, Route 0)=3.0, spacing(Route 2, Route 1)=4.0, spacing(Route 2, Route 2)=0.0)

    // layer 1: route layer - Visual
    std::vector<float> routes_width;
    //example:
    //route 0_width(6.0f)
    //route 1_width(3.0f)
    //route 2_width(4.5f)

    std::vector<std::array<float, 3>> routes_colors;
    //example:
    //Route 0(0,0,0)
    //Route 1(0.5,0.5,0.5)
    //Route 2(1,1,1) 

    std::vector<char> routes_is_obstacle;

    //dynamic list
    struct BendStyle {
        bool enabled = false;
        //cos(alpha)
        float angleCos = 0.0f;
        float angleCosTolerance = 0.03f;
		//control point 0
        float radiusX = 0.0f;
        //control point 2
        float radiusY = 0.0f;
        // Optional exact inner-angle interval for degree-based API parameters.
        double minInnerAngle = -1.0, maxInnerAngle = -1.0;
		//control point 1 = (Point(alpha)_x, Point(alpha)_y)
    };
    std::vector<BendStyle> routes_bends;
    // Empty means use desktop global routes_bends; otherwise indexed by route.
    std::vector<std::vector<BendStyle>> route_bend_overrides;
    
    // layer 2: station layer based on routes
    struct TransferStation {
        std::string id;
        std::string station_id;//IDFM
        std::string name;

        // WebMerc system
        Point pos;

        struct Position {
            int routeIndex = -1;
            int pathIndex = -1;
            float pathT = 0.0f;
        };

        std::vector<Position> positions;
    };
    std::vector<TransferStation> transfer_stations;

    struct NormalStation {
        std::string id;
        std::string station_id;//IDFM
        std::string name;

        // WebMerc system
        Point pos;
        int routeIndex = -1;
        int pathIndex = -1;
        float pathT = 0.0f;
    };
    std::vector<NormalStation> normal_stations;

    StationShapeStyle normalStationShape;
    StationShapeStyle transferStationShape;
    std::unordered_map<std::string, StationShapeStyle> stationShapeOverrides;
};

class Region;

// Transfer between GeoData and Shape
Shape geoData2Shape(const GeoData& data);
GeoData getGeoDataFromShape(const Shape& shape);
StyledShape Shape2StyleShape(const Shape& shape);
// todo
//Shape StyleShape2Shape(const StyledShape& styledShape);
std::unordered_map<std::string, int> buildShapeNodeIndexByKey(const GeoData& geo, const Shape& shape, const std::vector<int>& shapeStationNodeById);
void preserveObstacleLineStyles(const Shape& sourceShape, GeoData& targetData);
bool buildRouteLoopNodeIndices(const Shape& shape, const ShapeRoute& route, std::vector<int>& outNodeLoop);
std::string stableShapeNodeKey(const ShapeNode& node, int nodeIndex);
Point snapPointToOctilinearFromAnchor(const Point& anchor, const Point& p);

/* shape editing */
// delete nodes
bool deleteShapeNodes(Shape& s, const std::vector<int>& nodesToDelete);

// update station meta
bool updateStationMeta(Shape& s, int stationNodeIdx, const std::string& newName, const std::string& newStationId);

// merge stations
bool mergeStations(Shape& s, const std::vector<int>& stationNodeIndices, const std::string& mergedName, const std::string& mergedStationId);

// split station
bool splitStation(Shape& s, int stationNodeIdx, const std::string& nameA, const std::string& idA, const std::string& nameB, const std::string& idB, const std::vector<char>& routeToB, double offsetWorld = 20.0);

bool splitShapeSegmentAtMidpoint(Shape& shape, int segmentIndex);
int refineShapeSegmentsCrossingRegions(Shape& shape, const Region& region);

Point snapShapeStationPosition(const Shape& shape, int nodeIndex, const Point& proposed, double angleThresholdRad);
bool resolveShapeRegionCollisions(Shape& shape, const Region& region, const std::unordered_map<int, Point>& baseline);
