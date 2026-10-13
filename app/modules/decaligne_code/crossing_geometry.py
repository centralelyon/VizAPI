"""Local crossing elements, ordering and preview geometry (server only)."""
from collections import defaultdict
import hashlib
import json
import math



def flatten(commands, tolerance, parameters=False):
    """Adaptive Bezier subdivision in the same canvas coordinates as the renderer."""
    result, current = [], None
    def curve(points, depth=0, t0=0., t1=1.):
        a, b = points[0], points[-1]
        length = math.dist(a, b)
        error = max((abs((b[0]-a[0])*(p[1]-a[1])-(b[1]-a[1])*(p[0]-a[0])) / length
                     if length else math.dist(a, p)) for p in points[1:-1])
        if error <= tolerance or depth == 12:
            return [(b,t1)]
        levels = [points]
        while len(levels[-1]) > 1:
            levels.append([((a[0]+b[0])/2, (a[1]+b[1])/2) for a,b in zip(levels[-1], levels[-1][1:])])
        return curve([row[0] for row in levels], depth+1, t0, (t0+t1)/2) + curve([row[-1] for row in reversed(levels)], depth+1, (t0+t1)/2, t1)
    for index, command in enumerate(commands):
        kind = command['type']
        if kind == 'close':
            current = None
            continue
        point = (command['x'], command['y'])
        if kind == 'move' or current is None:
            current = point
            continue
        controls = ([current, (command['cx'], command['cy']), point] if kind == 'quadratic' else
                    [current, (command['c1x'], command['c1y']), (command['c2x'], command['c2y']), point] if kind == 'cubic' else None)
        samples = curve(controls) if controls else [(point,1.)]
        previous_t=0.
        for sample,t in samples:
            if math.dist(current, sample) > 1e-7:
                result.append((current, sample, index, previous_t, t) if parameters else (current, sample, index))
            current = sample
            previous_t=t
    return result


def intersection(a, b, c, d):
    ux, uy, vx, vy = b[0]-a[0], b[1]-a[1], d[0]-c[0], d[1]-c[1]
    det = ux*vy-uy*vx
    if abs(det) <= 1e-8 * math.hypot(ux,uy)*math.hypot(vx,vy):
        return None  # Shared/collinear segments are a different element.
    dx, dy = c[0]-a[0], c[1]-a[1]
    t, u = (dx*vy-dy*vx)/det, (dx*uy-dy*ux)/det
    if -1e-8 <= t <= 1+1e-8 and -1e-8 <= u <= 1+1e-8:
        return (a[0]+t*ux, a[1]+t*uy)


def clip_line(a, b, bounds):
    x, y, w, h = bounds
    dx, dy = b[0]-a[0], b[1]-a[1]
    lo, hi = 0., 1.
    for p,q in [(-dx,a[0]-x),(dx,x+w-a[0]),(-dy,a[1]-y),(dy,y+h-a[1])]:
        if abs(p)<1e-12:
            if q<0: return None
        elif p<0: lo=max(lo,q/p)
        else: hi=min(hi,q/p)
    return ((a[0]+lo*dx,a[1]+lo*dy),(a[0]+hi*dx,a[1]+hi*dy)) if lo<=hi else None


def route_data(routes):
    paths={}
    strokes=[]
    for route in routes:
        for path in route['paths']:
            key=(route['routeId'],str(path['id']))
            distance=0.
            parts=[]
            for a,b,command in flatten(path['commands'], .1):
                length=math.dist(a,b)
                parts.append((a,b,distance,distance+length))
                strokes.append({'route':key[0],'path':key,'command':command,'a':a,'b':b,'start':distance,'length':length})
                distance+=length
            paths[key]=(parts,distance)
    return paths,strokes


