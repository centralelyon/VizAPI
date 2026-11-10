"""City identity, station ordering and zoom visibility (server-owned metadata)."""
from . import detail
from copy import deepcopy
import hashlib
import base64
import gzip
import json
import math
from pathlib import Path
from fastapi import HTTPException

KEY = 'transitNetworks'


def name_for(graph, fallback=''):
    city = graph.get('city') or graph.get('networkName') or graph.get('name') or graph.get('properties', {}).get('city')
    if isinstance(city, dict): city = city.get('name')
    return str(city or Path(fallback).stem or 'City').strip()


def points(graph):
    return [f['geometry']['coordinates'][:2] for f in graph.get('features', []) if f.get('geometry', {}).get('type') == 'Point']


def identify(graph, catalog, fallback='', requested=''):
    name = name_for(graph, requested or fallback)
    if requested: name = requested.strip()
    for entry in catalog:
        if entry['name'].casefold() == name.casefold(): return entry
    # Station coordinates, not route numbers or raw stop IDs, identify partial feeds.
    if not requested and not (graph.get('city') or graph.get('networkName')):
        coordinates = {tuple(round(float(v), 5) for v in p) for p in points(graph)}
        matches = [entry for entry in catalog if coordinates.intersection(map(tuple, entry.get('locations', [])))]
        if len(matches) == 1: return matches[0]
    identity = 'city-' + hashlib.sha256(name.casefold().encode()).hexdigest()[:12]
    return {'id': identity, 'name': name, 'prefix': '' if not catalog else identity + ':', 'locations': []}


def namespace(graph, prefix, old_prefix=''):
    """Feed-local IDs must never connect stations/routes belonging to other cities."""
    if not prefix and not old_prefix: return graph
    graph = deepcopy(graph)
    def key(value): return prefix + str(value).removeprefix(old_prefix)
    for f in graph['features']:
        p = f['properties']
        if 'id' in p: p['id'] = key(p['id'])
        if 'station_id' in p and p['station_id']: p['station_id'] = key(p['station_id'])
        for field in ('from', 'to'):
            if field in p: p[field] = key(p[field])
        for line in p.get('lines', []):
            line['id'] = key(line.get('id', line.get('label')))
            for field in ('from', 'to'):
                if field in line: line[field] = key(line[field])
    for r in graph.get('transitMapDirections', {}).get('routes', []):
        r['routeId'] = key(r['routeId'])
        # Directional node references are stop IDs in the source feed.
        for pattern in r.get('patterns', []):
            if 'routeId' in pattern: pattern['routeId'] = key(pattern['routeId'])
            if pattern.get('shapeId'): pattern['shapeId'] = key(pattern['shapeId'])
            for stop in pattern.get('orderedStops', []): stop['stopId'] = key(stop['stopId'])
            for field in ('stopIds', 'nodeIds'):
                if field in pattern: pattern[field] = [key(v) for v in pattern[field]]
    for r in graph.get('transitMapRouteDirections', {}).get('segments', []):
        for field in ('routeId', 'from', 'to'):
            if field in r: r[field] = key(r[field])
    timetable = graph.get('transitMapTimetable', {})
    for feed in timetable.get('feeds', []):
        tables = json.loads(gzip.decompress(base64.b64decode(feed['data'])))
        for table in tables.values():
            columns = [i for i, field in enumerate(table['columns']) if field in ('route_id', 'stop_id', 'parent_station', 'trip_id', 'service_id', 'agency_id', 'shape_id')]
            for row in table['rows']:
                for i in columns:
                    if i < len(row) and row[i]: row[i] = key(row[i])
        raw = json.dumps(tables, ensure_ascii=False, separators=(',', ':')).encode()
        feed.update(id=hashlib.sha256(raw).hexdigest(), data=base64.b64encode(gzip.compress(raw, mtime=0)).decode())
    if 'routeSources' in timetable:
        timetable['routeSources'] = {key(r): key(source) for r, source in timetable['routeSources'].items()}
    if 'routeModes' in graph: graph['routeModes'] = {key(r): mode for r,mode in graph['routeModes'].items()}
    graph['bidirectionalRoutes'] = [key(v) for v in graph.get('bidirectionalRoutes', [])]
    for feature in graph.get('transitMapObstacles', {}).get('features', []):
        if 'id' in feature: feature['id'] = key(feature['id'])
        for field in ('id', 'topologyRouteId'):
            if field in feature.get('properties', {}): feature['properties'][field] = key(feature['properties'][field])
    return graph


