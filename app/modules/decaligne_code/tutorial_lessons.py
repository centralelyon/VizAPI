"""Independent lesson inputs and target recipes, executed by the existing core.

The fixture is private to the tutorial. No current editor map is accepted here.
Each recipe starts a new edit session; inputs never depend on previous lessons.
"""
import base64
from copy import deepcopy
from functools import lru_cache
import json
from pathlib import Path
import struct
import zlib
from fastapi import APIRouter, HTTPException
from .tutorial import STEPS, evaluate, reference_style, targets, step, MAP
from .edit_sessions import core_call

router = APIRouter()
step('endpoint-select','Legend','Select termini','Set A start to Cordeliers and end to Vaulx-en-Velin–La Soie.','@endpoints:A','endpoint-draft')
step('endpoint-apply','Legend','Apply termini','Click Apply on route A.','@apply:A','operation',operation='trim-route')
step('drawing-osm','Draw','OpenStreetMap','Turn on OpenStreetMap to trace the river or park.','[aria-label="OpenStreetMap"]','parameter',key='background-osm',value='true')
step('displacement-panel','Displacement','Station displacement','Inspect the station displacement panel at the bottom left.','[aria-label="Expand station displacement"]','field',field='displacementPanelVisible',value=True)

step('river-second-start','Draw','Saône','Click Draw line feature again for the Saône.','[aria-label="Draw line feature"]','drawing',operation='draw-line-feature')
step('river-second','Draw','Saône','Click the numbered points, then press Enter.',MAP,'operation',operation='draw-line-feature',gesture='draw')
step('comment-open','Comments','Comments','Expand the Comments panel.','.commentsDockToggle','field',field='commentOpen',value=True)
step('comment-mode','Comments','Comment mode','Click Comments to enable comment mode.','.commentsPanel > button','field',field='commentMode',value=True)
step('comment-draw','Comments','Mark an area','Drag a rectangle around a station on the canvas.',MAP,'field',field='commentDraft',value=True,gesture='drag')
step('comment-save','Comments','Write a comment','Enter a comment, then click OK.','.commentModal form','field',field='commentCount',value=1)



def leaf(id, label): return {'id':id,'label':label}
def folder(label, *children): return {'label':label,'children':list(children)}

TREE = [
    leaf('routes','Select Route → A, B, C, D'),
    folder('Legend panel',leaf('endpoints','Start / end stations'),leaf('name','Route name'),leaf('color','Route color'),leaf('width','Route width')),
    folder('Edit',
        folder('Station',leaf('merge','Merge · Hôtel de Ville (A + C)'),leaf('add-station','Add'),leaf('delete-station','Delete added station')),
        folder('Segment',leaf('divide','Divide'),leaf('delete-segment','Delete')),
        folder('Draw',leaf('draw-route','New route'),leaf('draw-line-feature','Rivers · line features'),leaf('draw-region-feature','Parks · region features')),
        leaf('octi','OCTI algorithm'),leaf('displacement','Displacement')),
    folder('Style panel',
        folder('Background',leaf('background','Color'),leaf('osm','Open Street Map'),leaf('image','Image')),
        leaf('obstacles','Obstacles'),leaf('route-style','Route'),leaf('station-style','Station'),leaf('label-style','Label')),
    folder('Comparison: two / four windows',leaf('windows','Two / four windows'),leaf('styles','Style comparison'),leaf('versions','Save version and version comparison')),
    leaf('comments','Comment system'),
    folder('Download',leaf('svg','SVG'),leaf('json','JSON')),
]
LESSONS = {
    'routes':['apply-routes'],
    'endpoints':['endpoint-select','endpoint-apply'], 'name':['rename'], 'color':['appearance','legend-color'], 'width':['appearance','legend-width'],
    'merge':['edit','merge-select','merge'], 'delete-station':['station-select','station-delete'],
    'add-station':['segment-select-add','station-add'], 'divide':['segment-select-divide','segment-divide'],
    'delete-segment':['segment-select-delete','segment-delete'],
    **{f'draw-{kind}':[f'draw-start-{kind}',f'draw-{kind}'] for kind in ['route','line-feature','region-feature']},
    'comments':['comment-open','comment-mode','comment-draw','comment-save'],
    'octi':['octi'], 'displacement':['displacement','displacement-route','displacement-panel'],
    'background':['background'], 'osm':['params','osm','osm-off'], 'image':['image','image-remove'], 'obstacles':['obstacles'],
    'route-style':['route-style'], 'station-style':['station-style'], 'label-style':['names','label-style'],
    'windows':['two','four','divider'], 'styles':['two','compare-style'],
    'versions':['save','version-menu','compare-saved','compare-version'],
    'svg':['download-menu','svg'], 'json':['download-menu','json'],
}