def command_slice(commands,start,end):
    """Trim by sampled arc length, but retain the original Bezier geometry."""
    ranges={};distance=0.
    for a,b,index,t0,t1 in flatten(commands,.1,True):
        length=math.dist(a,b);lo=max(start,distance);hi=min(end,distance+length)
        if hi>lo:
            first=t0+(t1-t0)*(lo-distance)/length
            last=t0+(t1-t0)*(hi-distance)/length
            if index in ranges:ranges[index][1]=last
            else:ranges[index]=[first,last]
        distance+=length
    def split(points,t):
        rows=[points]
        while len(rows[-1])>1:
            rows.append([tuple(a[j]+(b[j]-a[j])*t for j in (0,1)) for a,b in zip(rows[-1],rows[-1][1:])])
        return [row[0] for row in rows],[row[-1] for row in reversed(rows)]
    result=[];current=None;previous_index=None
    for index,command in enumerate(commands):
        if command['type']=='close':current=None;continue
        point=(command['x'],command['y'])
        if current is not None and index in ranges:
            kind=command['type'];points=[current]
            if kind=='quadratic':points.append((command['cx'],command['cy']))
            if kind=='cubic':points.extend([(command['c1x'],command['c1y']),(command['c2x'],command['c2y'])])
            points.append(point);first,last=ranges[index]
            points=split(points,last)[0]
            if first:points=split(points,first/last)[1]
            if previous_index!=index-1:result.append({'type':'move','x':points[0][0],'y':points[0][1]})
            item={'type':kind,'x':points[-1][0],'y':points[-1][1]}
            if kind=='quadratic':item.update(cx=points[1][0],cy=points[1][1])
            if kind=='cubic':item.update(c1x=points[1][0],c1y=points[1][1],c2x=points[2][0],c2y=points[2][1])
            result.append(item);previous_index=index
        current=point
    return result


def network_elements(routes):
    """Fixed network coordinates only: no station data, camera scale or lane width."""
    paths,strokes=route_data(routes)
    grid=defaultdict(list);events={};splits=defaultdict(list)
    cell=64.
    def node(p):return tuple(round(v,5) for v in p)
    # Identical shared bends are not crossings; diverging/shared junctions are.
    rays=defaultdict(lambda:defaultdict(set))
    for stroke in strokes:
        a,b=stroke['a'],stroke['b'];rid=stroke['route']
        rays[node(a)][rid].add(round(math.atan2(b[1]-a[1],b[0]-a[0]),7))
        rays[node(b)][rid].add(round(math.atan2(a[1]-b[1],a[0]-b[0]),7))
    for i,stroke in enumerate(strokes):
        a,b=stroke['a'],stroke['b'];rid=stroke['route']
        keys=[(x,y) for x in range(math.floor(min(a[0],b[0])/cell),math.floor(max(a[0],b[0])/cell)+1)
                    for y in range(math.floor(min(a[1],b[1])/cell),math.floor(max(a[1],b[1])/cell)+1)]
        for j in sorted(set(j for key in keys for j in grid[key])):
            other=strokes[j]
            if other['route']==rid:continue
            point=intersection(a,b,other['a'],other['b'])
            if point is None:
                # Shared collinear strokes can have different shape vertices.
                # Split them into identical graph edges without adding crossings.
                c,d=other['a'],other['b']
                for q in (c,d):
                    if abs(math.dist(a,q)+math.dist(q,b)-math.dist(a,b))<1e-6:splits[i].append(node(q))
                for q in (a,b):
                    if abs(math.dist(c,q)+math.dist(q,d)-math.dist(c,d))<1e-6:splits[j].append(node(q))
                continue
            key=node(point)
            if rays[key].get(rid) and rays[key].get(rid)==rays[key].get(other['route']):continue
            event=events.setdefault(key,{'point':key,'routes':set(),'identities':set()})
            event['routes'].update((rid,other['route']))
            event['identities'].add(tuple(sorted(((rid,stroke['path'][1],stroke['command']),(other['route'],other['path'][1],other['command'])))))
            splits[i].append(key);splits[j].append(key)
        for key in keys:grid[key].append(i)
    occurrences=defaultdict(list)
    for event in events.values():occurrences[repr(sorted(event['identities']))].append(event)
    for identity,items in occurrences.items():
        for index,event in enumerate(sorted(items,key=lambda e:e['point'])):
            event['id']='network:'+hashlib.sha1((identity+':'+str(index)).encode()).hexdigest()[:20]
    # Atomic crossings stay independent of zoom and retain their saved orders.
    groups=[{'events':[event]} for event in sorted(events.values(),key=lambda e:e['id'])]
    # Split the graph at crossings. Ordinary shape vertices remain traversable.
    edges={};adjacency=defaultdict(list)
    for i,stroke in enumerate(strokes):
        a,b=stroke['a'],stroke['b']
        points=sorted(set([node(a),node(b),*splits[i]]),key=lambda q:math.dist(a,q))
        for first,last in zip(points,points[1:]):
            if math.dist(first,last)<1e-6:continue
            key=tuple(sorted((first,last)))
            if key not in edges:
                edges[key]={'routes':set(),'intervals':[]}
                adjacency[first].append(key);adjacency[last].append(key)
            edges[key]['routes'].add(stroke['route'])
            edges[key]['intervals'].append((stroke['path'],stroke['start']+math.dist(a,first),stroke['start']+math.dist(a,last)))
    stops={e['point'] for e in events.values()}
    for group in groups:
        group['id']=min(e['id'] for e in group['events'])
        group['members']=set(r for e in group['events'] for r in e['routes'])
        starts={e['point'] for e in group['events']}
        reached=set()
        for rid in group['members']:
            queue=list(starts);visited=set()
            while queue:
                point=queue.pop()
                if point in visited:continue
                visited.add(point)
                if point in stops and point not in starts:continue
                for key in adjacency[point]:
                    if rid not in edges[key]['routes']:continue
                    reached.add(key);queue.append(key[1] if key[0]==point else key[0])
        group['edges']=reached
    return paths,groups,edges


