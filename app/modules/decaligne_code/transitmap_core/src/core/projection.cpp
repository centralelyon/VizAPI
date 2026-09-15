#include <cmath>
#include "projection.h"

#define M_PI 3.1415926535897932384

// lon/lat --> webMercator
Point lonlatToWebMerc(double lon, double lat) {

    const double R = 6378137.0;
    double x = R * lon * M_PI / 180.0;
    double y = R * log(tan((90.0 + lat) * M_PI / 360.0));
    return { x, y };
}

//webMercator -->lon/lat
Point webMercToLonLat(double x, double y) {

    const double R = 6378137.0;
    const double lon = (x / R) * 180.0 / M_PI;
    const double lat = (2.0 * std::atan(std::exp(y / R)) - M_PI / 2.0) * 180.0 / M_PI;
    return { lon, lat };
}