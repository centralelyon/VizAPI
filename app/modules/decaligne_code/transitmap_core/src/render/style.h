#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <utility>
#include <algorithm>
#include <variant>
#include "core/geometry.h"
struct StationStyle {
    std::string fillColorHex = "#FFFFFF";
    float pathWidth = 1.5f;
    std::string pathColorHex = "#000000";
    bool fillUsesRouteColor = false;
    bool pathUsesRouteColor = false;
};
struct CircleStationStyle { float radius = 4.0f; StationStyle stationstyle; };
struct RoundedRectangleStationStyle {
    float width = 14.0f;
    float height = 8.0f;
    float cornerRadius = 3.0f;
    float rotationDegrees = 0.0f;
    float normalOffset = 0.0f;
    StationStyle stationstyle;
}; 
using StationShapeStyle = std::variant<CircleStationStyle, RoundedRectangleStationStyle>;

struct TransitMapStyle {
    struct LabelStyle {
        bool available = false;
        std::string fontFamily;
        float fontSize = 13.0f;
        float color[3] = { 0.08f, 0.08f, 0.08f };
    };
    struct RouteWidthStyle {
        float color[3] = { 0.0f, 0.0f, 0.0f };
        float width = 6.0f;
    };

    struct RouteGeometryStyle {
        struct BendStyle {
            double angleRad = 0.0;
            Point control1{ 0.35, 0.0 };
            Point control2{ 0.65, 1.0 };
        };

        bool enabled = false;
        std::vector<double> bendAnglesRad; // legacy input, converted to routeStyle.bendStyles
    };

    struct RouteStyle {
        std::string id;
        std::vector<RouteGeometryStyle::BendStyle> bendStyles;
    };

    struct DisplayRoute {
        std::string routeId;
        float color[3] = { 0.0f, 0.0f, 0.0f };
        float width = 6.0f;
        std::vector<std::vector<Point>> segments;
    };

    std::string backgroundHex = "#FFFFFF";
    LabelStyle labelStyle;

    float routeWidth = 6.0f;
    float routeSpacing = 7.0f;
    std::vector<RouteWidthStyle> routeWidthsByColor;
    std::unordered_map<std::string, float> routeWidthsById;
    RouteGeometryStyle routeGeometry;
    RouteStyle routeStyle;

    StationShapeStyle normalStationShape;
    StationShapeStyle transferStationShape;
    std::vector<std::string> paletteHex;

    std::unordered_map<std::string, std::string> routeColorsById;
};

class StyleExtractor {

public:

    static bool extractFromImage(
        const std::string& imagePath,
        TransitMapStyle& outStyle,
        std::string& outError
    );
};

class StyleIO {

public:

    static bool save(
        const std::string& path,
        const TransitMapStyle& style,
        std::string& outError
    );

    static bool load(
        const std::string& path,
        TransitMapStyle& outStyle,
        std::string& outError
    );
};

class StyleApplicator {

public:

    static void applyToGeoData(
        const TransitMapStyle& style,
        GeoData& ioData
    );

    static std::vector<TransitMapStyle::DisplayRoute> buildDisplayRoutes(
        const TransitMapStyle& style,
        const GeoData& geometryData
    );
};