def ordered_members(routes,members,record):
    order=[r for r in record.get('order',[]) if r in members]
    return order+[r['routeId'] for r in reversed(routes) if r['routeId'] in members and r['routeId'] not in order]


def square_box(box):
    x,y,right,bottom=box
    size=max(right-x,bottom-y)
    cx,cy=(x+right)/2,(y+bottom)/2
    return [cx-size/2,cy-size/2,cx+size/2,cy+size/2]


def stroke_rectangle(a,b,radius):
    length=math.dist(a,b)
    if length==0:return []
    ux,uy=(b[0]-a[0])/length,(b[1]-a[1])/length
    nx,ny=-uy*radius,ux*radius
    # Include round end caps and joins conservatively.
    a=(a[0]-ux*radius,a[1]-uy*radius)
    b=(b[0]+ux*radius,b[1]+uy*radius)
    return [(a[0]-nx,a[1]-ny),(b[0]-nx,b[1]-ny),
            (b[0]+nx,b[1]+ny),(a[0]+nx,a[1]+ny)]


def polygon_overlap(subject,clip):
    for a,b in zip(clip,clip[1:]+clip[:1]):
        if not subject:break
        def side(p):return (b[0]-a[0])*(p[1]-a[1])-(b[1]-a[1])*(p[0]-a[0])
        output=[];previous=subject[-1];before=side(previous)
        for point in subject:
            after=side(point)
            if (before>=0)!=(after>=0):
                t=before/(before-after)
                output.append((previous[0]+t*(point[0]-previous[0]),previous[1]+t*(point[1]-previous[1])))
            if after>=0:output.append(point)
            previous=point;before=after
        subject=output
    return subject


