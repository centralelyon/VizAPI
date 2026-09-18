#include "io/route_directions.h"
#include "core/projection.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>
using json = nlohmann::json;
namespace {
std::string id(const json& j) {
    if(j.is_string())return j.get<std::string>();
    if(j.is_number_integer())return j.dump();
    throw std::invalid_argument("Direction endpoint must be a string/integer ID");
}
Point coordinate(const json& c, bool planar) {
    Point p{c.at(0),c.at(1)};
    return !planar&&std::abs(p.x)<=180&&std::abs(p.y)<=90 ? lonlatToWebMerc(p.x,p.y) : p;
}
double distance(Point a,Point b){return std::hypot(a.x-b.x,a.y-b.y);}
struct Graph {
    const Shape& shape;
    std::map<std::string,int> nodes;
    std::map<std::string,std::map<int,std::set<int>>> adj;
    explicit Graph(const Shape& s):shape(s) {
        for(size_t i=0;i<s.nodes.size();++i)nodes[s.nodes[i].uid]=int(i);
        for(const auto& r:s.routes)for(int ei:r.segmentIndices){const auto& e=s.segments[ei];adj[r.id][e.a].insert(e.b);adj[r.id][e.b].insert(e.a);}
    }
    // Only route topology supplies connectivity. Direction comes from the ordered
    // input anchors. Without a shape guide, competing paths are rejected.
    std::vector<int> path(const std::string& route,int a,int b,bool stopBarrier=false) const {
        if(a==b)return {a};
        auto ri=adj.find(route);if(ri==adj.end())return {};
        const auto& links=ri->second;
        std::vector<double> cost(shape.nodes.size(),std::numeric_limits<double>::infinity());
        std::vector<int> previous(shape.nodes.size(),-1),ways(shape.nodes.size(),0);
        using Item=std::pair<double,int>;std::priority_queue<Item,std::vector<Item>,std::greater<Item>> queue;
        cost[a]=0;ways[a]=1;queue.push({0,a});
        while(!queue.empty()) {
            auto [d,u]=queue.top();queue.pop();if(d>cost[u]+1e-8)continue;
            if(u==b)break;
            if(stopBarrier&&u!=a&&isStationLike(shape.nodes[u].type))continue;
            auto it=links.find(u);if(it==links.end())continue;
            for(int v:it->second) {
                double next=d+std::max(1e-6,distance(shape.nodes[u].pos,shape.nodes[v].pos));
                if(next<cost[v]-1e-8){cost[v]=next;previous[v]=u;ways[v]=ways[u];queue.push({next,v});}
                else if(std::abs(next-cost[v])<1e-8)ways[v]=2;
            }
        }
        if(previous[b]<0)return {};
        if(ways[b]>1)throw std::invalid_argument("Ambiguous directional path for route "+route);
        std::vector<int> out;for(int n=b;n!=-1;n=previous[n]){out.push_back(n);if(n==a)break;}
        std::reverse(out.begin(),out.end());
        if(stopBarrier)for(size_t i=1;i<out.size();++i) {
            // Stop order alone cannot choose a branch in a loop, even if one
            // branch is shorter. Check for an alternative to each path edge.
            std::set<int> seen{a};std::vector<int> pending{a};
            while(!pending.empty()) {
                int u=pending.back();pending.pop_back();
                if(u==b)throw std::invalid_argument("Ambiguous GTFS/OCTI traversal: shape guidance is required for route "+route);
                if(u!=a&&isStationLike(shape.nodes[u].type))continue;
                auto it=links.find(u);if(it==links.end())continue;
                for(int v:it->second) {
                    if((u==out[i-1]&&v==out[i])||(v==out[i-1]&&u==out[i]))continue;
                    if(seen.insert(v).second)pending.push_back(v);
                }
            }
        }
        return out;
    }
    bool edge(const json& r) const {
        auto route=adj.find(r.at("routeId"));auto a=nodes.find(r.at("from")),b=nodes.find(r.at("to"));
        if(route==adj.end()||a==nodes.end()||b==nodes.end())return false;
        auto it=route->second.find(a->second);return it!=route->second.end()&&it->second.count(b->second);
    }
};
void append(json& out,json record,const Shape& s,const std::vector<int>& nodes) {
    for(size_t i=1;i<nodes.size();++i){record["from"]=s.nodes[nodes[i-1]].uid;record["to"]=s.nodes[nodes[i]].uid;out.push_back(record);}
}
json unique(const json& records){json out=json::array();std::set<std::string> seen;for(const auto& r:records)if(seen.insert(r.dump()).second)out.push_back(r);return out;}
}
bool hasRouteDirections(const json& map) {
    for(const auto& f:map.at("features"))if(f.at("geometry").at("type")=="LineString")
        for(const auto& line:f.at("properties").at("lines"))if(line.contains("from")||line.contains("to")||line.contains("directions"))return true;
    return false;
}
void retainMapNodeIds(Shape& s,const json& map) {
    const bool planar=map.value("coordinateSystem","")=="planar";
    for(const auto& f:map.at("features"))if(f.at("geometry").at("type")=="Point") {
        auto p=coordinate(f["geometry"]["coordinates"],planar);
        for(auto& n:s.nodes)if(n.uid.empty()&&distance(n.pos,p)<1e-6){n.uid=id(f["properties"]["id"]);break;}
    }
}
Shape loadDirectedTopology(const json& map) {
    // An exported directed graph is already simplified. Re-simplifying would
    // remove identified shape points and could merge distinct coincident nodes.
    Shape s;std::map<std::string,int> nodes,routes;std::set<std::string> reserved;
    const bool planar=map.value("coordinateSystem","")=="planar";
    for(const auto& f:map.at("features"))if(f["geometry"]["type"]=="Point") {
        const auto& p=f.at("properties");const auto uid=id(p.at("id"));if(nodes.count(uid))continue;
        ShapeNode n;n.id=int(s.nodes.size());n.uid=uid;n.pos=coordinate(f["geometry"]["coordinates"],planar);
        if(p.contains("station_id")||p.contains("station_label")||p.contains("name")) {
            n.type=ShapeNodeType::Station;n.station_id=p.value("station_id","");n.name=p.value("station_label",p.value("name",std::string()));
        }
        nodes[uid]=n.id;reserved.insert(uid);s.nodes.push_back(n);
    }
    int serial=0;
    for(const auto& f:map.at("features"))if(f["geometry"]["type"]=="LineString") {
        const auto& p=f.at("properties");const auto& coords=f["geometry"]["coordinates"];
        int a=nodes.at(id(p.at("from"))),b=nodes.at(id(p.at("to")));
        std::vector<int> chain{a};
        for(size_t i=1;i+1<coords.size();++i){ShapeNode n;n.id=int(s.nodes.size());n.pos=coordinate(coords[i],planar);
            do{n.uid="direction-point-"+std::to_string(serial++);}while(reserved.count(n.uid));
            reserved.insert(n.uid);chain.push_back(n.id);s.nodes.push_back(n);}
        chain.push_back(b);std::vector<int> edges;
        for(size_t i=1;i<chain.size();++i)if(chain[i-1]!=chain[i]) {
            edges.push_back(int(s.segments.size()));s.segments.push_back({chain[i-1],chain[i],chain.size()==2&&p.contains("id")?id(p["id"]):std::string()});
        }
        for(const auto& line:p.at("lines")) {
            const auto rid=id(line.contains("id")?line["id"]:line.at("label"));
            if(!routes.count(rid)) {
                ShapeRoute r;r.id=rid;r.name=line.value("name",line.value("label",rid));r.route_width=line.value("route_width",6.f);
                auto hex=line.value("color",std::string("777777"));if(!hex.empty()&&hex[0]=='#')hex.erase(0,1);
                unsigned rgb=0x777777;try{if(hex.size()==6)rgb=std::stoul(hex,nullptr,16);}catch(...){}
                r.color[0]=((rgb>>16)&255)/255.f;r.color[1]=((rgb>>8)&255)/255.f;r.color[2]=(rgb&255)/255.f;
                routes[rid]=int(s.routes.size());s.routes.push_back(r);
            }
            auto& dest=s.routes[routes.at(rid)].segmentIndices;
            // Opposite traversals may repeat the route ID in public lines[].
            // They are one logical route and one geometry membership.
            for(int edge:edges)if(std::find(dest.begin(),dest.end(),edge)==dest.end())dest.push_back(edge);
        }
    }
    return s;
}
json importRouteDirections(const Shape& s,const json& map) {
    Graph g(s);json records=json::array();size_t edgeOffset=0;
    for(const auto& f:map.at("features"))if(f["geometry"]["type"]=="LineString") {
        const auto& p=f.at("properties");auto a=id(p.at("from")),b=id(p.at("to"));
        // loadDirectedTopology keeps feature order, including every interior
        // sample. Follow this specific segment, never a shorter parallel path.
        std::vector<int> chain{g.nodes.at(a)};
        for(size_t i=1;i<f["geometry"]["coordinates"].size();++i) {
            if(f["geometry"]["coordinates"].size()==2&&a==b)continue;
            const auto& edge=s.segments.at(edgeOffset++);chain.push_back(edge.b);
        }
        for(const auto& line:p.at("lines")) {
            auto items=line.value("directions",json::array());
            if(!items.is_array())throw std::invalid_argument("lines[].directions must be an array");
            if(line.contains("from")||line.contains("to")){if(!items.empty())throw std::invalid_argument("Use from/to or directions, not both");items.push_back(line);}
            for(const auto& item:items) {
                auto from=id(item.at("from")),to=id(item.at("to"));
                if(!((from==a&&to==b)||(from==b&&to==a)))throw std::invalid_argument("Route direction must reference its segment endpoints");
                json record={{"routeId",id(line.contains("id")?line["id"]:line.at("label"))}};
                for(const auto* key:{"direction","pattern"})if(item.contains(key))record[key]=id(item[key]);
                auto path=chain;if(from==b)std::reverse(path.begin(),path.end());
                append(records,record,s,path);
            }
        }
    }
    return unique(records);
}
json mapGtfsDirections(const Shape& s,const json& data) {
    Graph g(s);json records=json::array();const bool planar=data.value("coordinateSystem","")=="planar";
    for(const auto& r:data.at("routes")) {
        const auto rid=r.at("routeId").get<std::string>();auto ri=g.adj.find(rid);if(ri==g.adj.end())continue;
        for(const auto& pattern:r.at("patterns")) {
            json record={{"routeId",rid},{"direction",pattern.at("directionId")}};
            // Retain variants, including non-representative GTFS trips.
            if(r.at("patterns").size()>1)record["pattern"]=pattern.at("patternId");
            const bool shaped=pattern.at("orderedShapePoints").size()>1;
            const auto& rows=pattern.at(shaped?"orderedShapePoints":"orderedStops");
            std::vector<int> anchors;
            for(const auto& row:rows) {
                Point p=coordinate(row.at("position"),planar);int best=-1;double score=std::numeric_limits<double>::infinity();
                // Stop IDs are preferred; distance is only for mapping original
                // GTFS coordinates onto the simplified graph, never edit/export.
                bool matched=false;
                for(const auto& entry:ri->second) {
                    const auto& n=s.nodes[entry.first];bool match=!shaped&&n.station_id==row.value("stopId",std::string("\x01"));
                    if(match&&!matched){matched=true;score=std::numeric_limits<double>::infinity();}
                    if(matched&&!match)continue;
                    double d=distance(p,n.pos);if(d<score){score=d;best=entry.first;}
                }
                if(best>=0&&(anchors.empty()||anchors.back()!=best))anchors.push_back(best);
            }
            if(anchors.size()<2)throw std::invalid_argument("GTFS pattern has no distinct Shape anchors for route "+rid);
            for(size_t i=1;i<anchors.size();++i) {
                auto path=g.path(rid,anchors[i-1],anchors[i],!shaped);
                if(path.empty())throw std::invalid_argument("Cannot map GTFS traversal onto Shape for route "+rid);
                append(records,record,s,path);
            }
        }
    }
    return unique(records);
}
json editRouteDirections(const Shape& before,const Shape& after,const json& records,const std::string& op,const json& req) {
    Graph old(before),now(after);json changed=records;
    if(op=="merge-stations") {
        std::set<std::string> ids;int keep=int(before.nodes.size());
        for(const auto& n:req.at("nodeIds")){auto uid=id(n);ids.insert(uid);keep=std::min(keep,old.nodes.at(uid));}
        for(auto& r:changed)for(const auto* key:{"from","to"})if(ids.count(r.at(key)))r[key]=before.nodes[keep].uid;
    } else if(op=="split-station") {
        std::string added;for(const auto& n:after.nodes)if(!old.nodes.count(n.uid)){added=n.uid;break;}
        std::set<std::string> routes;for(const auto& r:req.at("routeIds"))routes.insert(id(r));
        for(auto& r:changed)if(routes.count(r.at("routeId")))for(const auto* key:{"from","to"})if(r.at(key)==req.at("nodeId"))r[key]=added;
    } else if(op=="add-route"&&req.contains("nodeIds")) {
        const auto& ns=req.at("nodeIds");for(size_t i=1;i<ns.size();++i)changed.push_back({{"routeId",id(req.at("routeId"))},{"from",id(ns[i-1])},{"to",id(ns[i])}});
    } else if(op=="delete-node") {
        // Compose only compatible directed incidences through the removed node.
        const auto uid=id(req.at("nodeId"));
        for(const auto& a:records)if(a.at("to")==uid)for(const auto& b:records)if(b.at("from")==uid) {
            auto left=a,right=b;left.erase("from");left.erase("to");right.erase("from");right.erase("to");
            if(left==right&&a.at("from")!=b.at("to")){auto r=a;r["to"]=b.at("to");changed.push_back(r);}
        }
    }
    json out=json::array();
    for(const auto& r:changed) {
        if(now.edge(r)){out.push_back(r);continue;}
        if((op=="split-segment"||op=="add-station")&&now.nodes.count(r.at("from"))&&now.nodes.count(r.at("to"))) {
            // A split inserts nodes on this exact former segment (including
            // other collinear segments split by the desktop operation).
            auto a=now.nodes.at(r.at("from")),b=now.nodes.at(r.at("to"));
            auto path=now.path(r.at("routeId"),a,b);
            double length=0;for(size_t i=1;i<path.size();++i)length+=distance(after.nodes[path[i-1]].pos,after.nodes[path[i]].pos);
            if(!path.empty()&&std::abs(length-distance(after.nodes[a].pos,after.nodes[b].pos))<1e-5)append(out,r,after,path);
            else throw std::invalid_argument("Cannot preserve traversal across segment split");
        }
    }
    return unique(out);
}
json remapRouteDirections(const Shape& before,const Shape& after,const json& records) {
    Graph old(before),now(after);std::map<std::string,int> mapped;
    for(const auto& n:before.nodes) {
        if(now.nodes.count(n.uid)){mapped[n.uid]=now.nodes.at(n.uid);continue;}
        if(n.station_id.empty())continue;
        int match=-1;for(size_t i=0;i<after.nodes.size();++i)if(after.nodes[i].station_id==n.station_id){if(match>=0){match=-2;break;}match=int(i);}
        if(match>=0)mapped[n.uid]=match;
    }
    // OCTI may remove shape points. Collapse directed chains through unmapped
    // nodes before mapping surviving anchors to the new route topology.
    json out=json::array();std::map<std::string,std::vector<json>> groups;
    for(const auto& r:records){auto key=r;key.erase("from");key.erase("to");groups[key.dump()].push_back(r);}
    for(const auto& group:groups) {
        const auto outputBefore=out.size();
        std::set<std::pair<std::string,std::string>> covered;
        std::map<std::string,std::set<std::string>> adj;
        for(const auto& r:group.second)adj[r.at("from")].insert(r.at("to"));
        for(const auto& start:adj)if(mapped.count(start.first)) {
            std::vector<std::string> pending(start.second.begin(),start.second.end());std::set<std::string> seen;
            for(const auto& next:start.second)covered.insert({start.first,next});
            while(!pending.empty()) {
                auto node=pending.back();pending.pop_back();if(!seen.insert(node).second)continue;
                if(mapped.count(node)) {
                    auto record=json::parse(group.first);auto path=now.path(record.at("routeId"),mapped.at(start.first),mapped.at(node),true);
                    if(path.empty())throw std::invalid_argument("OCTI lost a directed route connection");
                    append(out,record,after,path);
                } else {
                    if(adj[node].empty())throw std::invalid_argument("OCTI lost a directional endpoint");
                    for(const auto& next:adj[node]){covered.insert({node,next});pending.push_back(next);}
                }
            }
        }
        for(const auto& edge:group.second)if(!covered.count({edge.at("from"),edge.at("to")}))
            throw std::invalid_argument("OCTI lost the anchors of a directional component");
        if(outputBefore==out.size())throw std::invalid_argument("OCTI did not preserve enough node identities to retain a directional pattern");
    }
    if(!records.empty()&&out.empty())throw std::invalid_argument("OCTI did not preserve enough node identities to retain route directions");
    return unique(out);
}
void exportRouteDirections(json& graph,const json& records) {
    std::map<std::string,json> index;
    for(const auto& record:records) {
        auto a=record.at("from").get<std::string>(),b=record.at("to").get<std::string>();if(b<a)std::swap(a,b);
        auto key=json::array({record.at("routeId"),a,b}).dump();if(!index.count(key))index[key]=json::array();
        // GTFS direction/pattern IDs are private provenance. Public direction
        // is expressed exclusively by ordered endpoints.
        index[key].push_back({{"from",record.at("from")},{"to",record.at("to")}});
    }
    for(auto& f:graph["features"])if(f["geometry"]["type"]=="LineString") {
        auto& p=f["properties"];auto a=p.at("from").get<std::string>(),b=p.at("to").get<std::string>();if(b<a)std::swap(a,b);
        json lines=json::array();
        for(const auto& line:p["lines"]) {
            auto found=index.find(json::array({line.at("id"),a,b}).dump());
            if(found==index.end()){lines.push_back(line);continue;}
            for(const auto& traversal:unique(found->second)) {
                auto occurrence=line;
                occurrence["from"]=traversal.at("from");occurrence["to"]=traversal.at("to");
                lines.push_back(occurrence);
            }
        }
        p["lines"]=lines;
    }
}
