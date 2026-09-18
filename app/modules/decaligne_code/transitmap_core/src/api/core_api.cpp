#include "api/core_api.h"
#include "core/shape.h"
#include "core/projection.h"
#include "io/loader.h"
#include "io/directional.h"
#include "io/route_directions.h"
#include "io/shape_loom_export.h"
#include "render/render_geometry.h"
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
        er.at(ei).insert(r.id); nr.at(s.segments[ei].a).insert(r.id); nr.at(s.segments[ei].b).insert(r.id);
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
        routes.push_back({{"id",r.id},{"name",r.name},{"color",color(r.color)},{"rgb",json::array({r.color[0],r.color[1],r.color[2]})},
            {"width",r.route_width},{"isObstacle",r.isObstacle},{"obstacleKind",int(r.obstacleKind)},{"segmentIds",ids},{"paths",paths}});
    }
    state["pathIds"]=registry;
    return {{"nodes",nodes},{"segments",edges},{"routes",routes}};
}
void validate(const Shape& s) {
    require(s.nodes.size()<=50000 && s.segments.size()<=100000 && s.routes.size()<=500,"Network exceeds edit limits");
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
json rendered(const Shape& shape, const StyledShape& s, const Camera& camera, const json& topology) {
    auto display=buildStyledShapeDisplayData(s,camera); json routes=json::array(),stations=json::array();
    for(size_t ri=0;ri<shape.routes.size();++ri) {
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
        json ids=json::array(); for(int ri:ris) ids.push_back(logicalId(shape.routes.at(ri)));
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
    if(hasRouteDirections(map))return loadDirectedTopology(map);
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
void edit(Shape& s, const std::string& op, const json& req, const Camera& cam) {
    if(op=="move-node") {
        int n=nodeIndex(s,req.at("nodeId")); Point proposed=fromScreen(cam,req);
        s.nodes[n].pos=req.value("snap",true)?snapShapeStationPosition(s,n,proposed,pi):proposed;
    } else if(op=="delete-node") {
        require(deleteShapeNodes(s,{nodeIndex(s,req.at("nodeId"))}),"Delete failed");
    } else if(op=="merge-stations") {
        std::vector<int> ns;for(const auto& id:req.at("nodeIds"))ns.push_back(nodeIndex(s,id));
        require(ns.size()>=2,"Select at least two stations");
        require(mergeStations(s,ns,req.value("name",s.nodes[ns[0]].name),req.value("stationId",s.nodes[ns[0]].station_id)),"Merge failed");
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
        } else state.erase("directionalData");
        state["bidirectionalRoutes"]=map.value("bidirectionalRoutes",json::array());
        // Fresh generation after LOOM; never match new vector indices to old IDs.
        state["pathIds"]=json::object(); identities(shape,state);
        if(request.value("preserveRouteDirections",false)) {
            require(state.contains("shape"),"Direction remapping needs the previous Shape");
            state["routeTraversals"]=remapRouteDirections(decode(state.at("shape")),shape,state.value("routeTraversals",json::array()));
        } else if(hasRouteDirections(map))state["routeTraversals"]=importRouteDirections(shape,map);
        else if(state.contains("directionalData"))state["routeTraversals"]=mapGtfsDirections(shape,state["directionalData"]);
        else state["routeTraversals"]=json::array();
        Camera cam;cam.resize(1200,800);GeoData data;
        Route bounds;for(const auto& n:shape.nodes)bounds.segments.push_back({n.pos});data.routes.push_back(bounds);
        cam.fitToData(data);auto center=cam.screenToWorld(600,400);
        state["transform"]={{"x",center.x},{"y",center.y},{"scale",cam.getScale()}};
        state["planar"]=map.value("coordinateSystem",std::string("auto"))=="planar";
        state["obstacles"]=request.value("obstacles",map.value("transitMapObstacles",json{{"type","FeatureCollection"},{"features",json::array()}}));
    } else shape=decode(state.at("shape"));
    auto cam=cameraFor(state);
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
        if(op=="export")exportRouteDirections(graph,state.value("routeTraversals",json::array()));
        graph["transitMapObstacles"]=state["obstacles"]; return {{"map",graph}};
    }
    if(op!="session" && op!="replace-map") {
        Shape before=shape;edit(shape,op,request,cam);identities(shape,state,&before);
        state["routeTraversals"]=editRouteDirections(before,shape,state.value("routeTraversals",json::array()),op,request);
    }
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
    auto styles=request.value("styles",state.value("styles",json::object()));
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
    auto sceneFor=[&](const json& st) {
        if(enabled.empty()){auto styled=canonical;geometryStyle(styled,shape,st);return rendered(shape,styled,cam,topology);}
        auto styled=renderCanonical;geometryStyle(styled,renderShape,st);
        auto scene=rendered(renderShape,styled,cam,renderTopology);
        scene["displayNodes"]=json::array();
        for(const auto& n:renderTopology["nodes"])if(!n["routeIds"].empty())scene["displayNodes"].push_back(n);
        auto plain=renderCanonical;geometryStyle(plain,renderShape,json::object());
        scene["geometryRoutes"]=rendered(renderShape,plain,cam,renderTopology)["routes"];
        return scene;
    };
    for(auto it=styles.begin();it!=styles.end();++it) {
        // Discard styles for deleted routes/pairs as part of topology updates.
        auto st=it.value(); auto pairs=st.value("pairs",json::array());json retained=json::array();
        for(const auto& pair:pairs)if(std::any_of(shape.routes.begin(),shape.routes.end(),[&](const auto& r){return r.id==pair.at("a");})&&std::any_of(shape.routes.begin(),shape.routes.end(),[&](const auto& r){return r.id==pair.at("b");}))retained.push_back(pair);
        st["pairs"]=retained;scenes[it.key()]=sceneFor(st);
    }
    if(scenes.empty())scenes["right"]=sceneFor(json::object());
    json obstacles=json::array();size_t i=0;
    for(const auto& f:state["obstacles"].value("features",json::array())) {
        auto props=f.value("properties",json::object()); if(!props.is_object())props=json::object();
        auto label=props.contains("name")&&props["name"].is_string()?props["name"].get<std::string>():props.contains("name:fr")&&props["name:fr"].is_string()?props["name:fr"].get<std::string>():std::string();
        obstacles.push_back({{"id",std::to_string(i++)},{"name",label},{"properties",props},{"geometry",mapCoordinates(f.at("geometry"),&cam,state.value("planar",false))}});
    }
    return {{"state",state},{"shape",topology},{"renderGeometries",scenes},{"renderGeometry",scenes.begin().value()},{"obstacles",obstacles},{"bidirectionalRoutes",state["bidirectionalRoutes"]},{"directionalRouteIds",available}};
}