def display_groups(atoms,route_strokes,route_map,scale,mode):
    groups=[]
    for atom in atoms:
        point=atom['events'][0]['point'];footprint=[]
        for rid in atom['members']:
            nearest=None;distance=math.inf
            for a,b in route_strokes[rid]:
                dx,dy=b[0]-a[0],b[1]-a[1]
                length2=dx*dx+dy*dy
                t=max(0,min(1,((point[0]-a[0])*dx+(point[1]-a[1])*dy)/length2)) if length2 else 0
                q=(a[0]+t*dx,a[1]+t*dy)
                d=math.dist(point,q)
                if d<distance:nearest=q;distance=d
            if nearest is not None:
                radius=((route_map[rid]['width'] if mode=='styled' else 1)/2+4)*scale
                footprint.append([nearest[0]-radius,nearest[1]-radius,nearest[0]+radius,nearest[1]+radius])
        # Width-expanded strokes overlap farther along an oblique crossing
        # than half a route width. Include that entire overlap in the cap.
        seed=[min(b[0] for b in footprint),min(b[1] for b in footprint),
              max(b[2] for b in footprint),max(b[3] for b in footprint)]
        nearby=[]
        for rid in sorted(atom['members']):
            radius=(route_map[rid]['width'] if mode=='styled' else 1)*scale/2
            for a,b in route_strokes[rid]:
                if not clip_line(a,b,[seed[0]-radius,seed[1]-radius,
                                     seed[2]-seed[0]+2*radius,seed[3]-seed[1]+2*radius]):continue
                polygon=stroke_rectangle(a,b,radius)
                if polygon:nearby.append((rid,a,b,polygon))
        for i,(rid,a,b,polygon) in enumerate(nearby):
            for other,c,d,clip in nearby[:i]:
                if rid==other:continue
                cross=(b[0]-a[0])*(d[1]-c[1])-(b[1]-a[1])*(d[0]-c[0])
                if abs(cross)<1e-8*math.dist(a,b)*math.dist(c,d):continue
                overlap=polygon_overlap(polygon,clip)
                if overlap:
                    pad=4*scale
                    footprint.append([min(p[0] for p in overlap)-pad,min(p[1] for p in overlap)-pad,
                                      max(p[0] for p in overlap)+pad,max(p[1] for p in overlap)+pad])
        atom['box']=square_box([min(b[0] for b in footprint),min(b[1] for b in footprint),
                                max(b[2] for b in footprint),max(b[3] for b in footprint)])
        group={'atoms':[atom],'box':atom['box'][:]}
        changed=True
        while changed:
            changed=False
            for old in groups[:]:
                a,b=group['box'],old['box']
                if a[0]<=b[2] and b[0]<=a[2] and a[1]<=b[3] and b[1]<=a[3]:
                    group['atoms']+=old['atoms']
                    group['box']=square_box([min(a[0],b[0]),min(a[1],b[1]),max(a[2],b[2]),max(a[3],b[3])])
                    groups.remove(old);changed=True
        groups.append(group)
    for group in groups:
        group['events']=[e for atom in group['atoms'] for e in atom['events']]
        group['id']=min(e['id'] for e in group['events'])
        group['members']=set(r for atom in group['atoms'] for r in atom['members'])
    return groups


def crossing_records(atoms,orders,routes):
    for atom in atoms:
        atom['record']=orders.get(atom['id'],{})
        atom['order']=ordered_members(routes,atom['members'],atom['record'])


def crossing_scene(routes, network, orders, scale, mode, edit=None):
    _,network_atoms,edges=network
    atoms=[dict(atom) for atom in network_atoms]
    rendered,strokes=route_data(routes)
    route_map={r['routeId']:r for r in routes}
    route_strokes=defaultdict(list)
    for stroke in strokes:route_strokes[stroke['route']].append((stroke['a'],stroke['b']))
    groups=display_groups(atoms,route_strokes,route_map,scale,mode)
    crossing_records(atoms,orders,routes)
    found=False
    for group in groups:
        ids=[e['id'] for e in group['events']]
        record=max((atom['record'] for atom in group['atoms']),key=lambda item:item.get('serial',0),default={})
        group['order']=ordered_members(routes,group['members'],record)
        if edit and group['id']==edit['crossingId']:
            found=True;order=group['order'];rid=edit['routeId'];target=edit.get('beforeRouteId')
            if rid not in order or (target is not None and target not in order):raise ValueError('Route is no longer in this crossing')
            if rid!=target:
                order.remove(rid);order.insert(order.index(target) if target is not None else len(order),rid)
                record={'order':order[:],'serial':1+max((o.get('serial',0) for o in orders.values()),default=0)}
                for eid in ids:orders[eid]=record
        group['record']=record
    if edit and not found:raise ValueError('Crossing changed; select it again')
    crossing_records(atoms,orders,routes)
    for group in groups:
        record=max((atom['record'] for atom in group['atoms']),key=lambda record:record.get('serial',0),default={})
        group['record']=record
        group['order']=ordered_members(routes,group['members'],record)
    paint=[]
    output=[]
    for group in groups:
        x,y,right,bottom=group['box'];order=group['order'];members=group['members']
        marker_size=max(right-x,bottom-y)
        markers=[{'x':(x+right-marker_size)/2,'y':(y+bottom-marker_size)/2,'width':marker_size,'height':marker_size}]
        # The marker is the exact local order region; preview padding is display only.
        pad=max(marker_size*.25,4*scale)
        bounds=[x-pad,y-pad,right-x+2*pad,bottom-y+2*pad]
        local=[r['routeId'] for r in routes if r['routeId'] not in members]+list(reversed(order))
        layers=[{'routeId':rid,'width':route_map[rid]['width'] if mode=='styled' else 1} for rid in local
                if any(clip_line(a,b,bounds) for a,b in route_strokes[rid])]
        if group['record']:
            paint.append({'id':group['id'],'bounds':markers[0],
                          'routes':[{'routeId':rid,'width':route_map[rid]['width'] if mode=='styled' else 1,
                                     'paths':route_map[rid]['paths']} for rid in local]})
        factor=170/max(bounds[2],bounds[3],1);height=min(24,100/max(1,len(order)-1));projection=[]
        for level,rid in enumerate(reversed(order)):
            def project(p):
                dx,dy=p[0]-(x+right)/2,p[1]-(y+bottom)/2
                return [160+(dx-dy)*.7*factor,165+(dx+dy)*.32*factor-level*height]
            segments=[list(map(project,part)) for a,b in route_strokes[rid] if (part:=clip_line(a,b,bounds))]
            bx,by,w,h=bounds
            projection.append({'routeId':rid,'segments':segments,'plate':list(map(project,[(bx,by),(bx+w,by),(bx+w,by+h),(bx,by+h)])),
                               'width':max(2,min(12,(route_map[rid]['width'] if mode=='styled' else 1)*scale*factor))})
        size=max(bounds[2],bounds[3])
        output.append({'id':group['id'],'eventIds':[e['id'] for e in group['events']],'bounds':dict(zip(('x','y','width','height'),[x,y,right-x,bottom-y])),
                       'view':{'x':(x+right-size)/2,'y':(y+bottom-size)/2,'w':size,'h':size},
                       'markers':markers,'order':order,'layers':layers,'projection':projection})
    return output,paint


