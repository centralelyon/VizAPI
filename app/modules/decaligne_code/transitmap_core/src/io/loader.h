#pragma once
#include <string>
#include "json.hpp"
#include <vector>

#include "core/geometry.h"
#include "render/style.h"

struct TransitCssStyleEntry {
    std::string name;
    std::string filePath;
    TransitMapStyle style;
};

class Loader {

public:
    static void loadRoutesJson(const nlohmann::json& value, GeoData& data);
    static void loadRoutes(const std::string& path, GeoData& data);
    static void loadObstacles(const std::string& path, GeoData& data);
    static void loadLoomGraphGeoJson(const std::string& path, GeoData& data);

    static bool loadTransitCssStyle(const std::string& path, TransitMapStyle& outStyle);
    static bool loadTransitCssStyles(const std::string& folder, std::vector<TransitCssStyleEntry>& outStyles);
};