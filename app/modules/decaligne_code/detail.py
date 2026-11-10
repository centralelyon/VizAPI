"""Semantic detail levels; independent from client zoom and drawing code."""
import math


def levels(shape, meta):
    routes = {r['id']:r for r in shape.get('routes', []) if not r.get('isObstacle')}
    manual = meta.get('levels', {})
    rows = {}
    for n in shape.get('nodes', []):
        membership = set(n.get('routeIds', [])) & routes.keys()
        if n.get('isStation') and membership:
            rows[n['id']] = manual.get(n['id'], len(membership))
    return rows


def selection(shape, meta, scale):
    priorities = levels(shape, meta)
    owners = meta.get('routes', {})
    modes = meta.get('modes', {})
    stations, routes, depths = set(), set(), {}
    for city in meta.get('cities', []):
        cid = city['id']
        city_routes = {r for r, owner in owners.items() if owner==cid}
        depths[cid] = 0
        groups = {}
        for route in city_routes: groups.setdefault(modes.get(route, 'transit'), set()).add(route)
        for mode, group in groups.items():
            nodes = [n for n in shape.get('nodes', []) if group.intersection(n.get('routeIds', []))]
            if not nodes: continue
            width = max(n['position'][0] for n in nodes)-min(n['position'][0] for n in nodes)
            height = max(n['position'][1] for n in nodes)-min(n['position'][1] for n in nodes)
            extent = max(width,height,1)/scale
            if mode=='hiking' and extent<400:
                candidates = [n for n in nodes if n['id'] in priorities]
                if candidates:
                    anchor = min(candidates, key=lambda n: (-priorities[n['id']], n['id']))
                    stations.add(anchor['id']); routes.update(group.intersection(anchor.get('routeIds', [])))
                    depths[cid] = max(depths[cid], 1)
                continue
            tiers = sorted({priorities[n['id']] for n in nodes if n['id'] in priorities}, reverse=True)
            depth = min(len(tiers), max(1, 1+int(math.log2(max(1,extent/80)))))
            shown = set(tiers[:depth]); depths[cid] = max(depths[cid],depth)
            for n in nodes:
                if priorities.get(n['id']) not in shown: continue
                membership = group.intersection(n.get('routeIds', []))
                if membership: stations.add(n['id']); routes.update(membership)
            if mode=='rail': routes.update(group)
    return {'stations':stations,'routes':routes,'depths':depths,'priorities':priorities}
