"""Optional incremental transport. Full responses remain the fallback across workers."""
from collections import OrderedDict
from copy import deepcopy
import json
from threading import Lock

_cache = OrderedDict()
_lock = Lock()


def diff(before, after, path=()):
    if before == after: return []
    if isinstance(before, dict) and isinstance(after, dict):
        result = [[list(path+(k,))] for k in before.keys()-after.keys()]
        for k,v in after.items():
            result.extend(diff(before[k],v,path+(k,)) if k in before else [[list(path+(k,)),v]])
        return result
    if isinstance(before,list) and isinstance(after,list) and len(before)==len(after):
        result=[]
        for i,(a,b) in enumerate(zip(before,after)): result.extend(diff(a,b,path+(i,)))
        # Coordinate arrays are cheaper to send whole than as many tiny edits.
        if len(json.dumps(result)) < len(json.dumps(after)): return result
    return [[list(path),after]]


def apply(document, changes):
    result = deepcopy(document)
    for change in changes:
        path=change[0]
        if not path:
            result=deepcopy(change[1]);continue
        target=result
        for key in path[:-1]: target=target[key]
        if len(change)==1: target.pop(path[-1],None)
        else: target[path[-1]]=deepcopy(change[1])
    return result


def response(body, result):
    viewport = body.get('viewportDelta', False)
    if not ((body.get('incremental') and 'shape' in result) or viewport): return result
    sid=result['sessionId']+(':viewport' if viewport else '')
    with _lock:
        previous=_cache.get(sid)
        _cache[sid]=deepcopy(result);_cache.move_to_end(sid)
        while len(_cache)>8: _cache.popitem(last=False)
    if previous and previous['revision']==body.get('responseBase') and (not viewport or previous.get('spacingScale')==body.get('baseScale')):
        delta={'sessionId':result['sessionId'],'revision':result['revision'],'baseRevision':previous['revision'],'baseScale':previous.get('spacingScale'),'changes':diff(previous,result),'incremental':True}
        if len(json.dumps(delta)) < len(json.dumps(result)): return delta
    return result
