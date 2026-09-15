#pragma once
#include "geometry.h"

class Camera {

public:
    void resize(int w, int h);
    void setTransform(double x, double y, double units) { centerX=x; centerY=y; scale=units; }
    void fitToData(const GeoData& data);

    void pan(double dx, double dy);
    void zoom(double factor);
    void zoomAt(double factor, double screenX, double screenY);

    //void vertex(double x, double y) const;

    // protect private variables
    double getScale() const;
    int getScreenW() const;
    int getScreenH() const;

    // normalized device coordinates
    float ndcX(double wx) const;
    float ndcY(double wy) const;

    // webMercator to screen 
    double sx(double x) const;
    double sy(double y) const;

    //screen to webMercator
    Point screenToWorld(double px, double py) const;

private:
    double centerX = 0.0;
    double centerY = 0.0;
    double scale = 1.0;
    int screenW = 1;
    int screenH = 1;
};

// inverse the coordinates in exporting
inline Point worldToTopLeftScreen(const Camera& camera, const Point& world,
    double scaleX = 1.0, double scaleY = 1.0) {
    return {
        camera.sx(world.x) / scaleX,
        (camera.getScreenH() - camera.sy(world.y)) / scaleY
    };
}

inline Point finalLabelScreenPosition(const Point& stationScreen,
    const Point& labelOffset, double fontSize) {
    return {
        stationScreen.x + 10.0 + labelOffset.x,
        stationScreen.y - fontSize - 4.0 + labelOffset.y
    };
}