def crossing_details(scene,shape):
    nodes={node['id']:node for node in shape.get('nodes',[])}
    editable=set(scene.get('editNodeIds',nodes))
    for mode in ('styled','geometry'):
        for crossing in scene[mode+'Crossings']:
            view=crossing['view'];bounds=[view['x'],view['y'],view['w'],view['h']]
            def inside(p):return bounds[0]<=p[0]<=bounds[0]+bounds[2] and bounds[1]<=p[1]<=bounds[1]+bounds[3]
            crossing['shapePoints']=[node for node in nodes.values() if node['id'] in editable and not node['isStation'] and inside(node['position'])]
            pairs={}
            for edge in shape.get('segments',[]):
                shared=scene.get('sharedSegments',{}).get(edge['id'])
                if shared and clip_line(nodes[edge['a']]['position'],nodes[edge['b']]['position'],bounds):
                    for pair in shared['pairs']:pairs[tuple(sorted((pair['a'],pair['b'])))]=pair
            crossing['spacingPairs']=list(pairs.values())


def add_crossings(result,payload):
    if 'renderGeometries' not in result:return result
    state=result['state'];scale=result.get('spacingScale',1);edit=payload.get('crossingEdit')
    if edit and (edit.get('side') not in result['renderGeometries'] or edit.get('mode') not in ('styled','geometry')):raise ValueError('Invalid crossing window')
    networks={}
    for side,scene in result['renderGeometries'].items():
        orders=state.setdefault('styles',{}).setdefault(side,{}).setdefault('crossingOrders',{})
        geometry=scene.get('geometryRoutes',[])
        key=json.dumps(geometry,separators=(',',':'))
        if key not in networks:networks[key]=network_elements(geometry)
        network=networks[key]
        styled_network=network_elements(scene.get('routes',[]))
        canonical_paths,canonical_atoms,canonical_edges=network
        used=set()
        for atom in styled_network[1]:
            matches=[a for a in canonical_atoms if a['id'] not in used and atom['members']<=a['members']]
            if matches:
                nearest=min(matches,key=lambda a:math.dist(a['events'][0]['point'],atom['events'][0]['point']))
                used.add(nearest['id']);atom['id']=nearest['id'];atom['events'][0]['id']=nearest['id']
        # Process the edited mode first; both views share the same network order.
        modes=[('styled','routes'),('geometry','geometryRoutes')]
        if edit and edit['mode']=='geometry':modes.reverse()
        for mode,key in modes:
            scene[mode+'Crossings'],scene[mode+'CrossingPaint']=crossing_scene(scene.get(key,[]),styled_network if mode=='styled' else network,orders,scale,mode,
                edit if edit and edit['side']==side and edit['mode']==mode else None)
        crossing_details(scene,result.get('shape',state.get('shape',{})))
        scene['crossingOrders']=orders
    return result