LESSONS['draw-line-feature'].insert(0,'drawing-osm')
LESSONS['draw-region-feature'].insert(0,'drawing-osm')

def ordered(nodes):
    return [id for node in nodes for id in (ordered(node['children']) if 'children' in node else [node['id']])]
ORDER = ordered(TREE)

def title(id, tree=TREE):
    for node in tree:
        if node.get('id')==id: return node['label']
        if 'children' in node:
            value=title(id,node['children'])
            if value:return value


@router.get('/tutorial/catalog', include_in_schema=False)
def catalog():
    return {'tree':TREE,'first':'routes','count':len(LESSONS),'order':ORDER}


@lru_cache(maxsize=1)
def example():
    graph=json.loads(Path(__file__).with_name('tutorial_lyon.json').read_text())
    graph['transitMapObstacles']['features']=[]
    result=core_call({'op':'session','map':graph,'deferRender':True})
    return graph,result


def apply(result, operation, **body):
    return core_call({'op':operation,'state':result['state'],'deferRender':True,**body})


def image_values():
    # A small bundled practice image: all normalization remains on the server.
    from .background_image import prepare_background
    w,h=300,200
    raw=b''.join(b'\0'+b''.join(bytes((225,245,252,255) if abs(x-100-y//3)<15 else (246,246,246,255)) for x in range(w)) for y in range(h))
    def chunk(kind,data):return struct.pack('!I',len(data))+kind+data+struct.pack('!I',zlib.crc32(kind+data)&0xffffffff)
    png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('!2I5B',w,h,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b'')
    return prepare_background(png)['values']


@lru_cache(maxsize=32)
def build_lesson(id):
    if id not in LESSONS:raise HTTPException(404,'Unknown tutorial unit')
    graph,result=deepcopy(example())
    metro,hotels,station,segment=targets({'routes':result['shape']['routes'],'nodes':result['shape']['nodes'],'edges':result['shape']['segments']})
    base_ops=[]
    phase=ORDER.index(id)

    if phase>ORDER.index('merge') and len(hotels)>1:
        op={'operation':'merge-stations','body':{'nodeIds':[n['id'] for n in hotels]}}
        result=apply(result,op['operation'],**op['body']);base_ops.append(op)
    metro,hotels,station,segment=targets({'routes':result['shape']['routes'],'nodes':result['shape']['nodes'],'edges':result['shape']['segments']})
    if id=='delete-station':
        op={'operation':'add-station','body':{'segmentId':segment['id'],'name':'Station'}}
        result=apply(result,op['operation'],**op['body']);base_ops.append(op)
        station=next(n for n in result['shape']['nodes'] if n.get('name')=='Station')
    style=reference_style({'style':{'values':{},'overrides':{}},'routes':result['shape']['routes']})['style']
    for scoped in style['overrides'].values():scoped.pop('route-width',None)
    route_ids=[metro[c]['id'] for c in 'ABCD']
    ui={'legendOpen':id in ('routes','endpoints','name','color','width'),'visibleRoutes':route_ids,'labels':phase>ORDER.index('label-style'),'editing':False,'paramsOpen':False,'windowCount':1,'visualization':False,'split':50,'horizontalSplit':50}
    input_doc={'map':graph,'operations':base_ops,'style':style,'ui':ui}
    output=deepcopy(input_doc)
    goal='Lyon A · B · C · D'
    demo=[]
    def operation(op,**body):output['operations'].append({'operation':op,'body':body})
    if id=='routes':
        input_doc['ui']['visibleRoutes']=[];goal='Only A, B, C and D are visible.'
    elif id=='endpoints':
        route=metro['A'];end=next(n['id'] for n in route['stations'] if 'Vaulx' in n['name'])
        operation('trim-route',routeId=route['id'],fromNodeId=station['id'],toNodeId=end)
        goal=f'A: Cordeliers → {next(n["name"] for n in route["stations"] if n["id"]==end)}'
    elif id=='name':
        operation('update-routes',routes=[{'routeId':metro['A']['id'],'id':metro['A']['id'],'name':'Metro A'}]);goal='A → Metro A'
        output['style']['overrides']['Metro A']=output['style']['overrides'].pop(metro['A']['name'])
    elif id in ('color','width'):
        input_doc['style']['overrides'][metro['A']['name']]['route-'+id]='#777777' if id=='color' else '3'
        goal='A · #df4086' if id=='color' else 'A · width 8'
    elif id=='merge':
        operation('merge-stations',nodeIds=[n['id'] for n in hotels]);goal='One shared A/C station at Hôtel de Ville.'
    elif id in ('delete-station','add-station','divide','delete-segment'):
        input_doc['ui']['editing']=True;output['ui']['editing']=True
        if id=='delete-station':operation('delete-node',nodeIds=[station['id']]);goal='Remove the station just added.'
        if id=='add-station':operation('add-station',segmentId=segment['id'],name='Station');goal='A new station on the highlighted segment.'
        if id=='divide':operation('split-segment',segmentId=segment['id']);goal='One new shape point on the segment.'
        if id=='delete-segment':
            operation('delete-segment',segmentIds=[segment['id']]);output['ui']['visibleRoutes']=None;goal='A cut at the highlighted segment.'
    elif id in ('draw-line-feature','draw-region-feature'):
        input_doc['ui']['editing']=True;output['ui']['editing']=True
        goal='Trace a river on OpenStreetMap.' if id=='draw-line-feature' else 'Trace a park on OpenStreetMap.'
    elif id=='draw-route':
        input_doc['ui']['editing']=True;output['ui']['editing']=True
        input_doc['ui']['visibleRoutes']=None;output['ui']['visibleRoutes']=None
        goal='Draw your own route.'
    elif id=='octi':operation('loom');goal='The same four lines with an OCTI layout.'
    elif id=='displacement':
        moved={'operation':'move-node','body':{'nodeId':station['id'],'x':station['position'][0]+40,'y':station['position'][1]+25,'snap':False}}
        input_doc['operations'].append(moved);output['operations'].append(deepcopy(moved))
        output['ui']['visualization']=True;goal='Original and edited station positions overlaid.'
    elif id in ('background','obstacles','route-style','station-style','label-style'):
        key,value={'background':('background-color','#e8e8e8'),'obstacles':('water-color','#bababa'),'route-style':('route-width','3'),'station-style':('normal-fill','#222222'),'label-style':('label-color','#999999')}[id]
        input_doc['style']['values'][key]=value
        input_doc['ui']['paramsOpen']=True;output['ui']['paramsOpen']=True
        if id=='label-style':input_doc['ui']['labels']=False
        goal={'background':'White background.','obstacles':'Pale blue rivers.','route-style':'Route width 8.','station-style':'White station dots.','label-style':'Station names in #222222.'}[id]
    elif id=='osm':output['style']['values']['background-osm']='true';goal='OpenStreetMap behind Lyon ABCD.'
    elif id=='image':
        output['style']['values'].update(image_values());input_doc['ui']['paramsOpen']=True;output['ui']['paramsOpen']=True
        goal='The practice image behind the network.'
    elif id=='windows':output['ui'].update(windowCount=4,split=58,horizontalSplit=58);goal='Four windows with a movable divider.'
    elif id=='styles':
        output['ui']['windowCount']=2;output['ui']['rightStyle']='classic.css';goal='Reference style on the left, classic.css on the right.'
    elif id=='versions':
        output['ui']['prepareVersions']=True;goal='Saved “Lyon example” compared with current.'
    elif id in ('svg','json'):goal=f'The complete Lyon example, ready for {id.upper()} download.'
    # Start with a neutral, unlabelled map; styling is introduced by its units.
    if phase<ORDER.index('background'):
        for document in (input_doc,output):
            document['ui']['labels']=False
            document['style']['values'].update({'bend-90-offset-in':'0','bend-90-offset-out':'0','bend-135-offset-in':'0','bend-135-offset-out':'0'})
    if phase<ORDER.index('background'):input_doc['style']['values']['background-color']='#e8e8e8'
    if phase<ORDER.index('route-style'):input_doc['style']['values']['route-width']='3'
    if phase<ORDER.index('station-style'):input_doc['style']['values']['normal-fill']='#222222'
    if phase<ORDER.index('label-style'):input_doc['style']['values']['label-color']='#999999'
    if phase<=ORDER.index('color'):input_doc['style']['overrides'][metro['A']['name']]['route-color']='#777777'
    if phase<=ORDER.index('width'):input_doc['style']['overrides'][metro['A']['name']]['route-width']='3'
    # Materialize completed prerequisites once on the server. Starting a unit
    # renders one map instead of rendering every prior river/park operation.
    prerequisites=[op for op in base_ops if op['operation']!='move-node']
    if prerequisites:
        prepared=core_call({'op':'session','map':graph,'deferRender':True})
        for op in prerequisites:prepared=apply(prepared,op['operation'],**op['body'])
        checkpoint=core_call({'op':'export','state':prepared['state']})['map']
        for document in (input_doc,output):
            document['map']=deepcopy(checkpoint)
            document['operations']=document['operations'][len(prerequisites):]
    return {'id':id,'title':title(id),'steps':[next(i for i,s in enumerate(STEPS) if s['id']==key) for key in LESSONS[id]],
            'input':input_doc,'output':output,'goal':goal,'demo':demo,
            'carryMap':id in ('add-station','draw-route','draw-line-feature','draw-region-feature','octi','comments'),
            'holdOnComplete':id in ('displacement','windows','styles','versions'),
            'sampleImage':('data:image/png;base64,'+output['style']['values']['background-image-base64']) if id=='image' else None}


@router.get('/tutorial/lesson/{id}', include_in_schema=False)
def lesson(id: str):
    return build_lesson(id)


@router.post('/tutorial/lesson-step', include_in_schema=False)
def lesson_step(body: dict):
    unit=build_lesson(body['lessonId'])
    local=max(0,min(len(unit['steps'])-1,int(body.get('index',0))))
    if body['lessonId']=='delete-station':
        body=deepcopy(body)
        for node in body.get('state',{}).get('nodes',[]):
            if node.get('name')=='Station':node['name']='Cordeliers'
            elif 'Cordeliers' in node.get('name',''):node['name']='Existing station'
    result=evaluate({**body,'index':unit['steps'][local]})
    if body['lessonId']=='routes':
        result['hint']='Select A, B, C and D, then click Done.'
        result['target']='[data-tutorial="routes-done"]';result['watch']=['visibleRoutes']
    if body['lessonId']=='name':result['hint']='Rename A to Metro A, then press Enter.'
    if body['lessonId'] in ('delete-station','delete-segment') and result['rule']=='operation':result['hint']=result['hint'].replace('; next we will undo.','.').replace('; next we will undo','.').replace('this station','the added station')
    if body['lessonId'] in ('draw-line-feature','draw-region-feature') and result.get('gesture'):
        result['demo']=[]
        result['hint']='Trace a river on OpenStreetMap; press Enter to finish.' if body['lessonId']=='draw-line-feature' else 'Trace a park boundary on OpenStreetMap; press Enter to close it.'
    if body['lessonId']=='draw-route' and result.get('gesture'):
        result['demo']=[];result['hint']='Click to draw your route; press Enter to finish.'
    if body['lessonId']=='image' and local==0:result['hint']='Open Background → Image and upload the sample image.'
    if body['lessonId']=='styles' and result['id']=='compare-style':result['hint']='Choose classic.css on the right.'
    result.update(index=local,total=len(unit['steps']))
    return result


@router.post('/tutorial/continue', include_in_schema=False)
def continue_lesson(body: dict):
    """Sequential learning retains the user's actual geometry, style and comments.

    Clicking a tree entry instead uses the independent canonical lesson input.
    """
    unit=deepcopy(build_lesson(body['id']))
    document=unit['input']
    live=bool(body.get('live') or body.get('from','').startswith('draw-'))
    linked=body.get('from')=='add-station' and body['id']=='delete-station'
    if live or linked or body.get('from') in ('octi','comments'):
        document['map']=body['map']
        document['style']=body['style']
        document['operations']=[]
        document['ui']['visibleRoutes']=None
    unit['live']=live
    if body['id']=='displacement' and (live or linked or body.get('from')=='octi'):
        # This exercise needs a small, real edit relative to its original map.
        result=core_call({'op':'session','map':body['map'],'deferRender':True})
        _,_,station,_=targets({'routes':result['shape']['routes'],'nodes':result['shape']['nodes'],'edges':result['shape']['segments']})
        document['operations']=[{'operation':'move-node','body':{'nodeId':station['id'],'x':station['position'][0]+40,'y':station['position'][1]+25,'snap':False}}]
    return unit