def prepare(payload):
    payload = deepcopy(payload)
    op = payload.get('op', 'session')
    state = payload.setdefault('state', {})
    meta = state.setdefault(KEY, {'cities': [], 'routes': {}, 'features': {}, 'orders': {}})
    if op in ('session', 'replace-map'):
        saved = payload['map'].get('transitMapNetworks')
        if saved: state[KEY] = deepcopy(saved); meta = state[KEY]
        elif op == 'session': state[KEY] = meta = {'cities': [], 'routes': {}, 'features': {}, 'orders': {}}
    if op in ('session', 'append-map', 'replace-map'):
        graph = payload['map']
        imported_order = None
        imported_levels = {}
        imported_ranges = {}
        saved = graph.get('transitMapNetworks', {})
        if op == 'append-map' and len(saved.get('cities', [])) == 1:
            source_city = saved['cities'][0]
            old_prefix = source_city.get('prefix', '')
            graph = namespace(graph, '', old_prefix)
            graph.pop('transitMapNetworks')
            graph['city'] = source_city['name']
            graph['routeModes'] = {str(r).removeprefix(old_prefix):mode for r,mode in saved.get('modes', {}).items()}
            imported_levels = {str(n).removeprefix(old_prefix):level for n,level in saved.get('levels', {}).items()}
            imported_ranges = {str(r).removeprefix(old_prefix): [str(n).removeprefix(old_prefix) for n in pair] for r,pair in saved.get('ranges', {}).items()}
            payload['map'] = graph
            imported_order = [str(n).removeprefix(old_prefix) for n in saved.get('orders', {}).get(source_city['id'], [])]
        if not graph.get('transitMapNetworks'):
            city = identify(graph, meta['cities'], payload.get('cityName') or payload.get('inputName', ''), payload.get('cityName', ''))
            if not any(c['id'] == city['id'] for c in meta['cities']): meta['cities'].append(city)
            city['locations'] = sorted(set(map(tuple, city.get('locations', []))) | {tuple(round(float(v), 5) for v in p) for p in points(graph)})
            if op == 'append-map': graph = namespace(graph, city['prefix']); payload['map'] = graph
            for f in graph['features']:
                for r in f['properties'].get('lines', []): meta['routes'][str(r.get('id', r.get('label')))] = city['id']
            for f in graph.get('transitMapObstacles', {}).get('features', []):
                f.setdefault('properties', {})['networkId'] = city['id']
            for feature in graph['features']:
                for route in feature['properties'].get('lines', []):
                    r = str(route.get('id', route.get('label')))
                    meta.setdefault('modes', {})[r] = graph.get('routeModes', {}).get(r, graph.get('inputType', 'transit'))
            payload['networkId'] = city['id']
            for r,pair in imported_ranges.items(): meta.setdefault('ranges', {}).setdefault(city['prefix']+r, [city['prefix']+n for n in pair])
            for n,level in imported_levels.items(): meta.setdefault('levels', {}).setdefault(city['prefix']+n,level)
            if imported_order and city['id'] not in meta['orders']:
                meta['orders'][city['id']] = [city['prefix'] + n for n in imported_order]
            if op == 'append-map':
                features=state.setdefault('obstacles', {'type':'FeatureCollection','features':[]})['features']
                signatures={(f.get('properties', {}).get('networkId'), str(f.get('geometry'))) for f in features}
                for f in graph.get('transitMapObstacles', {}).get('features', []):
                    signature=(city['id'], str(f.get('geometry')))
                    if signature not in signatures: features.append(deepcopy(f)); signatures.add(signature)
        elif op == 'append-map':
            incoming = graph['transitMapNetworks']
            for city in incoming['cities']:
                if not any(c['id'] == city['id'] for c in meta['cities']): meta['cities'].append(deepcopy(city))
            for field in ('routes', 'features', 'orders', 'levels', 'modes', 'ranges'): meta.setdefault(field, {}).update(incoming.get(field, {}))
    if op == 'trim-route' and meta.get('modes', {}).get(str(payload['routeId'])) == 'hiking':
        rid = str(payload['routeId'])
        if payload.get('fromNodeId') is None and payload.get('toNodeId') is None:
            meta.setdefault('ranges', {}).pop(rid, None)
        else:
            meta.setdefault('ranges', {})[rid] = [str(payload['fromNodeId']), str(payload['toNodeId'])]
        payload['op'] = 'render-geometry'
    if op == 'update-routes':
        replacements = {str(r['routeId']): str(r['id']) for r in payload['routes']}
        meta['routes'] = {replacements.get(r, r): city for r, city in meta['routes'].items()}
        for field in ('ranges', 'modes'):
            if field in meta: meta[field] = {replacements.get(r, r): value for r, value in meta[field].items()}
    if op == 'station-priorities':
        city = payload.get('networkId')
        transit_routes = {r['id'] for r in state['shape']['routes'] if not r.get('isObstacle')}
        known = {n['id'] for n in state['shape']['nodes'] if n.get('isStation') and any(meta['routes'].get(r) == city for r in set(n.get('routeIds', [])) & transit_routes)}
        if payload.get('priorityLevel') is not None:
            node = payload.get('nodeId'); level = payload['priorityLevel']
            current_levels = detail.levels(state['shape'], meta)
            maximum = max([current_levels.get(n, 1) for n in known]+[len(set(n.get('routeIds', [])) & transit_routes) for n in state['shape']['nodes'] if n['id'] in known]+[1])
            if node not in known or type(level) is not int or not 1<=level<=maximum:
                raise HTTPException(422, 'Invalid station priority tier')
            meta.setdefault('levels', {})[node] = level
            payload['op'] = 'render-geometry'
            op = 'render-geometry'
        order = payload.get('nodeIds')
        if order is None and payload.get('priorityLevel') is None:
            ranked = sorted((n for n in state['shape']['nodes'] if n['id'] in known), key=lambda n: (-len(set(n.get('routeIds', [])) & transit_routes), n.get('name', ''), n['id']))
            order = [n for n in meta['orders'].get(city, []) if n in known]
            order.extend(n['id'] for n in ranked if n['id'] not in order)
            moved, before = payload.get('nodeId'), payload.get('beforeNodeId')
            if moved not in known or before is not None and before not in known:
                raise HTTPException(422, 'Station does not belong to this city')
            order.remove(moved)
            order.insert(order.index(before) if before in order else len(order), moved)
        if order is not None and (len(order) != len(set(order)) or set(order) != known):
            raise HTTPException(422, 'Submit each station in this city exactly once')
        if order is not None: meta['orders'][city] = order
        payload['op'] = 'render-geometry'
    # Reject cross-city topology operations. Drawing can only snap to its own city.
    city = payload.get('networkId') or (meta['cities'][0]['id'] if meta['cities'] else '')
    payload['networkId'] = city
    owners = {n['id']: {meta['routes'].get(r) for r in n.get('routeIds', [])} - {None} for n in state.get('shape', {}).get('nodes', [])}
    if op == 'merge-stations':
        groups = set().union(*(owners.get(n, set()) for n in payload.get('nodeIds', [])))
        if len(groups) > 1: raise HTTPException(422, 'Stations from different cities cannot be merged')
    if op == 'draw-route' and city:
        payload['nodeIds'] = [n if not owners.get(n) or city in owners[n] else None for n in payload.get('nodeIds', [])]
        payload['cityNodeIds'] = [n for n, groups in owners.items() if city in groups]
        payload['citySegmentIds'] = [e['id'] for e in state['shape']['segments'] if any(meta['routes'].get(r)==city for r in e.get('routeIds', []))]
    if state.get('shape') and meta['cities'] and payload.get('op')=='render-geometry':
        chosen = detail.selection(state['shape'], meta, payload.get('spacingScale', 1))
        payload['detailRouteIds'] = sorted(chosen['routes'])
    return payload


