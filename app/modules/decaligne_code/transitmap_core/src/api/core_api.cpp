#include "api/core_api.h"
#include "core/shape.h"
#include "core/projection.h"
#include "io/loader.h"
#include "io/directional.h"
#include "io/route_directions.h"
#include "io/shape_loom_export.h"
#include "render/render_geometry.h"
#include "obs/line.h"
#include "obs/region.h"
#include "obs/region_deformation.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>

#include <stdexcept>

using json = nlohmann::json;
namespace {
constexpr double pi = 3.14159265358979323846;
void require(bool test, const std::string& message) { if(!test) throw std::invalid_argument(message); }
double number(const json& j, const char* key, double fallback, double lo, double hi) {
    if(!j.contains(key)) return fallback;
    require(j[key].is_number(), std::string(key)+" must be numeric");
    double v=j[key].get<double>();
    require(std::isfinite(v) && v>=lo && v<=hi, std::string(key)+" is outside its allowed range");
    return v;
}
std::string identifier(const json& j) {
    if(j.is_string()) return j.get<std::string>();
    if(j.is_number_integer()) return j.dump();
    throw std::invalid_argument("Expected a string/integer ID");
}
std::string color(const float c[3]) {
    std::ostringstream out; out << '#';
    for(int i=0;i<3;++i) out << std::hex << std::setw(2) << std::setfill('0') << int(std::clamp(c[i],0.f,1.f)*255+.5f);
    return out.str();
}
int nodeIndex(const Shape& s, const json& id) {
    auto value=identifier(id);
    for(size_t i=0;i<s.nodes.size();++i) if(s.nodes[i].uid==value) return int(i);
    throw std::invalid_argument("Unknown node: "+value);
}
int segmentIndex(const Shape& s, const json& id) {
    auto value=identifier(id);
    for(size_t i=0;i<s.segments.size();++i) if(s.segments[i].uid==value) return int(i);
    throw std::invalid_argument("Unknown segment: "+value);
}
int routeIndex(const Shape& s, const json& id) {
    auto value=identifier(id);
    for(size_t i=0;i<s.routes.size();++i) if(s.routes[i].id==value) return int(i);
    throw std::invalid_argument("Unknown route: "+value);
}
std::string edgeKey(const Shape& s, const ShapeSegment& e) {
    auto a=s.nodes.at(e.a).uid,b=s.nodes.at(e.b).uid;
    if(b<a) std::swap(a,b);
    return json::array({a,b}).dump();
}
// Recover unchanged segment identities after desktop operations compact/rebuild
// their vectors. Changed edges receive fresh IDs; deleted IDs are never reused.
void identities(Shape& s, json& state, const Shape* before=nullptr) {
    long long counter=state.value("nextId",1LL);
    std::set<std::string> reserved;
    for(const auto& n:s.nodes)reserved.insert(n.uid);
    for(const auto& e:s.segments)reserved.insert(e.uid);
    auto fresh=[&](const char* prefix) { std::string id;do{id=std::string(prefix)+std::to_string(counter++);}while(reserved.count(id));reserved.insert(id);return id; };
    std::set<std::string> used;
    int internal=0; for(const auto& n:s.nodes) internal=std::max(internal,n.id+1);
    for(auto& n:s.nodes) {
        if(n.uid.empty() || used.count(n.uid)) n.uid=fresh("n");
        used.insert(n.uid);
        if(n.id<0) n.id=internal++;
    }
    std::map<std::string,std::string> previous;
    if(before) for(const auto& e:before->segments) previous[edgeKey(*before,e)]=e.uid;
    used.clear();
    for(auto& e:s.segments) {
        const auto key=edgeKey(s,e);
        if(before) e.uid=previous.count(key)?previous[key]:"";
        if(e.uid.empty() || used.count(e.uid)) e.uid=fresh("e");
        used.insert(e.uid);
    }
    state["nextId"]=counter;
}
Shape decode(const json& j) {
    Shape s;
    for(const auto& n:j.at("nodes")) {
        ShapeNode v; v.uid=n.at("id"); v.id=n.at("internalId"); v.name=n.value("name","");
        v.station_id=n.value("stationId",""); v.type=ShapeNodeType(n.at("type").get<int>());
        v.pos={n.at("x"),n.at("y")}; s.nodes.push_back(v);
    }
    for(const auto& e:j.at("segments")) s.segments.push_back({nodeIndex(s,e.at("a")),nodeIndex(s,e.at("b")),e.at("id")});
    for(const auto& r:j.at("routes")) {
        ShapeRoute v; v.id=r.at("id"); v.name=r.at("name"); v.route_width=r.value("width",6.f);
        for(int i=0;i<3;++i) v.color[i]=r.at("rgb")[i];
        v.isObstacle=r.value("isObstacle",false); v.obstacleKind=ObstacleKind(r.value("obstacleKind",0));
        for(const auto& id:r.at("segmentIds")) v.segmentIndices.push_back(segmentIndex(s,id));
        s.routes.push_back(v);
    }
    return s;
}
Camera cameraFor(const json& state) {
    Camera c; c.resize(1200,800);
    auto t=state.at("transform"); c.setTransform(t.at("x"),t.at("y"),t.at("scale")); return c;
}
Point screen(const Camera& c, Point p) { return {c.sx(p.x),800-c.sy(p.y)}; }
Point fromScreen(const Camera& c, const json& request) {
    return c.screenToWorld(number(request,"x",0,-1e7,1e7),800-number(request,"y",0,-1e7,1e7));
}
json point(Point p) { return json::array({p.x,p.y}); }
json encode(const Shape& s, const Camera& camera, const StyledShape& styled, json& state) {
    json nodes=json::array(),edges=json::array(),routes=json::array();
    std::vector<std::set<std::string>> nr(s.nodes.size()),er(s.segments.size());
    for(const auto& r:s.routes) for(int ei:r.segmentIndices) {
        er.at(ei).insert(r.id); if(!r.isObstacle){nr.at(s.segments[ei].a).insert(r.id); nr.at(s.segments[ei].b).insert(r.id);}
    }
    for(size_t i=0;i<s.nodes.size();++i) {
        const auto& n=s.nodes[i];
        nodes.push_back({{"id",n.uid},{"internalId",n.id},{"type",int(n.type)},{"name",n.name},{"stationId",n.station_id},
            {"x",n.pos.x},{"y",n.pos.y},{"position",point(screen(camera,n.pos))},{"isStation",isStationLike(n.type)},
            {"interchange",isStationLike(n.type)&&nr[i].size()>1},{"routeIds",nr[i]}});
    }
    for(size_t i=0;i<s.segments.size();++i) {
        const auto& e=s.segments[i]; edges.push_back({{"id",e.uid},{"a",s.nodes[e.a].uid},{"b",s.nodes[e.b].uid},{"routeIds",er[i]}});
    }
    auto registry=state.value("pathIds",json::object());
    for(size_t ri=0;ri<s.routes.size();++ri) {
        const auto& r=s.routes[ri]; json ids=json::array(),paths=json::array();
        for(int e:r.segmentIndices) ids.push_back(s.segments[e].uid);
        for(const auto& p:styled.route_path_topology[ri]) {
            json ns=json::array(),es=json::array();
            for(int n:p.nodes) ns.push_back(s.nodes[n].uid);
            for(int e:p.segments) es.push_back(s.segments[e].uid);
            auto sorted=p.segments; std::vector<std::string> keys;
            for(int e:sorted) keys.push_back(s.segments[e].uid);
            std::sort(keys.begin(),keys.end()); const auto key=json::array({r.id,keys}).dump();
            if(!registry.contains(key)) { long long counter=state.at("nextId"); registry[key]="p"+std::to_string(counter); state["nextId"]=counter+1; }
            paths.push_back({{"id",registry[key]},{"nodeIds",ns},{"segmentIds",es}});
        }
        std::map<int,int> degree;
        for(int e:r.segmentIndices){++degree[s.segments[e].a];++degree[s.segments[e].b];}
        json endpoints=json::array();
        auto endpoint=[&](int index){const auto& n=s.nodes[index];return n.name.empty()?n.station_id:n.name;};
        for(const auto& entry:degree)if(entry.second==1)endpoints.push_back(endpoint(entry.first));
        if(endpoints.empty()&&!styled.route_path_topology[ri].empty()) {
            const auto& ns=styled.route_path_topology[ri].front().nodes;
            if(!ns.empty()){endpoints.push_back(endpoint(ns.front()));endpoints.push_back(endpoint(ns.back()));}
        }
        // Ordered trajectory visits retain direction through loops and repeated edges.
        if(state.contains("routeTraversals")) {
            json visits=json::array();for(const auto& visit:state["routeTraversals"])if(visit["routeId"]==r.id)visits.push_back(visit);
            if(!visits.empty()&&std::all_of(visits.begin(),visits.end(),[](const auto& v){return v.contains("traversalIndex");})) {
                std::stable_sort(visits.begin(),visits.end(),[](const auto& a,const auto& b){return a["traversalIndex"]<b["traversalIndex"];});
                auto first=visits.front()["from"],last=visits.back()["to"];
                auto start=std::find_if(s.nodes.begin(),s.nodes.end(),[&](const auto& n){return n.uid==first;});
                auto end=std::find_if(s.nodes.begin(),s.nodes.end(),[&](const auto& n){return n.uid==last;});
                if(start!=s.nodes.end()&&end!=s.nodes.end())endpoints=json::array({endpoint(int(start-s.nodes.begin())),endpoint(int(end-s.nodes.begin()))});
            }
        }
        routes.push_back({{"endpoints",endpoints},{"id",r.id},{"name",r.name},{"color",color(r.color)},{"rgb",json::array({r.color[0],r.color[1],r.color[2]})},
            {"width",r.route_width},{"isObstacle",r.isObstacle},{"obstacleKind",int(r.obstacleKind)},{"segmentIds",ids},{"paths",paths}});
    }
    state["pathIds"]=registry;
    return {{"nodes",nodes},{"segments",edges},{"routes",routes}};
}
void validate(const Shape& s) {

    for(const auto& n:s.nodes) require(std::isfinite(n.pos.x)&&std::isfinite(n.pos.y),"Non-finite node");
    for(const auto& e:s.segments) require(e.a>=0&&e.b>=0&&e.a<int(s.nodes.size())&&e.b<int(s.nodes.size())&&e.a!=e.b,"Invalid topology segment");
    std::set<std::string> ids;
    for(const auto& r:s.routes) {
        require(!r.id.empty() && ids.insert(r.id).second,"Duplicate/empty route ID");
        for(int e:r.segmentIndices) require(e>=0&&e<int(s.segments.size()),"Invalid route membership");
    }
}
std::string logicalId(const ShapeRoute& r) {return r.logicalRouteId.empty()?r.id:r.logicalRouteId;}
void geometryStyle(StyledShape& s, const Shape& shape, const json& style) {
    require(style.is_object(),"Style must be an object");
    auto routes=style.value("routes",json::object());
    require(routes.is_object(),"style.routes must be an object");
    const auto count=shape.routes.size(); s.route_bend_overrides.resize(count);
    std::vector<double> spacing(count,0);
    for(size_t ri=0;ri<count;++ri) {
        const auto r=routes.value(logicalId(shape.routes[ri]),json::object());
        require(r.is_object(),"Route style must be an object");
        s.routes_width[ri]=number(r,"width",shape.routes[ri].route_width,0,1000);
        spacing[ri]=number(r,"spacing",0,-1000,1000);
        const double tolerance=number(r,"bendTolerance",15,0,89);
        const auto rules=r.value("bends",json::array()); require(rules.is_array(),"bends must be an array");
        require(rules.size()<=100,"Too many bend rules");
        for(const auto& rule:rules) {
            const double angle=number(rule,"angle",90,0.001,179.999);
            StyledShape::BendStyle b; b.enabled=true; b.angleCos=std::cos((180-angle)*pi/180);
            b.minInnerAngle=std::max(0.0,angle-tolerance); b.maxInnerAngle=std::min(180.0,angle+tolerance);
            b.radiusX=number(rule,"offsetIn",0,0,1000); b.radiusY=number(rule,"offsetOut",0,0,1000);
            b.enabled=b.radiusX>0&&b.radiusY>0; s.route_bend_overrides[ri].push_back(b);
        }
    }
    // C++ spacing is signed centerline distance, independent of route width.
    for(size_t a=0;a<count;++a) for(size_t b=0;b<count;++b)
        s.routes_spacing[a][b]=a==b?0:(std::abs(spacing[a])>=std::abs(spacing[b])?spacing[a]:spacing[b]);
    const auto pairs=style.value("pairs",json::array()); require(pairs.is_array(),"pairs must be an array");
    for(const auto& p:pairs) {
        const double gap=number(p,"spacing",0,-1000,1000);
        for(size_t a=0;a<count;++a)for(size_t b=0;b<count;++b)
            if(logicalId(shape.routes[a])==p.at("a")&&logicalId(shape.routes[b])==p.at("b"))
                s.routes_spacing[a][b]=s.routes_spacing[b][a]=a==b?0:gap;
    }
}
json rendered(const Shape& shape, const StyledShape& s, const Camera& camera, const json& topology, double spacingScale=1.0) {
    auto display=buildStyledShapeDisplayData(s,camera,spacingScale); json routes=json::array(),stations=json::array();
    for(size_t ri=0;ri<shape.routes.size();++ri) {
        if(shape.routes[ri].isObstacle)continue;
        json paths=json::array();
        for(size_t pi=0;pi<display.commands[ri].size();++pi) {
            json commands=json::array(),points=json::array();
            for(const auto& c:display.commands[ri][pi]) {
                json v={{"type",c.type==RenderCommand::Type::Move?"move":c.type==RenderCommand::Type::Line?"line":"quadratic"},{"x",c.end.x},{"y",800-c.end.y}};
                if(c.type==RenderCommand::Type::Quadratic) {v["cx"]=c.control.x;v["cy"]=800-c.control.y;}
                commands.push_back(v);
            }
            for(auto p:display.screenRoutePaths[ri][pi]) points.push_back(point({p.x,800-p.y}));
            json path={{"id",topology["routes"][ri]["paths"][pi]["id"]},{"commands",commands},{"points",points}};
            if(!shape.routes[ri].logicalRouteId.empty()) {path["directionId"]=shape.routes[ri].directionId;path["patternId"]=shape.routes[ri].patternId;}
            paths.push_back(path);
        }
        const auto id=logicalId(shape.routes[ri]);
        auto existing=std::find_if(routes.begin(),routes.end(),[&](const auto& r){return r.at("routeId")==id;});
        if(existing==routes.end())routes.push_back({{"routeId",id},{"width",s.routes_width[ri]},{"paths",paths}});
        else for(const auto& path:paths)(*existing)["paths"].push_back(path);
    }
    std::map<int,std::string> nodeIds; for(const auto& n:shape.nodes) nodeIds[n.id]=n.uid;
    auto addStation=[&](const auto& st,const std::vector<int>& ris) {
        auto id=st.id.substr(0,st.id.find("_split_")); int internal=std::stoi(id);
        json ids=json::array(); for(int ri:ris) if(!shape.routes.at(ri).isObstacle)ids.push_back(logicalId(shape.routes.at(ri)));
        if(ids.empty())return;
        stations.push_back({{"id",st.id},{"nodeId",nodeIds.at(internal)},{"point",point(screen(camera,st.pos))},{"routeIds",ids}});
    };
    for(const auto& st:display.normalStations) addStation(st,{st.routeIndex});
    for(const auto& st:display.transferStations) {std::vector<int> ids;for(const auto& p:st.positions)ids.push_back(p.routeIndex);addStation(st,ids);}
    // One optional display symbol per topology node. Compute its anchor here
    // so the Web client never reconstructs station geometry.
    json mergedStations=json::array();
    for(const auto& node:topology["nodes"]) {
        if(!node.value("interchange",false))continue;
        double x=0,y=0;size_t count=0;
        for(const auto& st:stations)if(st["nodeId"]==node["id"]) {
            x+=st["point"][0].get<double>();y+=st["point"][1].get<double>();++count;
        }
        if(count)mergedStations.push_back({{"id",node["id"].get<std::string>()+"_merged"},
            {"nodeId",node["id"]},{"point",json::array({x/count,y/count})},{"routeIds",node["routeIds"]}});
    }
    return {{"routes",routes},{"stations",stations},{"mergedStations",mergedStations}};
}
json mapCoordinates(json geometry, const Camera* camera, bool planar) {
    auto convert=[&](auto&& self,json& coords)->void {
        if(coords.is_array()&&coords.size()>=2&&coords[0].is_number()&&coords[1].is_number()) {
            Point p{coords[0],coords[1]};
            require(std::isfinite(p.x)&&std::isfinite(p.y),"Invalid obstacle coordinate");
            if(!planar && std::abs(p.x)<=180&&std::abs(p.y)<=90) p=lonlatToWebMerc(p.x,std::clamp(p.y,-85.05112878,85.05112878));
            if(camera) p=screen(*camera,p); coords=point(p);
        } else if(coords.is_array()) for(auto& c:coords) self(self,c);
    };
    if(geometry.contains("coordinates")) convert(convert,geometry["coordinates"]);
    if(geometry.contains("geometries")) for(auto& g:geometry["geometries"])g=mapCoordinates(g,camera,planar);
    return geometry;
}
Shape loadMap(json map) {
    require(map.value("type","")=="FeatureCollection" && map.contains("features")&&map["features"].is_array(),"Expected GeoJSON FeatureCollection");
    require(map["features"].size()<=100000,"Too many features");
    for(auto& f:map["features"]) {
        auto& p=f["properties"]; const auto& g=f.at("geometry");
        if(g.value("type","")=="Point") { p["id"]=identifier(p.at("id")); }
        if(g.value("type","")=="LineString") {
            p["from"]=identifier(p.at("from")); p["to"]=identifier(p.at("to"));
            for(auto& line:p["lines"]) {
                if(line.contains("id")) {
                    const auto id=identifier(line["id"]);
                    const auto displayName=line.contains("name")&&line["name"].is_string()
                        ? line["name"].get<std::string>() : line.contains("label")&&line["label"].is_string()
                        ? line["label"].get<std::string>() : id;
                    line["name"]=displayName;
                    line["label"]=id;
                }
            }
        }
    }
    std::map<std::string,json> points;
    for(const auto& f:map["features"]) {
        const auto& g=f.at("geometry");const auto& props=f.at("properties");
        auto validPoint=[](const json& p) {
            require(p.is_array()&&p.size()>=2&&p[0].is_number()&&p[1].is_number(),"Expected numeric coordinates");
            for(int i=0;i<2;++i)require(std::isfinite(double(p[i]))&&std::abs(double(p[i]))<1e10,"Invalid coordinate range");
        };
        if(g["type"]=="Point") {
            validPoint(g.at("coordinates"));std::string id=props.at("id");
            require(!points.count(id)||points[id]==g["coordinates"],"Conflicting duplicate point ID: "+id);points[id]=g["coordinates"];
        } else if(g["type"]=="LineString") {
            require(g["coordinates"].is_array()&&g["coordinates"].size()>=2,"LineString needs two points");
            for(const auto& p:g["coordinates"])validPoint(p);
            require(props.at("lines").is_array()&&!props["lines"].empty(),"LineString needs route membership");
        } else throw std::invalid_argument("Network supports Point and LineString features only");
    }
    for(const auto& f:map["features"])if(f["geometry"]["type"]=="LineString") {
        const auto& p=f["properties"];require(points.count(p.at("from"))&&points.count(p.at("to")),"LineString references a missing point");
    }
    // GTFS topo already defines connectivity with from/to. Re-snapping its
    // samples to nearby stations can create links absent from that topology.
    if(hasRouteDirections(map)||map.contains("transitMapDirections"))return loadDirectedTopology(map);
    auto loaderMap=map;
    std::set<std::string> anchors,stationPositions;
    if(map.contains("transitMapDirections")) {
        // geoData2Shape snaps samples to stations. Protect explicit topology
        // vertices too: otherwise two sides of a loop can collapse to one edge.
        // The temporary station marker exists only inside preprocessing.
        for(const auto& f:map["features"])if(f["geometry"]["type"]=="Point") {
            const auto& p=f["properties"];
            if(p.contains("station_label")||p.contains("name")||p.contains("station_id"))
                stationPositions.insert(f["geometry"]["coordinates"].dump());
        }
        for(auto& f:loaderMap["features"])if(f["geometry"]["type"]=="Point") {
            auto& p=f["properties"];
            if(!p.contains("station_label")&&!p.contains("name")) {
                p["station_label"]="";
                if(!p.contains("station_id")&&!stationPositions.count(f["geometry"]["coordinates"].dump()))anchors.insert(p["id"]);
            }
        }
    }
    GeoData data; Loader::loadRoutesJson(loaderMap,data);
    std::sort(data.routes.begin(),data.routes.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    auto shape=geoData2Shape(data); require(!shape.routes.empty(),"Map contains no routes");
    retainMapNodeIds(shape,map);
    for(auto& n:shape.nodes)if(anchors.count(n.uid)){n.type=ShapeNodeType::ShapePoint;n.name.clear();n.station_id.clear();}
    return shape;
}

// Drawing uses the desktop Line/Region builders. Only the core turns pointer
// samples into topology; background vertices never enter the transit Shape.
std::vector<Point> drawingPoints(const json& req, const Camera& cam, bool keepDuplicates=false) {
    const auto& input=req.at("points");
    require(input.is_array()&&input.size()>=2&&input.size()<=1000,"Draw 2–1000 points");
    std::vector<Point> points;
    for(const auto& p:input) {
        require(p.is_array()&&p.size()==2&&p[0].is_number()&&p[1].is_number(),"Expected [x,y] canvas points");
        Point world=fromScreen(cam,{{"x",p[0]},{"y",p[1]}});
        if(keepDuplicates||points.empty()||std::hypot(world.x-points.back().x,world.y-points.back().y)>1e-6/cam.getScale())points.push_back(world);
    }
    require(points.size()>=2,"Drawing needs two different points");
    return points;
}
double along(Point p, Point a, Point b) {
    const double dx=b.x-a.x,dy=b.y-a.y,len=dx*dx+dy*dy;
    return len>0?((p.x-a.x)*dx+(p.y-a.y)*dy)/len:0;
}
Point at(Point a, Point b, double t) {return {a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t};}
bool onEdge(Point p, Point a, Point b, double eps, double& t) {
    t=along(p,a,b);auto q=at(a,b,std::clamp(t,0.0,1.0));
    return std::hypot(p.x-q.x,p.y-q.y)<=eps;
}
bool crossing(Point a, Point b, Point c, Point d, Point& p) {
    const double x=b.x-a.x,y=b.y-a.y,u=d.x-c.x,v=d.y-c.y,den=x*v-y*u;
    if(std::abs(den)<=1e-12*std::hypot(x,y)*std::hypot(u,v))return false;
    const double t=((c.x-a.x)*v-(c.y-a.y)*u)/den;
    const double w=((c.x-a.x)*y-(c.y-a.y)*x)/den;
    if(t<0||t>1||w<0||w>1)return false;
    p=at(a,b,t);return true;
}
struct DrawnRoute {std::string id;std::vector<int> nodes;};
DrawnRoute drawRoute(Shape& shape,const json& req,const Camera& cam) {
    auto points=drawingPoints(req,cam,true);
    const auto anchors=req.value("nodeIds",json::array());
    require(anchors.is_array()&&(anchors.empty()||anchors.size()==points.size()),"nodeIds must match drawing points");
    const double radius=number(req,"pickRadius",8,0,1000)/cam.getScale(),eps=1e-5/cam.getScale();
    Line draft;
    for(const auto& n:shape.nodes)draft.newRouteNextNodeId=std::max(draft.newRouteNextNodeId,n.id+1);
    // Pointer tolerance is expressed in canvas units, supplied from the SVG
    // transform so picking behaves consistently while zoomed.
    for(size_t pointIndex=0;pointIndex<points.size();++pointIndex) {
        auto p=points[pointIndex];
        double best=radius;int nearest=-1;
        // A clicked styled symbol may be offset from its topology position.
        // Resolve its identity in C++, never infer a new station from that offset.
        if(!anchors.empty()&&!anchors[pointIndex].is_null()) {
            p=shape.nodes[nodeIndex(shape,anchors[pointIndex])].pos;
        }
        for(size_t n=0;n<shape.nodes.size();++n) {
            const auto q=shape.nodes[n].pos;double distance=std::hypot(p.x-q.x,p.y-q.y);
            if(distance<=best){best=distance;nearest=int(n);}
        }
        if(nearest>=0)p=shape.nodes[nearest].pos;
        else {
            if(req.value("snap",true)&&!draft.newRouteShape.nodes.empty()) {
                const auto a=draft.newRouteShape.nodes.back().pos;
                const double length=std::hypot(p.x-a.x,p.y-a.y),angle=std::round(std::atan2(p.y-a.y,p.x-a.x)/(pi/4))*(pi/4);
                p={a.x+length*std::cos(angle),a.y+length*std::sin(angle)};
            }
            Point projected=p;best=radius;
            for(const auto& e:shape.segments) {
                const auto a=shape.nodes[e.a].pos,b=shape.nodes[e.b].pos;
                auto q=at(a,b,std::clamp(along(p,a,b),0.0,1.0));double distance=std::hypot(p.x-q.x,p.y-q.y);
                if(distance<=best){best=distance;projected=q;}
            }
            p=projected;
        }
        draft.addNewRoutePoint(p);
    }
    draft.finalizeNewRoute();
    require(draft.newRouteShape.routes.size()==1,"Drawing needs two different stations");
    auto route=draft.newRouteShape.routes.front();
    auto exists=[&](const std::string& id){return std::any_of(shape.routes.begin(),shape.routes.end(),[&](const auto& r){return r.id==id;});};
    route.id=req.value("routeId",std::string());
    if(route.id.empty()){int i=1;do{route.id="drawn_route_"+std::to_string(i++);}while(exists(route.id));}
    require(route.id.size()<=120&&!exists(route.id),"Route ID already exists or is too long");
    route.name=req.value("name",std::string());if(route.name.empty())route.name=route.id;
    const std::string hex=req.value("color",std::string("#3366b3"));
    require(hex.size()==7&&hex[0]=='#'&&hex.find_first_not_of("0123456789abcdefABCDEF",1)==std::string::npos,"Expected #RRGGBB color");
    for(int i=0;i<3;++i)route.color[i]=std::stoi(hex.substr(1+i*2,2),nullptr,16)/255.f;
    route.segmentIndices.clear();
    int nextInternal=draft.newRouteNextNodeId;
    std::set<int> touched;
    auto station=[&](Point p) {
        for(size_t i=0;i<shape.nodes.size();++i)if(std::hypot(shape.nodes[i].pos.x-p.x,shape.nodes[i].pos.y-p.y)<=eps){touched.insert(int(i));return int(i);}
        ShapeNode node;node.id=nextInternal++;node.type=ShapeNodeType::Station;node.pos=p;
        node.name="Station "+std::to_string(node.id);shape.nodes.push_back(node);
        touched.insert(int(shape.nodes.size())-1);return int(shape.nodes.size())-1;
    };
    std::vector<Point> vertices;for(const auto& n:draft.newRouteShape.nodes){vertices.push_back(n.pos);station(n.pos);}
    // Crossings, T-junctions and collinear shared sections become real shared
    // topology. All incidences of a split edge retain their route membership.
    for(size_t i=1;i<vertices.size();++i) {
        const auto a=vertices[i-1],b=vertices[i];Point hit;
        for(const auto& e:shape.segments)if(crossing(a,b,shape.nodes[e.a].pos,shape.nodes[e.b].pos,hit))station(hit);
        for(size_t j=1;j<i;++j)if(crossing(a,b,vertices[j-1],vertices[j],hit))station(hit);
    }
    auto chain=[&](Point a,Point b,const std::vector<int>& candidates) {
        std::vector<std::pair<double,int>> result;
        for(int n:candidates){double t;if(onEdge(shape.nodes[n].pos,a,b,eps,t))result.push_back({std::clamp(t,0.0,1.0),n});}
        std::sort(result.begin(),result.end());return result;
    };
    std::vector<int> all;for(size_t n=0;n<shape.nodes.size();++n)all.push_back(int(n));
    std::vector<std::vector<int>> paths;
    for(size_t i=1;i<vertices.size();++i) {
        std::vector<int> path;
        for(const auto& entry:chain(vertices[i-1],vertices[i],all)) {
            if(!path.empty()&&std::hypot(shape.nodes[path.back()].pos.x-shape.nodes[entry.second].pos.x,shape.nodes[path.back()].pos.y-shape.nodes[entry.second].pos.y)<=eps)continue;
            path.push_back(entry.second);touched.insert(entry.second);
        }
        require(path.size()>=2,"Route contains a zero-length segment");paths.push_back(path);
    }
    const auto oldEdges=shape.segments;std::vector<std::vector<int>> replacements(oldEdges.size());
    shape.segments.clear();std::map<std::pair<int,int>,int> edgeIds;
    auto edge=[&](int a,int b){auto key=std::minmax(a,b);auto it=edgeIds.find(key);if(it!=edgeIds.end())return it->second;
        int index=int(shape.segments.size());shape.segments.push_back({a,b,{}});edgeIds[key]=index;return index;};
    const std::vector<int> candidates(touched.begin(),touched.end());
    for(size_t i=0;i<oldEdges.size();++i) {
        const auto e=oldEdges[i];auto ns=chain(shape.nodes[e.a].pos,shape.nodes[e.b].pos,candidates);
        int last=e.a;
        for(const auto& entry:ns)if(entry.first>0&&entry.first<1&&entry.second!=last){replacements[i].push_back(edge(last,entry.second));last=entry.second;}
        replacements[i].push_back(edge(last,e.b));
    }
    for(auto& r:shape.routes){std::vector<int> indices;for(int e:r.segmentIndices)indices.insert(indices.end(),replacements[e].begin(),replacements[e].end());r.segmentIndices=indices;}
    DrawnRoute result;result.id=route.id;
    for(const auto& path:paths) {
        for(size_t i=1;i<path.size();++i){int e=edge(path[i-1],path[i]);if(std::find(route.segmentIndices.begin(),route.segmentIndices.end(),e)==route.segmentIndices.end())route.segmentIndices.push_back(e);}
        result.nodes.insert(result.nodes.end(),path.begin()+(result.nodes.empty()?0:1),path.end());
        for(int n:path){auto& node=shape.nodes[n];node.type=ShapeNodeType::Station;if(node.name.empty())node.name="Station "+std::to_string(node.id);}
    }
    shape.routes.push_back(route);return result;
}
void drawFeature(json& state,const std::string& op,const json& req,const Camera& cam) {
    json drawing=req;
    if(op=="draw-region-feature"&&req.contains("preset")) {
        auto preset=req.at("preset").get<std::string>();require(preset=="rectangle"||preset=="triangle"||preset=="circle","Unknown region preset");
        auto viewport=req.value("viewport",json::object());double vw=number(viewport,"w",1200,120,6000),vh=number(viewport,"h",800,80,6000);auto center=req.value("center",json{{"x",number(viewport,"x",0,-1e7,1e7)+vw*.5},{"y",number(viewport,"y",0,-1e7,1e7)+vh*.5}});double x=number(center,"x",600,-1e7,1e7),y=number(center,"y",400,-1e7,1e7);
        double size=number(req,"size",vw*.1,8,3000);
        if(!req.contains("center")) {
            auto network=decode(state.at("shape"));for(auto& n:network.nodes)n.pos=screen(cam,n.pos);
            auto faces=RegionDeformation::cells(network);double best=1e30;
            for(const auto& face:faces){auto c=RegionDeformation::center(face.polygon);double distance=std::hypot(c.x-x,c.y-y);if(distance<best){best=distance;center={{"x",c.x},{"y",c.y}};double clearance=RegionDeformation::boundaryDistance(c,face.polygon);size=std::min(number(req,"size",vw*.1,8,3000),std::max(8.0,clearance*1.3));}}
            x=center.at("x");y=center.at("y");
        }
        drawing["points"]=json::array();drawing["snap"]=false;
        if(preset=="rectangle")for(auto p:std::vector<Point>{{x-size*.6,y-size*.4},{x+size*.6,y-size*.4},{x+size*.6,y+size*.4},{x-size*.6,y+size*.4}})drawing["points"].push_back(point(p));
        else if(preset=="triangle")for(auto p:std::vector<Point>{{x,y-size*.55},{x+size*.55,y+size*.4},{x-size*.55,y+size*.4}})drawing["points"].push_back(point(p));
        else for(int i=0;i<32;++i){double a=i*2*3.14159265358979323846/32;drawing["points"].push_back(point({x+size*.5*std::cos(a),y+size*.5*std::sin(a)}));}
    }
    auto points=drawingPoints(drawing,cam);const bool region=op=="draw-region-feature";
    const double eps=1e-5/cam.getScale();
    if(region&&std::hypot(points.front().x-points.back().x,points.front().y-points.back().y)<=eps)points.pop_back();
    require(points.size()>=(region?3:2),region?"Region needs three different vertices":"Line needs two vertices");
    if(region) {
        double area=0;const auto origin=points.front();
        for(size_t i=0;i<points.size();++i){auto a=points[i],b=points[(i+1)%points.size()];area+=(a.x-origin.x)*(b.y-origin.y)-(b.x-origin.x)*(a.y-origin.y);}
        require(std::abs(area)>eps*eps,"Region must have non-zero area");
        for(size_t i=0;i<points.size();++i)for(size_t j=i+1;j<points.size();++j) {
            require(std::hypot(points[i].x-points[j].x,points[i].y-points[j].y)>eps,"Region has a repeated vertex");
            if(j==i+1||(i==0&&j+1==points.size()))continue;
            const auto a=points[i],b=points[(i+1)%points.size()],c=points[j],d=points[(j+1)%points.size()];Point hit;double t;
            require(!crossing(a,b,c,d,hit)&&!onEdge(a,c,d,eps,t)&&!onEdge(b,c,d,eps,t)&&!onEdge(c,a,b,eps,t)&&!onEdge(d,a,b,eps,t),"Region edges must not intersect");
        }
    }
    Line draft;Shape empty;
    for(auto p:points)draft.addObstaclePoint(empty,p);
    if(region)draft.addObstaclePoint(empty,points.front(),draft.obstacleDraftNodes.front());
    draft.finalizeObstacleRoute();
    auto built=region?draft.obstacleLoopRoutes:draft.obstacleNoLoopRoutes;
    require(built.size()==1,"Invalid feature drawing");
    if(region) {
        Region polygon;std::vector<int> indices;
        for(size_t i=0;i<points.size();++i){polygon.nodes.push_back(points[i]);polygon.segments.push_back({int(i),int((i+1)%points.size())});indices.push_back(int(i));}
        polygon.segIdx.push_back(indices);std::vector<Point> loop;
        require(buildRegionPolygonPoints(polygon,0,loop),"Cannot build region boundary");
        built[0]=loop;if(!Geometry::samePoint(built[0].front(),built[0].back()))built[0].push_back(built[0].front());
    }
    const std::string hex=req.value("color",region?std::string("#b6d7a8"):std::string("#9bcde8"));
    require(hex.size()==7&&hex[0]=='#'&&hex.find_first_not_of("0123456789abcdefABCDEF",1)==std::string::npos,"Expected #RRGGBB color");
    json coords=json::array();for(auto p:built[0])coords.push_back(point(state.value("planar",false)?p:webMercToLonLat(p.x,p.y)));
    auto& obstacles=state["obstacles"];if(!obstacles.is_object())obstacles=json::object();
    obstacles["type"]="FeatureCollection";if(!obstacles.contains("features"))obstacles["features"]=json::array();
    require(obstacles["features"].is_array()&&obstacles["features"].size()<100000,"Too many background features");
    long long counter=state.value("nextFeatureId",1LL);std::string id;
    do{id="drawn_feature_"+std::to_string(counter++);}while(std::any_of(obstacles["features"].begin(),obstacles["features"].end(),[&](const auto& f){return f.value("id",json())==id;}));
    state["nextFeatureId"]=counter;
    obstacles["features"].push_back({{"type","Feature"},{"id",id},
        {"properties",{{"name",req.value("name",std::string())},{"featureKind",region?"region":"line"},{"color",hex},{"width",number(req,"width",region?1:30,0.1,1000)}}},
        {"geometry",{{"type",region?"Polygon":"LineString"},{"coordinates",region?json::array({coords}):coords}}}});
}

// An OCTI transaction has a private graph containing transit + line features.
// Region boundaries contribute intersection anchors, not transit routes. The
// binding travels separately from OCTI, whose output discards custom metadata.
struct LayoutNode {Point p;int original=-1;};
struct LayoutEdge {int a,b;std::set<std::string> routes;};
struct LayoutPrimitive {int a,b;std::string route;int transit=-1,feature=-1;};
Point layoutWorld(const json& c,bool planar) {Point p{c.at(0),c.at(1)};return planar?p:lonlatToWebMerc(p.x,p.y);}
json layoutCoordinate(Point p,bool planar){return point(planar?p:webMercToLonLat(p.x,p.y));}
std::vector<Point> layoutPoints(const json& array,bool planar) {std::vector<Point> out;for(const auto& c:array)out.push_back(layoutWorld(c,planar));return out;}
double layoutDistance(Point a,Point b){return std::hypot(a.x-b.x,a.y-b.y);}
// Projection roundoff must not turn a shared straight boundary into a
// transverse crossing. Collinear contacts are tested separately with onEdge.
bool layoutCrossing(Point a,Point b,Point c,Point d,double eps,Point& hit) {
    const double den=(b.x-a.x)*(d.y-c.y)-(b.y-a.y)*(d.x-c.x);
    if(std::abs(den)<=eps*std::max(layoutDistance(a,b),layoutDistance(c,d)))return false;
    return crossing(a,b,c,d,hit);
}
json prepareInsertionTopology(const Shape& shape,const json& state) {
    const double eps=1e-5/cameraFor(state).getScale();
    std::vector<LayoutNode> nodes;for(size_t n=0;n<shape.nodes.size();++n)nodes.push_back({shape.nodes[n].pos,int(n)});
    auto node=[&](Point p){for(size_t i=0;i<nodes.size();++i)if(layoutDistance(p,nodes[i].p)<=eps)return int(i);nodes.push_back({p,-1});return int(nodes.size())-1;};
    std::vector<LayoutPrimitive> primitives;
    std::vector<std::set<std::string>> memberships(shape.segments.size());
    for(const auto& r:shape.routes)for(int e:r.segmentIndices)memberships[e].insert(r.id);
    for(size_t i=0;i<shape.segments.size();++i){const auto& e=shape.segments[i];bool feature=false;for(const auto& r:shape.routes)if(r.isObstacle&&memberships[i].count(r.id))feature=true;primitives.push_back({e.a,e.b,"",int(i),feature?0:-1});}
    const auto obstacles=state.at("obstacles").value("features",json::array());
    json featurePrimitives=json::object();std::set<std::string> routeIds;
    for(const auto& r:shape.routes)routeIds.insert(r.id);
    for(size_t i=0;i<obstacles.size();++i) {
        const auto& f=obstacles[i];const auto props=f.value("properties",json::object());
        if(props.value("featureKind","")!="line")continue;
        if(routeIds.count(props.value("topologyRouteId",std::string())))continue;
        require(f.at("geometry").at("type")=="LineString","Drawn line must be a LineString");
        auto pts=layoutPoints(f["geometry"]["coordinates"],state.value("planar",false));
        std::string id="__tm_line_"+std::to_string(i);while(routeIds.count(id))id+="_";routeIds.insert(id);
        auto& list=featurePrimitives[std::to_string(i)];list=json::array();
        for(size_t j=1;j<pts.size();++j) {
            int a=node(pts[j-1]),b=node(pts[j]);if(a==b)continue;
            list.push_back(primitives.size());primitives.push_back({a,b,id,-1,int(i)});
        }
        require(!list.empty(),"Line feature has no usable segments");
    }
    std::vector<std::vector<std::pair<double,int>>> splits(primitives.size());
    for(size_t i=0;i<primitives.size();++i){const auto e=primitives[i];splits[i]={{0,e.a},{1,e.b}};}
    auto split=[&](size_t i,Point p){const auto e=primitives[i];double t;if(onEdge(p,nodes[e.a].p,nodes[e.b].p,eps,t))splits[i].push_back({std::clamp(t,0.0,1.0),node(p)});};
    // Apply the desktop obstacle-line rule: explicit shared junctions split
    // every incident transit edge. Also retain overlapping feature sections.
    for(size_t i=0;i<primitives.size();++i)for(size_t j=i+1;j<primitives.size();++j) {
        const auto a=primitives[i],b=primitives[j];if(a.feature<0&&b.feature<0)continue;
        const Point p=nodes[a.a].p,q=nodes[a.b].p,r=nodes[b.a].p,s=nodes[b.b].p;Point hit;
        if(layoutCrossing(p,q,r,s,eps,hit)){split(i,hit);split(j,hit);}
        split(i,r);split(i,s);split(j,p);split(j,q);
    }
    // Region contacts become durable A--contact--B constraints in the graph.
    for(const auto& f:obstacles) {
        if(f.value("properties",json::object()).value("featureKind","")!="region")continue;
        require(f.at("geometry").at("type")=="Polygon","Drawn region must be a Polygon");
        for(const auto& ring:f["geometry"]["coordinates"]) {
            auto ps=layoutPoints(ring,state.value("planar",false));
            for(size_t k=1;k<ps.size();++k)for(size_t i=0;i<primitives.size();++i) {
                const auto e=primitives[i];Point hit;
                if(layoutCrossing(ps[k-1],ps[k],nodes[e.a].p,nodes[e.b].p,eps,hit))split(i,hit);
                split(i,ps[k-1]);split(i,ps[k]);
            }
        }
    }
    std::vector<LayoutEdge> edges;std::map<std::pair<int,int>,int> lookup;
    json primitiveChains=json::array();
    for(size_t i=0;i<primitives.size();++i) {
        const auto p=primitives[i];auto entries=splits[i];std::sort(entries.begin(),entries.end());std::vector<int> chain;
        for(auto entry:entries)if(chain.empty()||layoutDistance(nodes[chain.back()].p,nodes[entry.second].p)>eps)chain.push_back(entry.second);
        require(chain.size()>=2,"OCTI input has a collapsed edge");primitiveChains.push_back(chain);
        for(size_t j=1;j<chain.size();++j){auto key=std::minmax(chain[j-1],chain[j]);auto it=lookup.find(key);int ei;
            if(it==lookup.end()){ei=edges.size();edges.push_back({chain[j-1],chain[j],{}});lookup[key]=ei;}else ei=it->second;
            if(p.transit>=0)edges[ei].routes.insert(memberships[p.transit].begin(),memberships[p.transit].end());else edges[ei].routes.insert(p.route);
        }
    }
    json binding={{"version",1},{"nodes",json::array()},{"edges",json::array()},
        {"transitChains",json::array()},{"lineChains",json::object()},{"obstacles",state["obstacles"]},{"planar",state.value("planar",false)}};
    for(const auto& n:nodes)binding["nodes"].push_back({{"position",point(n.p)},{"original",n.original}});
    for(const auto& e:edges)binding["edges"].push_back({{"a",e.a},{"b",e.b},{"routes",e.routes}});
    for(size_t i=0;i<shape.segments.size();++i)binding["transitChains"].push_back(primitiveChains[i]);
    for(auto it=featurePrimitives.begin();it!=featurePrimitives.end();++it){std::vector<int> chain;
        for(const auto& index:it.value()){auto part=primitiveChains[index.get<size_t>()].get<std::vector<int>>();chain.insert(chain.end(),part.begin()+(chain.empty()?0:1),part.end());}
        binding["lineChains"][it.key()]=chain;
    }
    return binding;
}
Point layoutAlong(const std::vector<Point>& ps,double t) {
    double length=0;for(size_t i=1;i<ps.size();++i)length+=layoutDistance(ps[i-1],ps[i]);
    double d=std::clamp(t,0.0,1.0)*length;
    for(size_t i=1;i<ps.size();++i){double l=layoutDistance(ps[i-1],ps[i]);if(d<=l&&l>0)return at(ps[i-1],ps[i],d/l);d-=l;}return ps.back();
}
// Affine moving least squares transfers the displacement field to free region
// vertices. Shared contact vertices/edges bypass interpolation and use the
// exact optimized graph path when station correspondences are available.
Point layoutWarp(Point p,const std::vector<Point>& before,const std::vector<Point>& after,const json& edges,const std::vector<std::vector<Point>>& paths,double eps) {
    for(size_t i=0;i<before.size();++i)if(layoutDistance(p,before[i])<=eps)return after[i];
    for(size_t i=0;i<edges.size();++i){double t;if(onEdge(p,before[edges[i]["a"].get<int>()],before[edges[i]["b"].get<int>()],eps,t))return layoutAlong(paths[i],t);}
    std::vector<double> weights;double sum=0;Point a{0,0},b{0,0};
    for(size_t i=0;i<before.size();++i){double d=layoutDistance(p,before[i]);double w=1/std::max(eps*eps,d*d);weights.push_back(w);sum+=w;a.x+=w*before[i].x;a.y+=w*before[i].y;b.x+=w*after[i].x;b.y+=w*after[i].y;}
    require(sum>0,"Region propagation has no anchors");a.x/=sum;a.y/=sum;b.x/=sum;b.y/=sum;
    double xx=0,xy=0,yy=0,ux=0,uy=0,vx=0,vy=0;
    for(size_t i=0;i<before.size();++i){double x=before[i].x-a.x,y=before[i].y-a.y,u=after[i].x-b.x,v=after[i].y-b.y,w=weights[i];xx+=w*x*x;xy+=w*x*y;yy+=w*y*y;ux+=w*u*x;uy+=w*u*y;vx+=w*v*x;vy+=w*v*y;}
    const double det=xx*yy-xy*xy,dx=p.x-a.x,dy=p.y-a.y;
    if(det>1e-12*(xx+yy)*(xx+yy))return {b.x+((ux*yy-uy*xy)*dx+(uy*xx-ux*xy)*dy)/det,b.y+((vx*yy-vy*xy)*dx+(vy*xx-vx*xy)*dy)/det};
    const double norm=xx+yy;if(norm<=eps*eps)return {p.x+b.x-a.x,p.y+b.y-a.y};
    const double c=(ux+vy)/norm,s=(vx-uy)/norm;return {b.x+c*dx-s*dy,b.y+s*dx+c*dy};
}
// Commit intersection splitting at draw time. This never runs on OCTI output.
Shape commitInsertionTopology(json& state,const json& binding) {
    Shape previous=decode(state.at("shape"));const auto& es=binding.at("edges");
    std::vector<Point> after;for(const auto& n:binding["nodes"])after.push_back({n["position"][0],n["position"][1]});
    const auto& before=after;
    std::map<std::pair<int,int>,int> edgeIds;std::vector<std::vector<Point>> paths;
    for(size_t i=0;i<es.size();++i){int a=es[i]["a"],b=es[i]["b"];edgeIds[std::minmax(a,b)]=i;paths.push_back({after[a],after[b]});}
    auto obstacles=binding.at("obstacles");
    // Rebuild only transit membership. Private feature anchors are ShapePoints;
    // original station identity/type/name and route styles remain authoritative.
    Shape next=previous;for(size_t i=0;i<previous.nodes.size();++i)next.nodes[i].pos=after[i];next.segments.clear();
    std::vector<int> publicNodes(before.size(),-1);for(size_t i=0;i<previous.nodes.size();++i)publicNodes[i]=i;
    auto publicNode=[&](int n){if(publicNodes[n]>=0)return publicNodes[n];ShapeNode v;v.pos=after[n];next.nodes.push_back(v);return publicNodes[n]=int(next.nodes.size())-1;};
    std::map<int,std::vector<int>> publicPaths;std::vector<std::vector<int>> oldChains(previous.segments.size());
    auto publicPath=[&](int ei){if(publicPaths.count(ei))return publicPaths[ei];std::vector<int> ns{publicNode(es[ei]["a"])};for(size_t k=1;k+1<paths[ei].size();++k){ShapeNode n;n.pos=paths[ei][k];next.nodes.push_back(n);ns.push_back(next.nodes.size()-1);}ns.push_back(publicNode(es[ei]["b"]));publicPaths[ei]=ns;return ns;};
    std::map<std::pair<int,int>,int> publicEdges;std::vector<std::vector<int>> oldEdges(previous.segments.size());
    for(size_t old=0;old<previous.segments.size();++old){auto chain=binding["transitChains"][old].get<std::vector<int>>();auto& ns=oldChains[old];for(size_t i=1;i<chain.size();++i){int ei=edgeIds.at(std::minmax(chain[i-1],chain[i]));auto part=publicPath(ei);if(es[ei]["a"]!=chain[i-1])std::reverse(part.begin(),part.end());ns.insert(ns.end(),part.begin()+(ns.empty()?0:1),part.end());}
        for(size_t i=1;i<ns.size();++i){auto key=std::minmax(ns[i-1],ns[i]);auto it=publicEdges.find(key);int ei;if(it==publicEdges.end()){ei=next.segments.size();next.segments.push_back({ns[i-1],ns[i],{}});publicEdges[key]=ei;}else ei=it->second;oldEdges[old].push_back(ei);}}
    for(auto& r:next.routes){std::vector<int> ids;for(int e:r.segmentIndices)ids.insert(ids.end(),oldEdges[e].begin(),oldEdges[e].end());r.segmentIndices=ids;}
    // Persist drawn lines as real obstacle routes in Shape, including their
    // shared intersection nodes. They are editable, but never metro stations.
    for(auto it=binding["lineChains"].begin();it!=binding["lineChains"].end();++it) {
        auto chain=it.value().get<std::vector<int>>();auto& feature=obstacles["features"][std::stoul(it.key())];
        ShapeRoute route;int first=edgeIds.at(std::minmax(chain[0],chain[1]));
        for(const auto& id:es[first]["routes"])if(std::none_of(previous.routes.begin(),previous.routes.end(),[&](const auto& r){return r.id==id;})){route.id=id;break;}
        require(!route.id.empty(),"Missing feature route identity");
        auto props=feature.value("properties",json::object());route.name=props.value("name",std::string("Line feature"));
        route.isObstacle=true;route.obstacleKind=ObstacleKind::Line;route.route_width=props.value("width",12.f);
        auto hex=props.value("color",std::string("#9bcde8"));for(int c=0;c<3;++c)route.color[c]=std::stoi(hex.substr(1+c*2,2),nullptr,16)/255.f;
        for(size_t i=1;i<chain.size();++i){int ei=edgeIds.at(std::minmax(chain[i-1],chain[i]));auto ns=publicPath(ei);
            for(size_t k=1;k<ns.size();++k){auto key=std::minmax(ns[k-1],ns[k]);auto found=publicEdges.find(key);int index;
                if(found==publicEdges.end()){index=next.segments.size();next.segments.push_back({ns[k-1],ns[k],{}});publicEdges[key]=index;}else index=found->second;
                if(std::find(route.segmentIndices.begin(),route.segmentIndices.end(),index)==route.segmentIndices.end())route.segmentIndices.push_back(index);}}
        next.routes.push_back(route);feature["properties"]["topologyRouteId"]=route.id;
    }
    identities(next,state,&previous);state["shapeRevision"]=state.value("shapeRevision",0ULL)+1;
    json directions=json::array();for(const auto& record:state.value("routeTraversals",json::array())){int found=-1;bool forward=true;for(size_t i=0;i<previous.segments.size();++i){const auto e=previous.segments[i];if(previous.nodes[e.a].uid==record["from"]&&previous.nodes[e.b].uid==record["to"]){found=i;break;}if(previous.nodes[e.b].uid==record["from"]&&previous.nodes[e.a].uid==record["to"]){found=i;forward=false;break;}}require(found>=0,"Cannot preserve a route traversal after OCTI");auto ns=oldChains[found];if(!forward)std::reverse(ns.begin(),ns.end());for(size_t i=1;i<ns.size();++i){auto r=record;r["from"]=next.nodes[ns[i-1]].uid;r["to"]=next.nodes[ns[i]].uid;directions.push_back(r);}}state["routeTraversals"]=directions;
    if(state.contains("shapeTraversals")){auto& mapping=state["shapeTraversals"];for(auto& t:mapping["traversals"]){json ns=json::array(),ee=json::array();for(size_t i=0;i<t["orderedSegments"].size();++i){int old=segmentIndex(previous,t["orderedSegments"][i]);auto chain=oldChains[old];auto edges=oldEdges[old];if(previous.nodes[previous.segments[old].a].uid!=t["orderedNodes"][i]){std::reverse(chain.begin(),chain.end());std::reverse(edges.begin(),edges.end());}for(size_t k=ns.empty()?0:1;k<chain.size();++k)ns.push_back(next.nodes[chain[k]].uid);for(int e:edges)ee.push_back(next.segments[e].uid);}t["orderedNodes"]=ns;t["orderedSegments"]=ee;}mapping["revision"]=state["shapeRevision"];}
    state["obstacles"]=obstacles;
    // Fit both the optimized network and its propagated background together.
    Camera cam;cam.resize(1200,800);GeoData data;Route bounds;for(auto p:after)bounds.segments.push_back({p});for(const auto& f:obstacles.value("features",json::array())){const auto kind=f.value("properties",json::object()).value("featureKind","");if(kind=="line"){auto parts=f["geometry"]["type"]=="MultiLineString"?f["geometry"]["coordinates"]:json::array({f["geometry"]["coordinates"]});for(const auto& part:parts)for(auto p:layoutPoints(part,binding.value("planar",false)))bounds.segments.push_back({p});}if(kind=="region")for(const auto& ring:f["geometry"]["coordinates"])for(auto p:layoutPoints(ring,binding.value("planar",false)))bounds.segments.push_back({p});}data.routes.push_back(bounds);cam.fitToData(data);auto center=cam.screenToWorld(600,400);state["transform"]={{"x",center.x},{"y",center.y},{"scale",cam.getScale()}};
    return next;
}

void syncFeatureGeometry(const Shape& shape,json& state) {
    auto canonical=Shape2StyleShape(shape);json retained=json::array();
    for(auto f:state["obstacles"].value("features",json::array())) {
        const auto id=f.value("properties",json::object()).value("topologyRouteId",std::string());
        if(id.empty()){retained.push_back(f);continue;}
        auto r=std::find_if(shape.routes.begin(),shape.routes.end(),[&](const auto& route){return route.id==id;});
        if(r==shape.routes.end())continue;
        json paths=json::array();for(const auto& path:canonical.route_path_topology[r-shape.routes.begin()]){json ps=json::array();for(int n:path.nodes)ps.push_back(layoutCoordinate(shape.nodes[n].pos,state.value("planar",false)));if(ps.size()>1)paths.push_back(ps);}
        if(paths.empty())continue;
        f["geometry"]={{"type",paths.size()==1?"LineString":"MultiLineString"},{"coordinates",paths.size()==1?paths[0]:paths}};retained.push_back(f);
    }
    state["obstacles"]["features"]=retained;
}
Shape integrateFeatures(Shape shape,json& state) {
    const auto transform=state["transform"];auto canonical=Shape2StyleShape(shape);
    state["shape"]=encode(shape,cameraFor(state),canonical,state);
    auto binding=prepareInsertionTopology(shape,state);
    auto result=commitInsertionTopology(state,binding);
    state["transform"]=transform;syncFeatureGeometry(result,state);return result;
}

// Follow the desktop RUN pipeline: regular Shape exporter -> unmodified OCTI
// -> regular loader. Only semantic/style metadata is restored on the result.
json nativeLayoutInput(const Shape& shape) {
    auto graph=shapeToLoomGeoJson(shape,true);
    for(auto& f:graph["features"])if(f["geometry"]["type"]=="Point") {
        auto& p=f["properties"];
        if(p.value("kind",std::string())=="station"&&!p.contains("station_id"))p["station_id"]=p["id"];
    }
    return {{"map",graph}};
}
std::vector<Point> nativeRoutePath(const Shape& shape,const ShapeRoute& route,int from,int to) {
    std::vector<std::vector<int>> adj(shape.nodes.size());for(int e:route.segmentIndices){adj[shape.segments[e].a].push_back(shape.segments[e].b);adj[shape.segments[e].b].push_back(shape.segments[e].a);}
    std::vector<double> dist(shape.nodes.size(),1e100);std::vector<int> prev(shape.nodes.size(),-1);std::set<std::pair<double,int>> queue;
    dist[from]=0;queue.insert({0,from});
    while(!queue.empty()){auto [d,n]=*queue.begin();queue.erase(queue.begin());if(n==to)break;if(d!=dist[n])continue;
        for(int k:adj[n]){double nd=d+layoutDistance(shape.nodes[n].pos,shape.nodes[k].pos);if(nd<dist[k]){queue.erase({dist[k],k});dist[k]=nd;prev[k]=n;queue.insert({nd,k});}}}
    if(from!=to&&prev[to]<0)return {};
    std::vector<Point> result;for(int n=to;n>=0;n=prev[n]){result.push_back(shape.nodes[n].pos);if(n==from)break;}std::reverse(result.begin(),result.end());return result;
}
std::vector<Point> nativePathInterval(const std::vector<Point>& path,double start,double end) {
    double len=0;for(size_t k=1;k<path.size();++k)len+=layoutDistance(path[k-1],path[k]);
    std::vector<Point> result{layoutAlong(path,start)};double d=0;
    for(size_t k=1;k<path.size();++k){d+=layoutDistance(path[k-1],path[k]);if(d>start*len&&d<end*len)result.push_back(path[k]);}
    result.push_back(layoutAlong(path,end));return result;
}
Shape applyNativeLayout(json& state,json output) {
    const auto previous=decode(state.at("shape"));
    for(auto& f:output["features"])if(f["geometry"]["type"]=="LineString")for(auto& line:f["properties"]["lines"]){
        auto old=std::find_if(previous.routes.begin(),previous.routes.end(),[&](const auto& r){return r.id==identifier(line["id"]);});
        if(old!=previous.routes.end()){line["name"]=old->name;line["label"]=old->name;line["color"]=color(old->color).substr(1);line["route_width"]=old->route_width;}}
    Shape next=loadMap(output);
    for(auto& route:next.routes){auto old=std::find_if(previous.routes.begin(),previous.routes.end(),[&](const auto& r){return r.id==route.id;});
        if(old!=previous.routes.end()){route.name=old->name;route.route_width=old->route_width;route.isObstacle=old->isObstacle;route.obstacleKind=old->obstacleKind;for(int c=0;c<3;++c)route.color[c]=old->color[c];}}
    // Feature-only nodes stay ShapePoints, including optimizer-added bends.
    std::vector<bool> transit(next.nodes.size(),false),feature(next.nodes.size(),false);
    for(const auto& route:next.routes)for(int e:route.segmentIndices)for(int n:{next.segments[e].a,next.segments[e].b})(route.isObstacle?feature:transit)[n]=true;
    for(size_t i=0;i<next.nodes.size();++i)if(feature[i]&&!transit[i]){next.nodes[i].type=ShapeNodeType::ShapePoint;next.nodes[i].name.clear();next.nodes[i].station_id.clear();}
    // Regions follow the resulting layout, without changing optimizer geometry.
    std::vector<Point> before,after;std::map<int,int> matched;
    std::map<std::string,int> stationIds;
    for(size_t i=0;i<next.nodes.size();++i)if(next.nodes[i].type==ShapeNodeType::Station&&!next.nodes[i].station_id.empty())stationIds[next.nodes[i].station_id]=i;
    for(size_t i=0;i<previous.nodes.size();++i)if(previous.nodes[i].type==ShapeNodeType::Station){const auto& n=previous.nodes[i];auto it=stationIds.find(n.station_id.empty()?n.uid:n.station_id);
        if(it!=stationIds.end()){matched[i]=it->second;before.push_back(n.pos);after.push_back(next.nodes[it->second].pos);}}
    json edges=json::array();std::vector<std::vector<Point>> paths;
    auto oldCanonical=Shape2StyleShape(previous);
    for(size_t ri=0;ri<previous.routes.size();++ri){auto route=std::find_if(next.routes.begin(),next.routes.end(),[&](const auto& r){return r.id==previous.routes[ri].id;});if(route==next.routes.end())continue;
        for(const auto& chain:oldCanonical.route_path_topology[ri]){int start=-1;
            for(size_t k=0;k<chain.nodes.size();++k)if(matched.count(chain.nodes[k])){
                if(start>=0){auto path=nativeRoutePath(next,*route,matched.at(chain.nodes[start]),matched.at(chain.nodes[k]));
                    double length=0;for(size_t j=start+1;j<=k;++j)length+=layoutDistance(previous.nodes[chain.nodes[j-1]].pos,previous.nodes[chain.nodes[j]].pos);
                    if(path.size()>1&&length>0){double d=0;
                        for(size_t j=start+1;j<=k;++j){auto a=previous.nodes[chain.nodes[j-1]].pos,b=previous.nodes[chain.nodes[j]].pos;double end=d+layoutDistance(a,b);auto part=nativePathInterval(path,d/length,end/length);
                            int ai=before.size();before.push_back(a);after.push_back(part.front());int bi=before.size();before.push_back(b);after.push_back(part.back());edges.push_back({{"a",ai},{"b",bi}});paths.push_back(part);d=end;}}
                }start=k;}
        }
    }
    double regionStep=1; if(!before.empty()){auto lo=before.front(),hi=lo;for(auto p:before){lo.x=std::min(lo.x,p.x);lo.y=std::min(lo.y,p.y);hi.x=std::max(hi.x,p.x);hi.y=std::max(hi.y,p.y);}regionStep=std::max(1e-6,layoutDistance(lo,hi)/100);}
    for(auto& f:state["obstacles"]["features"])if(f.value("properties",json::object()).value("featureKind","")=="region"){
        require(!before.empty(),"Region propagation needs matching network stations");
        for(auto& ring:f["geometry"]["coordinates"]){auto original=layoutPoints(ring,state.value("planar",false));json mapped=json::array();
            for(size_t i=1;i<original.size();++i){auto a=original[i-1],b=original[i];std::vector<double> ts;int samples=std::clamp(int(std::ceil(layoutDistance(a,b)/regionStep)),1,16);for(int k=0;k<samples;++k)ts.push_back(double(k)/samples);
                for(const auto& e:edges){Point hit;if(layoutCrossing(a,b,before[e["a"].get<int>()],before[e["b"].get<int>()],1e-6,hit))ts.push_back(std::clamp(along(hit,a,b),0.0,1.0));}
                std::sort(ts.begin(),ts.end());for(double t:ts)if(t<1)mapped.push_back(layoutCoordinate(layoutWarp(at(a,b,t),before,after,edges,paths,1e-6),state.value("planar",false)));}
            if(!mapped.empty())mapped.push_back(mapped.front());ring=mapped;
        }
    }
    state["pathIds"]=json::object();identities(next,state);state["shapeRevision"]=state.value("shapeRevision",0ULL)+1;
    // Default OCTI contracts ShapePoints. Compose explicit directed records
    // through those removed vertices before expanding them onto native paths.
    auto records=state.value("routeTraversals",json::array());json composed=json::array();
    std::set<std::string> surviving;for(const auto& entry:matched)surviving.insert(previous.nodes[entry.first].uid);
    std::map<std::pair<std::string,std::string>,std::vector<size_t>> outgoing;
    for(size_t i=0;i<records.size();++i)outgoing[{records[i]["routeId"].get<std::string>(),records[i]["from"].get<std::string>()}].push_back(i);
    std::set<std::string> emitted;
    for(const auto& record:records)if(surviving.count(record["from"].get<std::string>())) {
        const auto route=record["routeId"].get<std::string>(),start=record["from"].get<std::string>();
        std::vector<std::string> pending{record["to"].get<std::string>()};std::set<std::string> seen;
        while(!pending.empty()){auto n=pending.back();pending.pop_back();if(!seen.insert(n).second)continue;
            if(surviving.count(n)){if(n!=start){auto r=record;r["to"]=n;if(emitted.insert(r.dump()).second)composed.push_back(r);}continue;}
            for(size_t i:outgoing[{route,n}])pending.push_back(records[i]["to"].get<std::string>());
        }
    }
    auto directionSource=previous;for(auto& n:directionSource.nodes)if(n.type==ShapeNodeType::Station&&n.station_id.empty())n.station_id=n.uid;
    state["routeTraversals"]=remapRouteDirections(directionSource,next,composed);
    state.erase("shapeTraversals");if(state.contains("directionalData"))state["shapeTraversals"]=mapGtfsDirections(next,state["directionalData"],state["shapeRevision"]);
    syncFeatureGeometry(next,state);
    Camera cam;cam.resize(1200,800);GeoData data;Route bounds;for(const auto& n:next.nodes)bounds.segments.push_back({n.pos});data.routes.push_back(bounds);cam.fitToData(data);auto center=cam.screenToWorld(600,400);
    state["transform"]={{"x",center.x},{"y",center.y},{"scale",cam.getScale()}};
    return next;
}

// Render background boundaries through the same C++ bend engine as routes.
// These temporary paths never enter the authoritative topology or OCTI input.
json featureCommands(const json& state,const json& style,const Camera& cam) {
    json result=json::object();size_t fi=0;
    for(const auto& f:state.at("obstacles").value("features",json::array())) {
        auto props=f.value("properties",json::object());auto kind=props.value("featureKind",std::string());
        auto id=std::to_string(fi++);if(kind!="line"&&kind!="region")continue;
        const auto& g=f.at("geometry");auto parts=g["type"]=="LineString"?json::array({g["coordinates"]}):g["coordinates"];
        json commands=json::array();
        for(const auto& part:parts) {
            auto points=layoutPoints(part,state.value("planar",false));bool closed=kind=="region";
            if(closed&&points.size()>1&&layoutDistance(points.front(),points.back())<1e-9)points.pop_back();
            if(points.size()<2)continue;
            // Put the closed-path seam on a straight edge so every polygon
            // corner is an interior bend in the route command generator.
            if(closed)points.insert(points.begin(),Point{(points.front().x+points.back().x)/2,(points.front().y+points.back().y)/2});
            Shape boundary;ShapeRoute route{};route.id="boundary";route.name="boundary";
            for(size_t i=0;i<points.size();++i){ShapeNode n;n.id=i;n.uid=std::to_string(i);n.pos=points[i];boundary.nodes.push_back(n);
                if(i){route.segmentIndices.push_back(boundary.segments.size());boundary.segments.push_back({int(i-1),int(i),{}});}}
            if(closed){route.segmentIndices.push_back(boundary.segments.size());boundary.segments.push_back({int(points.size()-1),0,{}});}
            boundary.routes.push_back(route);auto styled=Shape2StyleShape(boundary);
            geometryStyle(styled,boundary,{{"routes",{{"boundary",style.value("featureStyle",json::object())}}}});
            auto display=buildStyledShapeDisplayData(styled,cam);
            for(const auto& path:display.commands[0]){
                for(const auto& c:path){json v={{"type",c.type==RenderCommand::Type::Move?"move":c.type==RenderCommand::Type::Line?"line":"quadratic"},{"x",c.end.x},{"y",800-c.end.y}};
                    if(c.type==RenderCommand::Type::Quadratic){v["cx"]=c.control.x;v["cy"]=800-c.control.y;}commands.push_back(v);}
                if(closed)commands.push_back({{"type","close"}});
            }
        }
        result[id]=commands;
    }
    return result;
}
// Region editing runs in canvas units, just like desktop interaction, then
// converts back to the map CRS. The view camera is never refitted by a drag.
Region editableRegions(const json& state,const Camera& cam) {
    Region region;
    for(const auto& f:state.at("obstacles").at("features")) {
        if(f.value("properties",json::object()).value("featureKind","")!="region")continue;
        require(f.at("geometry").at("type")=="Polygon","Region must be a Polygon");
        require(f["geometry"]["coordinates"].size()==1,"Region editing requires a single outer boundary");
        auto ps=layoutPoints(f["geometry"]["coordinates"][0],state.value("planar",false));
        if(ps.size()>1&&layoutDistance(ps.front(),ps.back())<1e-8)ps.pop_back();
        require(ps.size()>=3,"Region needs three vertices");
        int start=region.nodes.size();std::vector<int> edges;
        for(auto p:ps)region.nodes.push_back(screen(cam,p));
        for(int i=0;i<int(ps.size());++i){edges.push_back(region.segments.size());region.segments.push_back({start+i,start+(i+1)%int(ps.size())});}
        region.segIdx.push_back(edges);
    }
    return region;
}
void storeRegions(json& state,const Region& region,const Camera& cam) {
    int polygon=0;
    for(auto& f:state["obstacles"]["features"]) {
        if(f.value("properties",json::object()).value("featureKind","")!="region")continue;
        std::vector<Point> loop;require(buildRegionPolygonPoints(region,polygon++,loop),"Invalid region boundary");
        json coords=json::array();for(auto p:loop)coords.push_back(layoutCoordinate(cam.screenToWorld(p.x,800-p.y),state.value("planar",false)));
        if(coords.front()!=coords.back())coords.push_back(coords.front());
        f["geometry"]["coordinates"]=json::array({coords});
    }
}
bool strictlyInsideRegion(Point p,const std::vector<Point>& polygon) {
    if(!Geometry::pointInPolygon(p,polygon))return false;
    for(size_t i=0;i<polygon.size();++i) {
        auto a=polygon[i],b=polygon[(i+1)%polygon.size()];
        double dx=b.x-a.x,dy=b.y-a.y,d=dx*dx+dy*dy;
        double t=d>1e-12?std::clamp(((p.x-a.x)*dx+(p.y-a.y)*dy)/d,0.0,1.0):0;
        if(std::hypot(p.x-a.x-t*dx,p.y-a.y-t*dy)<1e-6)return false;
    }
    return true;
}
void pushOutside(Shape& shape,const Region& region) {
    std::vector<std::vector<Point>> polygons;buildRegionPolygons(region,polygons);
    for(auto& node:shape.nodes) {
        // A zero direction selects the nearest boundary in the desktop helper.
        // Boundary contacts stay put, avoiding round-trip floating-point drift.
        for(size_t pass=0;pass<polygons.size()*4+1;++pass) {
            bool moved=false;
            for(const auto& polygon:polygons)if(strictlyInsideRegion(node.pos,polygon)) {
                node.pos=movePointOutsideRegionPolygonsAlongDirection(node.pos,{polygon},{0,0});moved=true;
            }
            if(!moved)break;
        }
        for(const auto& polygon:polygons)require(!strictlyInsideRegion(node.pos,polygon),"Overlapping regions leave no nearby free position");
    }
}
void alignNearbyRegions(Region& region,const Shape& shape) {
    // Use the nearest transit direction within 100 canvas units as the local
    // octilinear frame, then reuse the desktop boundary alignment routine.
    for(size_t i=0;i<region.segIdx.size();++i) {
        std::vector<int> loop;buildRegionLoopNodeIndices(region,int(i),loop);
        if(loop.size()>1&&loop.front()==loop.back())loop.pop_back();
        double best=100.0,angle=0;
        for(const auto& route:shape.routes)if(!route.isObstacle)for(int e:route.segmentIndices) {
            const auto& edge=shape.segments[e];auto a=shape.nodes[edge.a].pos,b=shape.nodes[edge.b].pos;
            double dx=b.x-a.x,dy=b.y-a.y,len2=dx*dx+dy*dy;if(len2<1e-8)continue;
            for(int n:loop){auto p=region.nodes[n];double t=std::clamp(((p.x-a.x)*dx+(p.y-a.y)*dy)/len2,0.0,1.0);
                double d=std::hypot(p.x-a.x-t*dx,p.y-a.y-t*dy);
                if(d<best){best=d;angle=std::remainder(std::atan2(dy,dx),pi/4);}}
        }
        Region local;Point origin=region.nodes[loop.front()];
        auto rotate=[](Point p,double a){return Point{p.x*std::cos(a)-p.y*std::sin(a),p.x*std::sin(a)+p.y*std::cos(a)};};
        std::vector<int> edges;
        for(int n:loop){auto p=region.nodes[n];local.nodes.push_back(rotate({p.x-origin.x,p.y-origin.y},-angle));}
        for(int n=0;n<int(loop.size());++n){edges.push_back(n);local.segments.push_back({n,(n+1)%int(loop.size())});}local.segIdx.push_back(edges);
        refineRegionToOctilinear(local);
        for(size_t n=0;n<loop.size();++n){auto p=rotate(local.nodes[n],angle);region.nodes[loop[n]]={origin.x+p.x,origin.y+p.y};}
    }
}
void preserveRegionTopology(const Shape& before,const Shape& after) {
    // Region Apply only changes positions of existing nodes.
    require(after.nodes.size()==before.nodes.size(),"Region Apply changed the network node count");
    require(after.segments.size()==before.segments.size(),"Region Apply changed the segment count");
    for(size_t i=0;i<before.segments.size();++i)
        require(before.segments[i].a==after.segments[i].a&&before.segments[i].b==after.segments[i].b,"Region Apply changed a segment connection");
    const int count=before.nodes.size();
    std::vector<std::vector<int>> adj(after.nodes.size());
    for(size_t i=0;i<after.segments.size();++i){const auto& e=after.segments[i];adj[e.a].push_back(i);adj[e.b].push_back(i);}
    for(size_t n=count;n<adj.size();++n)require(adj[n].size()==2&&!isStationLike(after.nodes[n].type),"Region Apply would create a new junction; move the region and retry");
    std::map<std::pair<int,int>,int> original;
    for(size_t i=0;i<before.segments.size();++i){const auto& e=before.segments[i];original[std::minmax(e.a,e.b)]=i;}
    std::vector<int> parent(after.segments.size(),-1);
    std::vector<std::set<std::string>> oldRoutes(before.segments.size()),newRoutes(after.segments.size());
    for(const auto& r:before.routes)for(int e:r.segmentIndices)oldRoutes[e].insert(r.id);
    for(const auto& r:after.routes)for(int e:r.segmentIndices)newRoutes[e].insert(r.id);
    std::set<int> retained;
    for(int n=0;n<count;++n)for(int first:adj[n]) {
        if(parent[first]>=0)continue;
        int at=n,e=first;std::vector<int> chain;
        for(size_t guard=0;guard<=after.segments.size();++guard) {
            chain.push_back(e);const auto& edge=after.segments[e];at=edge.a==at?edge.b:edge.a;
            if(at<count)break;
            e=adj[at][0]==e?adj[at][1]:adj[at][0];
        }
        auto it=original.find(std::minmax(n,at));
        require(at<count&&it!=original.end(),"Region Apply would change network connectivity");
        require(retained.insert(it->second).second,"Region Apply would duplicate a connection");
        for(int part:chain){parent[part]=it->second;require(newRoutes[part]==oldRoutes[it->second],"Region Apply would change shared route membership");}
    }
    require(retained.size()==before.segments.size(),"Region Apply would remove a connection");
    auto crosses=[](const Shape& s,const ShapeSegment& a,const ShapeSegment& b){
        if(a.a==b.a||a.a==b.b||a.b==b.a||a.b==b.b)return false;
        auto p=s.nodes[a.a].pos,q=s.nodes[a.b].pos,r=s.nodes[b.a].pos,t=s.nodes[b.b].pos;
        auto cross=[](Point a,Point b,Point c){return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);};
        // Projection residuals near collinear contacts are not new crossings.
        double pq=std::hypot(q.x-p.x,q.y-p.y),rt=std::hypot(t.x-r.x,t.y-r.y);
        if(pq<1e-8||rt<1e-8)return false;
        auto opposite=[](double a,double b){return (a>1e-5&&b<-1e-5)||(b>1e-5&&a<-1e-5);};
        return opposite(cross(p,q,r)/pq,cross(p,q,t)/pq)&&opposite(cross(r,t,p)/rt,cross(r,t,q)/rt);
    };
    for(size_t i=0;i<after.segments.size();++i)for(size_t j=i+1;j<after.segments.size();++j) {
        require(parent[i]>=0&&parent[j]>=0,"Region Apply created an isolated connection");
        if(crosses(after,after.segments[i],after.segments[j]))
            require(parent[i]!=parent[j]&&crosses(before,before.segments[parent[i]],before.segments[parent[j]]),"Region Apply would introduce a network crossing; move the region and retry");
    }
}
json regionGeometry(const json& state) {
    json geometry=json::array();for(const auto& f:state["obstacles"]["features"])if(f.value("properties",json::object()).value("featureKind",std::string())=="region")geometry.push_back(f["geometry"]);return geometry;
}
void editRegions(Shape& shape,json& state,const json& req,const Camera& cam,bool apply) {
    if(apply&&state.value("regionPropagationVersion",0)==3&&state.value("regionAppliedGeometry",json())==regionGeometry(state))return;
    if(!apply)state.erase("regionAppliedGeometry");
    if(!state.contains("regionBaseline"))state["regionBaseline"]=state["shape"];
    Shape baseline=decode(state["regionBaseline"]);
    for(auto& n:baseline.nodes)n.pos=screen(cam,n.pos);
    if(!apply) {
        const auto id=identifier(req.at("featureId"));
        auto& features=state["obstacles"]["features"];size_t index=features.size();
        for(size_t i=0;i<features.size();++i)if(std::to_string(i)==id){index=i;break;}
        require(index<features.size(),"Unknown region feature");
        auto& f=features[index];require(f.value("properties",json::object()).value("featureKind","")=="region","Feature is not a region");
        const auto op=req.at("op").get<std::string>();
        if(op=="delete-region-feature") {
            features.erase(features.begin()+index);shape=baseline;
            auto remaining=editableRegions(state,cam);pushOutside(shape,remaining);
            if(remaining.segIdx.empty())state.erase("regionBaseline");
            for(auto& n:shape.nodes)n.pos=cam.screenToWorld(n.pos.x,800-n.pos.y);
            return;
        }
        for(auto& ring:f["geometry"]["coordinates"]) {
            auto ps=layoutPoints(ring,state.value("planar",false));bool closed=ps.size()>1&&std::hypot(ps.front().x-ps.back().x,ps.front().y-ps.back().y)<1e-9;if(closed)ps.pop_back();
            for(auto& p:ps)p=screen(cam,p);
            if(op=="move-region-feature") {
                auto a=req.at("from"),b=req.at("to");double dx=number(b,"x",0,-1e7,1e7)-number(a,"x",0,-1e7,1e7),dy=number(b,"y",0,-1e7,1e7)-number(a,"y",0,-1e7,1e7);
                for(auto& p:ps){p.x+=dx;p.y+=dy;}
            } else if(op=="scale-region-feature") {
                double delta=0;
                if(req.contains("wheelEvents")) {
                    const auto& events=req.at("wheelEvents");require(events.is_array()&&!events.empty()&&events.size()<=2048,"Invalid region wheel gesture");
                    for(const auto& event:events){double d=number(event,"delta",0,-1000,1000);int mode=event.value("mode",0);delta+=d*(mode==1?16:mode==2?800:1);}
                } else {delta=number(req,"wheelDelta",0,-1000,1000);int mode=req.value("deltaMode",0);delta*=mode==1?16:mode==2?800:1;}
                delta=std::clamp(delta,-2300.0,2300.0);double factor=std::exp(-delta*.001);
                auto c=RegionDeformation::center(ps);for(auto& p:ps){p.x=c.x+(p.x-c.x)*factor;p.y=c.y+(p.y-c.y)*factor;}
            } else if(op=="reshape-region-feature") {
                auto a=req.at("from"),b=req.at("to");Point d{number(b,"x",0,-1e7,1e7)-number(a,"x",0,-1e7,1e7),number(b,"y",0,-1e7,1e7)-number(a,"y",0,-1e7,1e7)};
                int n=req.value("vertexIndex",req.value("edgeIndex",-1));require(n>=0&&n<int(ps.size()),"Unknown region handle");
                if(req.contains("vertexIndex")){ps[n].x+=d.x;ps[n].y+=d.y;}
                else {int next=(n+1)%ps.size();auto t=RegionDeformation::sub(ps[next],ps[n]);double len2=RegionDeformation::dot(t,t);require(len2>1e-8,"Region edge is too short");Point normal{-t.y,t.x};double distance=RegionDeformation::dot(d,normal)/len2;for(int k:{n,next}){ps[k].x+=normal.x*distance;ps[k].y+=normal.y*distance;}}
            }
            require(std::abs(RegionDeformation::polygonAreaSigned(ps))>4,"Region is too small");
            for(size_t i=0;i<ps.size();++i){auto a=ps[i],b=ps[(i+1)%ps.size()];require(std::hypot(a.x-b.x,a.y-b.y)>.5,"Region edge is too short");for(size_t j=i+1;j<ps.size();++j){if(j==i+1||(i==0&&j+1==ps.size()))continue;auto c=ps[j],d=ps[(j+1)%ps.size()];require(!(RegionDeformation::cross(a,b,c)*RegionDeformation::cross(a,b,d)<-1e-8&&RegionDeformation::cross(c,d,a)*RegionDeformation::cross(c,d,b)<-1e-8),"Region edges must not intersect");}}
            ring=json::array();for(auto p:ps)ring.push_back(layoutCoordinate(cam.screenToWorld(p.x,800-p.y),state.value("planar",false)));ring.push_back(ring.front());
        }
    }
    auto region=editableRegions(state,cam);require(!region.segIdx.empty(),"No region features to apply");
    shape=baseline;
    if(apply) {
        RegionDeformation::apply(shape,baseline,region);
        std::vector<std::vector<Point>> polygons;buildRegionPolygons(region,polygons);
        for(const auto& e:shape.segments)
            require(!segmentIntersectsAnyRegionPolygonInterior(shape.nodes[e.a].pos,shape.nodes[e.b].pos,polygons),"A region still overlaps its cell boundary");
        preserveRegionTopology(baseline,shape);
        state.erase("regionBaseline");
    } else pushOutside(shape,region);
    storeRegions(state,region,cam);
    if(apply){state["regionAppliedGeometry"]=regionGeometry(state);state["regionPropagationVersion"]=3;}
    for(auto& n:shape.nodes)n.pos=cam.screenToWorld(n.pos.x,800-n.pos.y);
}

void updateRoutes(Shape& shape,json& state,const json& request) {
    const auto& changes=request.at("routes");require(changes.is_array()&&!changes.empty(),"No route changes");
    std::map<std::string,std::string> ids,names;
    for(const auto& c:changes){auto old=identifier(c.at("routeId")),id=identifier(c.at("id"));routeIndex(shape,old);require(!id.empty()&&id.size()<=256&&id.find_first_not_of(" \t\r\n")!=std::string::npos,"Route ID must be non-empty and at most 256 characters");require(!ids.count(old),"Repeated route update");ids[old]=id;names[old]=c.value("name",id);}
    std::set<std::string> used;for(const auto& r:shape.routes)require(used.insert(ids.count(r.id)?ids.at(r.id):r.id).second,"Route ID already exists");
    // Rewrite only semantic route references, preserving stop IDs and geometry.
    auto rewrite=[&](auto&& self,json& value)->void {
        if(value.is_array()){for(auto& x:value)self(self,x);return;}if(!value.is_object())return;
        for(auto it=value.begin();it!=value.end();++it){
            if((it.key()=="routeId"||it.key()=="logicalRouteId"||it.key()=="topologyRouteId")&&it.value().is_string()&&ids.count(it.value().get<std::string>()))it.value()=ids.at(it.value().get<std::string>());
            else if((it.key()=="routeIds"||it.key()=="visibleRouteIds"||it.key()=="bidirectionalRoutes")&&it.value().is_array()){for(auto& id:it.value())if(id.is_string()&&ids.count(id.get<std::string>()))id=ids.at(id.get<std::string>());}
            else self(self,it.value());
        }
    };
    for(const auto* key:{"directionalData","shapeTraversals","routeTraversals","obstacles"})if(state.contains(key))rewrite(rewrite,state[key]);
    for(auto& id:state["bidirectionalRoutes"])if(ids.count(id.get<std::string>()))id=ids.at(id.get<std::string>());
    for(auto& st:state["styles"]){auto old=st.value("routes",json::object()),next=json::object();for(auto it=old.begin();it!=old.end();++it)next[ids.count(it.key())?ids.at(it.key()):it.key()]=it.value();st["routes"]=next;
        for(auto& pair:st["pairs"])for(const auto* k:{"a","b"})if(ids.count(pair[k].get<std::string>()))pair[k]=ids.at(pair[k].get<std::string>());}
    for(auto& r:shape.routes)if(ids.count(r.id)){auto old=r.id;r.id=ids.at(old);r.name=names.at(old);}
}

void edit(Shape& s, const std::string& op, const json& req, const Camera& cam) {
    if(op=="move-node") {
        int n=nodeIndex(s,req.at("nodeId")); Point proposed=fromScreen(cam,req);
        if(req.value("orthogonal",false)) {
            const auto origin=s.nodes[n].pos;
            const auto axis=req.value("axis",std::string());
            if(axis=="x"||(axis!="y"&&std::abs(proposed.x-origin.x)>=std::abs(proposed.y-origin.y)))proposed.y=origin.y;
            else proposed.x=origin.x;
            s.nodes[n].pos=proposed;
        } else s.nodes[n].pos=req.value("snap",true)?snapShapeStationPosition(s,n,proposed,pi):proposed;
    } else if(op=="delete-node") {
        require(deleteShapeNodes(s,{nodeIndex(s,req.at("nodeId"))}),"Delete failed");
    } else if(op=="merge-stations") {
        std::vector<int> ns;for(const auto& id:req.at("nodeIds"))ns.push_back(nodeIndex(s,id));
        require(ns.size()>=2,"Select at least two stations");
        std::sort(ns.begin(),ns.end());const auto keep=s.nodes[ns.front()].uid;
        bool station=false;std::string name,stationId;
        for(int n:ns)if(s.nodes[n].type==ShapeNodeType::Station){station=true;name=s.nodes[n].name;stationId=s.nodes[n].station_id;break;}
        for(int n:ns)s.nodes[n].type=ShapeNodeType::Station;
        require(mergeStations(s,ns,station?req.value("name",name):"",station?req.value("stationId",stationId):""),"Merge failed");
        auto& merged=s.nodes[nodeIndex(s,keep)];merged.type=station?ShapeNodeType::Station:ShapeNodeType::ShapePoint;
    } else if(op=="split-station") {
        int n=nodeIndex(s,req.at("nodeId"));std::vector<char> mask(s.routes.size(),0);
        for(const auto& id:req.at("routeIds"))mask[routeIndex(s,id)]=1;
        std::set<int> attached;for(size_t r=0;r<s.routes.size();++r)for(int e:s.routes[r].segmentIndices)if(s.segments[e].a==n||s.segments[e].b==n)attached.insert(int(r));
        size_t moved=0;for(int r:attached)moved+=mask[r]!=0;
        require(moved>0 && moved<attached.size(),"Split must leave routes on both stations");
        require(splitStation(s,n,s.nodes[n].name,s.nodes[n].station_id,req.value("name",s.nodes[n].name+" B"),req.value("stationId",s.nodes[n].station_id+"_B"),mask,number(req,"offset",12,1,1000)/cam.getScale()),"Split failed");
    } else if(op=="split-segment" || op=="add-station") {
        if(req.contains("nodeId") && op=="add-station") {
            auto& n=s.nodes[nodeIndex(s,req.at("nodeId"))];n.type=ShapeNodeType::Station;n.name=req.value("name","Station");n.station_id=req.value("stationId",n.uid);
        } else {
            int ei=segmentIndex(s,req.at("segmentId")); auto e=s.segments[ei];
            Point mid{(s.nodes[e.a].pos.x+s.nodes[e.b].pos.x)/2,(s.nodes[e.a].pos.y+s.nodes[e.b].pos.y)/2};
            require(splitShapeSegmentAtMidpoint(s,ei),"Cannot split this segment");
            if(op=="add-station")for(auto& n:s.nodes)if(std::hypot(n.pos.x-mid.x,n.pos.y-mid.y)<1e-6){n.type=ShapeNodeType::Station;n.name=req.value("name","Station");n.station_id=req.value("stationId","");break;}
        }
    } else if(op=="add-route") {
        ShapeRoute route; route.id=identifier(req.at("routeId")); route.name=req.value("name",route.id);
        require(std::none_of(s.routes.begin(),s.routes.end(),[&](const auto& r){return r.id==route.id;}),"Route ID already exists");
        route.color[0]=.2f;route.color[1]=.4f;route.color[2]=.7f;
        if(req.contains("segmentIds"))for(const auto& id:req["segmentIds"])route.segmentIndices.push_back(segmentIndex(s,id));
        if(req.contains("nodeIds")) {
            const auto& ids=req["nodeIds"];require(ids.size()>=2,"Select at least two nodes");
            for(size_t i=1;i<ids.size();++i) {
                int a=nodeIndex(s,ids[i-1]),b=nodeIndex(s,ids[i]); require(a!=b,"Repeated route node");
                int found=-1;for(size_t e=0;e<s.segments.size();++e)if((s.segments[e].a==a&&s.segments[e].b==b)||(s.segments[e].a==b&&s.segments[e].b==a)){found=int(e);break;}
                if(found<0){found=int(s.segments.size());s.segments.push_back({a,b,{}});} route.segmentIndices.push_back(found);
            }
        }
        require(!route.segmentIndices.empty(),"New route needs segments or an ordered node selection");s.routes.push_back(route);
    } else if(op=="delete-route") {
        s.routes.erase(s.routes.begin()+routeIndex(s,req.at("routeId")));
        std::set<int> used;for(const auto& r:s.routes)for(int e:r.segmentIndices)used.insert(e);
        std::vector<int> map(s.segments.size(),-1);std::vector<ShapeSegment> edges;
        for(int e:used){map[e]=int(edges.size());edges.push_back(s.segments[e]);}
        for(auto& r:s.routes)for(int& e:r.segmentIndices)e=map[e];s.segments=edges;
        std::set<int> nodes;for(const auto& e:s.segments){nodes.insert(e.a);nodes.insert(e.b);}
        std::vector<int> nm(s.nodes.size(),-1);std::vector<ShapeNode> ns;for(int n:nodes){nm[n]=int(ns.size());ns.push_back(s.nodes[n]);}
        for(auto& e:s.segments){e.a=nm[e.a];e.b=nm[e.b];}s.nodes=ns;
    } else if(op!="render-geometry") throw std::invalid_argument("Unknown core operation: "+op);
}
}
json transitCoreRequest(const json& request) {
    const auto op=request.value("op",std::string("session"));
    if(op=="gtfs-patterns")return {{"directionalData",gtfsDirectionalPatterns(request.at("tables"))}};
    json state=request.value("state",json::object()); Shape shape;
    if(op=="session" || op=="replace-map") {
        auto map=request.at("map"); shape=loadMap(map);
        if(map.contains("transitMapDirections")) {
            validateDirectionalData(map["transitMapDirections"]);
            state["directionalData"]=map["transitMapDirections"];
        } else if(op!="replace-map" || !request.value("preserveRouteDirections",false)) state.erase("directionalData");
        state["bidirectionalRoutes"]=map.value("bidirectionalRoutes",json::array());
        // Fresh generation after LOOM; never match new vector indices to old IDs.
        state["pathIds"]=json::object(); identities(shape,state);
        state["shapeRevision"]=state.value("shapeRevision",0ULL)+1;
        state.erase("shapeTraversals");
        if(state.contains("directionalData")) {
            state["shapeTraversals"]=mapGtfsDirections(shape,state["directionalData"],state["shapeRevision"]);
            state["routeTraversals"]=json::array();
        } else if(request.value("preserveRouteDirections",false)) {
            require(state.contains("shape"),"Direction remapping needs the previous Shape");
            state["routeTraversals"]=remapRouteDirections(decode(state.at("shape")),shape,state.value("routeTraversals",json::array()));
        } else if(hasRouteDirections(map))state["routeTraversals"]=importRouteDirections(shape,map);
        else state["routeTraversals"]=json::array();
        Camera cam;cam.resize(1200,800);GeoData data;
        Route bounds;for(const auto& n:shape.nodes)bounds.segments.push_back({n.pos});data.routes.push_back(bounds);
        cam.fitToData(data);auto center=cam.screenToWorld(600,400);
        state["transform"]={{"x",center.x},{"y",center.y},{"scale",cam.getScale()}};
        state["planar"]=map.value("coordinateSystem",std::string("auto"))=="planar";
        state["obstacles"]=request.value("obstacles",map.value("transitMapObstacles",json{{"type","FeatureCollection"},{"features",json::array()}}));
    } else if(op=="loom-apply")shape=applyNativeLayout(state,request.at("map"));
    else shape=decode(state.at("shape"));
    if(op!="move-region-feature"&&op!="scale-region-feature"&&op!="reshape-region-feature"&&op!="delete-region-feature"&&op!="apply-regions"&&op!="render-geometry"&&op!="export"&&op!="loom-export"){state.erase("regionBaseline");state.erase("regionAppliedGeometry");}
    auto cam=cameraFor(state);
    if(op=="loom-export")return nativeLayoutInput(shape);
    if(op=="export" || op=="loom-export") {
        auto graph=shapeToLoomGeoJson(shape);
        // Existing exporter writes lon/lat; planar imports explicitly retain
        // their original CRS and coordinates on JSON/LOOM export.
        if(op=="export" && state.value("planar",false)) {
            for(auto& f:graph["features"]) {
                auto& g=f["geometry"];const auto& p=f["properties"];
                if(g["type"]=="Point")g["coordinates"]=point(shape.nodes[nodeIndex(shape,p["id"])].pos);
                else if(g["type"]=="LineString") {const auto& e=shape.segments[segmentIndex(shape,p["id"])];g["coordinates"]=json::array({point(shape.nodes[e.a].pos),point(shape.nodes[e.b].pos)});}
            }
            graph["coordinateSystem"]="planar";
        }
        // OCTI consumes logical route membership once per segment. Its
        // directions survive separately in state and are remapped on return.
        if(op=="loom-export" && state.contains("directionalData"))graph["transitMapDirections"]=state["directionalData"];
        if(op=="export") {
            if(state.contains("directionalData")) {
                if(!state.contains("shapeTraversals"))state["shapeTraversals"]=mapGtfsDirections(shape,state["directionalData"],state.value("shapeRevision",0ULL));
                exportShapeTraversals(graph,shape,state["shapeTraversals"],state.value("shapeRevision",0ULL));
            }
            exportRouteDirections(graph,state.value("routeTraversals",json::array()));
        }
        graph["transitMapObstacles"]=state["obstacles"]; return {{"map",graph},{"traversalDiagnostics",state.value("shapeTraversals",json::object()).value("diagnostics",json::array())}};
    }
    if(op=="append-map") {
        auto incoming=loadMap(request.at("map"));
        std::map<std::string,int> nodes;
        auto stationKey=[](const ShapeNode& n){return !n.station_id.empty()?n.station_id:isStationLike(n.type)?n.uid:std::string();};
        for(size_t i=0;i<shape.nodes.size();++i){auto key=stationKey(shape.nodes[i]);if(!key.empty())nodes[key]=int(i);}
        std::vector<int> remap;
        for(auto n:incoming.nodes) {
            // Generated shape-point IDs belong to one conversion, not the feed.
            // Only a real station ID may join independently imported routes.
            const auto key=stationKey(n);
            auto found=key.empty()?nodes.end():nodes.find(key);
            if(found!=nodes.end())remap.push_back(found->second);
            else {int index=int(shape.nodes.size());if(!key.empty())nodes[key]=index;remap.push_back(index);n.id=-1;shape.nodes.push_back(n);}
        }
        std::map<std::pair<int,int>,int> edges;
        for(size_t i=0;i<shape.segments.size();++i){const auto& e=shape.segments[i];edges[std::minmax(e.a,e.b)]=int(i);}
        std::vector<int> edgeMap;
        for(auto e:incoming.segments){e.a=remap[e.a];e.b=remap[e.b];auto key=std::minmax(e.a,e.b);auto found=edges.find(key);
            if(found!=edges.end())edgeMap.push_back(found->second);
            else {int index=int(shape.segments.size());edges[key]=index;edgeMap.push_back(index);e.uid="";shape.segments.push_back(e);}}
        for(auto r:incoming.routes)if(std::none_of(shape.routes.begin(),shape.routes.end(),[&](const auto& old){return old.id==r.id;})) {
            for(int& e:r.segmentIndices)e=edgeMap[e];shape.routes.push_back(r);
        }
        identities(shape,state);state["shapeRevision"]=state.value("shapeRevision",0ULL)+1;
        const auto& map=request.at("map");
        if(map.contains("transitMapDirections")) {
            if(!state.contains("directionalData"))state["directionalData"]=map["transitMapDirections"];
            else for(const auto& r:map["transitMapDirections"]["routes"]) {
                auto& routes=state["directionalData"]["routes"];
                if(std::none_of(routes.begin(),routes.end(),[&](const auto& old){return old["routeId"]==r["routeId"];}))routes.push_back(r);
            }
            state["shapeTraversals"]=mapGtfsDirections(shape,state["directionalData"],state["shapeRevision"]);
        }
        if(hasRouteDirections(map)) {
            auto records=importRouteDirections(incoming,map);
            std::map<std::string,std::string> nodeIds;
            for(size_t i=0;i<incoming.nodes.size();++i)nodeIds[incoming.nodes[i].uid]=shape.nodes[remap[i]].uid;
            for(auto record:records){record["from"]=nodeIds.at(record["from"]);record["to"]=nodeIds.at(record["to"]);state["routeTraversals"].push_back(record);}
        }
    }
    if(op=="update-routes"){state["styles"]=request.value("styles",state.value("styles",json::object()));updateRoutes(shape,state,request);}
    if(op!="session" && op!="replace-map" && op!="loom-apply" && op!="append-map" && op!="update-routes") {
        Shape before=shape;DrawnRoute drawn;
        const bool feature=op=="draw-line-feature"||op=="draw-region-feature";
        if(op=="draw-route")drawn=drawRoute(shape,request,cam);
        else if(feature)drawFeature(state,op,request,cam);
        else if(op=="move-region-feature"||op=="scale-region-feature"||op=="reshape-region-feature"||op=="delete-region-feature"||op=="apply-regions")editRegions(shape,state,request,cam,op=="apply-regions");
        else edit(shape,op,request,cam);
        identities(shape,state,&before);
        if(op!="render-geometry" && op!="move-node" && op!="move-region-feature" && op!="scale-region-feature" && op!="reshape-region-feature" && op!="delete-region-feature" && !feature) {
            state["shapeRevision"]=state.value("shapeRevision",0ULL)+1;
            if(state.contains("directionalData"))
                state["shapeTraversals"]=mapGtfsDirections(shape,state["directionalData"],state["shapeRevision"]);
            state["routeTraversals"]=op=="apply-regions"?remapRouteDirections(before,shape,state.value("routeTraversals",json::array())):editRouteDirections(before,shape,state.value("routeTraversals",json::array()),op=="draw-route"?"split-segment":op,request);
            if(op=="draw-route")for(size_t i=1;i<drawn.nodes.size();++i)
                state["routeTraversals"].push_back({{"routeId",drawn.id},{"from",shape.nodes[drawn.nodes[i-1]].uid},{"to",shape.nodes[drawn.nodes[i]].uid}});
        }
    }
    if(op=="draw-line-feature"||op=="draw-region-feature"||op=="draw-route")shape=integrateFeatures(shape,state);
    if(op=="draw-region-feature") {
        state["regionBaseline"]=encode(shape,cam,Shape2StyleShape(shape),state);
        auto region=editableRegions(state,cam);
        for(auto& n:shape.nodes)n.pos=screen(cam,n.pos);
        pushOutside(shape,region);
        for(auto& n:shape.nodes)n.pos=cam.screenToWorld(n.pos.x,800-n.pos.y);
    }
    // Imported line membership is flagged using its persisted obstacle record.
    for(const auto& f:state["obstacles"].value("features",json::array())){auto props=f.value("properties",json::object());auto id=props.value("topologyRouteId",std::string());for(auto& r:shape.routes)if(r.id==id){r.isObstacle=true;r.obstacleKind=ObstacleKind::Line;}}
    if(op=="session"||op=="replace-map") {
        const auto& features=state["obstacles"]["features"];
        if(std::any_of(features.begin(),features.end(),[](const auto& f){auto p=f.value("properties",json::object());return p.value("featureKind",std::string())=="line"&&!p.contains("topologyRouteId");}))shape=integrateFeatures(shape,state);
    }
    syncFeatureGeometry(shape,state);
    validate(shape);
    auto selected=request.value("bidirectionalRoutes",state.value("bidirectionalRoutes",json::array()));
    require(selected.is_array(),"bidirectionalRoutes must be an array");
    std::set<std::string> available,enabled;
    if(state.contains("directionalData"))for(const auto& r:state["directionalData"]["routes"])
        if(!r.at("patterns").empty()&&std::any_of(shape.routes.begin(),shape.routes.end(),[&](const auto& s){return s.id==r.at("routeId");}))available.insert(r.at("routeId"));
    for(const auto& id:selected) {
        auto key=identifier(id);
        if(available.count(key))enabled.insert(key);
        else require(!request.contains("bidirectionalRoutes"),"Route has no directional GTFS data: "+key);
    }
    state["bidirectionalRoutes"]=enabled;
    auto canonical=Shape2StyleShape(shape);
    auto topology=encode(shape,cam,canonical,state);state["shape"]=topology;
    // Two comparison styles share one authoritative topology/revision.
    auto styles=op=="update-routes"?state["styles"]:request.value("styles",state.value("styles",json::object()));
    if(request.contains("style"))styles["right"]=request["style"];
    state["styles"]=styles;json scenes=json::object();
    Shape renderShape;StyledShape renderCanonical;json renderTopology;
    if(!enabled.empty()) {
        renderShape=directionalRenderShape(shape,state.at("directionalData"),state["bidirectionalRoutes"]);
        renderCanonical=Shape2StyleShape(renderShape);directionalSharedGeometry(renderShape,renderCanonical);
        auto temporary=state;renderTopology=encode(renderShape,cam,renderCanonical,temporary);
        // Public display membership uses the logical parent, never the instance ID.
        std::map<std::string,std::string> parents;for(const auto& r:renderShape.routes)parents[r.id]=logicalId(r);
        for(auto& n:renderTopology["nodes"]) {std::set<std::string> ids;for(const auto& id:n["routeIds"])ids.insert(parents.at(id));n["routeIds"]=ids;n["interchange"]=n.value("isStation",false)&&ids.size()>1;}
    }
    // Read-only viewport rendering compensates lane offsets, not authored styles.
    const double spacingScale=number(request,"spacingScale",1.0,0.1,5.0);
    auto scaleSpacing=[&](StyledShape& styled) {
        for(auto& row:styled.routes_spacing)for(auto& gap:row)gap*=spacingScale;
    };
    auto sceneFor=[&](const json& st) {
        Shape visible=enabled.empty()?shape:renderShape;
        if(st.contains("visibleRouteIds")) {
            std::set<std::string> ids=st.at("visibleRouteIds").get<std::set<std::string>>();
            visible.routes.erase(std::remove_if(visible.routes.begin(),visible.routes.end(),[&](const auto& r){return !r.isObstacle&&!ids.count(logicalId(r));}),visible.routes.end());
        }
        auto plain=Shape2StyleShape(visible);
        if(!enabled.empty())directionalSharedGeometry(visible,plain);
        auto temporary=state;auto visibleTopology=encode(visible,cam,plain,temporary);
        std::map<std::string,std::string> parents;for(const auto& r:visible.routes)parents[r.id]=logicalId(r);
        for(auto& n:visibleTopology["nodes"]){std::set<std::string> ids;for(const auto& id:n["routeIds"])ids.insert(parents.at(id));n["routeIds"]=ids;n["interchange"]=n.value("isStation",false)&&ids.size()>1;}
        auto styled=plain;geometryStyle(styled,visible,st);scaleSpacing(styled);
        auto scene=rendered(visible,styled,cam,visibleTopology,spacingScale);
        scene["displayNodes"]=json::array();scene["displaySegmentIds"]=json::array();scene["editNodeIds"]=json::array();
        for(const auto& n:visibleTopology["nodes"])if(!n["routeIds"].empty())scene["displayNodes"].push_back(n);
        std::set<std::string> logicalRoutes;for(const auto& r:visible.routes)logicalRoutes.insert(logicalId(r));
        auto shown=[&](const json& item){return std::any_of(item["routeIds"].begin(),item["routeIds"].end(),[&](const auto& id){return logicalRoutes.count(id.template get<std::string>());});};
        for(const auto& e:topology["segments"])if(shown(e))scene["displaySegmentIds"].push_back(e["id"]);
        for(const auto& n:topology["nodes"])if(shown(n))scene["editNodeIds"].push_back(n["id"]);
        geometryStyle(plain,visible,json::object());
        scene["geometryRoutes"]=rendered(visible,plain,cam,visibleTopology)["routes"];
        return scene;
    };
    for(auto it=styles.begin();it!=styles.end();++it) {
        // Discard styles for deleted routes/pairs as part of topology updates.
        auto st=it.value(); auto pairs=st.value("pairs",json::array());json retained=json::array();
        for(const auto& pair:pairs)if(std::any_of(shape.routes.begin(),shape.routes.end(),[&](const auto& r){return r.id==pair.at("a");})&&std::any_of(shape.routes.begin(),shape.routes.end(),[&](const auto& r){return r.id==pair.at("b");}))retained.push_back(pair);
        st["pairs"]=retained;scenes[it.key()]=sceneFor(st);
    }
    if(scenes.empty())scenes["right"]=sceneFor(json::object());
    for(auto it=scenes.begin();it!=scenes.end();++it)it.value()["features"]=featureCommands(state,styles.value(it.key(),json::object()),cam);
    json obstacles=json::array();size_t i=0;
    for(const auto& f:state["obstacles"].value("features",json::array())) {
        auto props=f.value("properties",json::object()); if(!props.is_object())props=json::object();
        auto label=props.contains("name")&&props["name"].is_string()?props["name"].get<std::string>():props.contains("name:fr")&&props["name:fr"].is_string()?props["name:fr"].get<std::string>():std::string();
        auto geometry=mapCoordinates(f.at("geometry"),&cam,state.value("planar",false));
        json obstacle={{"id",std::to_string(i++)},{"name",label},{"properties",props},{"geometry",geometry}};
        if(props.value("featureKind",std::string())=="region"&&geometry.at("type")=="Polygon") {
            auto ps=geometry.at("coordinates").at(0);if(ps.size()>1&&ps.front()==ps.back())ps.erase(ps.end()-1);
            json vertices=json::array(),edges=json::array();double x=1e30,y=1e30;
            for(size_t j=0;j<ps.size();++j){auto a=ps[j],b=ps[(j+1)%ps.size()];x=std::min(x,a[0].get<double>());y=std::min(y,a[1].get<double>());vertices.push_back({{"index",j},{"point",a}});edges.push_back({{"index",j},{"from",a},{"to",b},{"center",json::array({(a[0].get<double>()+b[0].get<double>())*.5,(a[1].get<double>()+b[1].get<double>())*.5})}});}
            obstacle["editHandles"]={{"vertices",vertices},{"edges",edges},{"trash",json::array({x,y})}};
        }
        obstacles.push_back(std::move(obstacle));
    }
    return {{"state",state},{"shape",topology},{"renderGeometries",scenes},{"renderGeometry",scenes.begin().value()},{"obstacles",obstacles},{"bidirectionalRoutes",state["bidirectionalRoutes"]},{"directionalRouteIds",available},{"traversalDiagnostics",state.value("shapeTraversals",json::object()).value("diagnostics",json::array())}};
}