def finish(result, payload):
    state = result.get('state', payload.get('state', {}))
    meta = state.get(KEY)
    if not meta: return result
    if 'map' in result:
        result['map']['transitMapNetworks'] = deepcopy(meta)
        return result
    shape = result.get('shape')
    if not shape: return result
    default = payload.get('networkId') or (meta['cities'][0]['id'] if meta['cities'] else '')
    for old, children in result.get('routeReplacements', {}).items():
        for child in children: meta['routes'][child['id']] = meta['routes'].get(old, default)
    for feature in state.get('obstacles', {}).get('features', []):
        props=feature.setdefault('properties', {})
        owner=props.get('networkId') or default
        props['networkId']=owner
        if props.get('topologyRouteId'): meta['routes'][props['topologyRouteId']]=owner
    for route in shape['routes']:
        route['networkId'] = meta['routes'].setdefault(route['id'], default)
        if meta.get('modes', {}).get(route['id'])=='hiking':
            route['reversibleRange'] = True
            route['rangeActive'] = route['id'] in meta.get('ranges', {})
            if route['rangeActive']:
                route['endpointIds'] = meta['ranges'][route['id']]
            elif len(route.get('endpointIds', []))<2 and route.get('stations'):
                route['endpointIds'] = [route['stations'][0]['id'], route['stations'][-1]['id']]
            names = {n['id']: n.get('name') or n['id'] for n in shape['nodes']}
            route['endpoints'] = [names.get(n, n) for n in route['endpointIds']]
    transit_routes = {r['id'] for r in shape['routes'] if not r.get('isObstacle')}
    priorities = detail.levels(shape, meta)
    nodes = {}
    for node in shape['nodes']:
        node['networkId'] = next((meta['routes'][r] for r in node.get('routeIds', []) if r in meta['routes']), default)
        node['routeCount'] = len(set(node.get('routeIds', [])) & transit_routes)
        nodes[node['id']] = node
    cities = []
    for city in meta['cities']:
        members = [n for n in shape['nodes'] if n['networkId'] == city['id']]
        stations = sorted((n for n in members if n.get('isStation') and n['routeCount']), key=lambda n: (-n['routeCount'], n.get('name', ''), n['id']))
        custom = {n: i for i, n in enumerate(meta['orders'].get(city['id'], []))}
        if custom: stations.sort(key=lambda n: (custom.get(n['id'], len(custom)), -n['routeCount'], n.get('name', ''), n['id']))
        for station in stations: station['priority'] = priorities.get(station['id'], 1)
        stations.sort(key=lambda n: -n['priority'])
        xs = [n['position'][0] for n in members]; ys = [n['position'][1] for n in members]
        bounds = {'x': min(xs), 'y': min(ys), 'width': max(xs)-min(xs), 'height': max(ys)-min(ys)} if xs else {'x': 0, 'y': 0, 'width': 1200, 'height': 800}
        cities.append({'id': city['id'], 'name': city['name'], 'bounds': bounds,
                       'backgroundBounds': {'x':bounds['x']-30,'y':bounds['y']-30,'width':max(60,bounds['width']+60),'height':max(60,bounds['height']+60)},
                       'backgroundOrigin': {'x':bounds['x'] if cities else 0,'y':bounds['y'] if cities else 0},
                       'routeIds': [r['id'] for r in shape['routes'] if not r.get('isObstacle') and r['networkId'] == city['id']],
                       'priorityTiers': [{'level':level,'rank':index+1,'width':round(40+60*(index+1)/max([n['priority'] for n in stations]+[n['routeCount'] for n in stations]+[1])),'stationIds':[n['id'] for n in stations if n['priority']==level]} for index,level in enumerate(range(max([n['priority'] for n in stations]+[n['routeCount'] for n in stations]+[1]),0,-1))],
                       'stations': [{'id': n['id'], 'name': n.get('name') or n['id'], 'routeCount': n['routeCount'], 'priority': n['priority']} for n in stations]})
    for obstacle in result.get('obstacles', []):
        obstacle['networkId'] = obstacle.get('properties', {}).get('networkId', default)
    scale = result.get('spacingScale', 1)
    chosen = detail.selection(shape, meta, scale)
    for scene in result.get('renderGeometries', {}).values():
        for node in scene.get('displayNodes', []):
            source = nodes.get(node['id'], {})
            for field in ('networkId', 'priority', 'routeCount'): node[field] = source.get(field)
        present = {n['id'] for n in scene.get('displayNodes', []) if n.get('isStation')}
        scene['visibleStationIds'] = sorted(chosen['stations'] & present)
        for route_id in meta.get('ranges', {}):
            candidates = [n for n in scene.get('displayNodes', []) if n.get('isStation') and route_id in n.get('routeIds', [])]
            if candidates and not any(n['id'] in scene['visibleStationIds'] for n in candidates):
                scene['visibleStationIds'].append(min(candidates, key=lambda n: (-priorities.get(n['id'], 1), n['id']))['id'])
        scene['visibleRouteIds'] = sorted(chosen['routes'])
        scene['detailDepths'] = chosen['depths']
    result['networks'] = cities
    if shape['nodes']:
        xs=[n['position'][0] for n in shape['nodes']];ys=[n['position'][1] for n in shape['nodes']]
        w=max(1200,(max(xs)-min(xs))*1.15,(max(ys)-min(ys))*1.15*1.5)
        result['fitView']={'x':(min(xs)+max(xs)-w)/2,'y':(min(ys)+max(ys)-w/1.5)/2,'w':w,'h':w/1.5}
    return result
