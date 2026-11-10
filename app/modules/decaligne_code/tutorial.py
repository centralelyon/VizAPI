"""Lyon walkthrough: target resolution, completion rules and reference styling.

No map mutations here: every exercise uses the existing edit endpoints.
The browser sends UI state and successful-operation receipts, and paints hints.
"""
import re
import unicodedata
from copy import deepcopy
from fastapi import APIRouter

router = APIRouter()
STEPS = []


def step(id, chapter, title, hint, target, rule='visit', **extra):
    STEPS.append(dict(id=id, chapter=chapter, title=title, hint=hint,
                      target=target, rule=rule, **extra))


def button(label):
    return f'[data-tutorial="{label}"]'


MAP = '[data-comparison-side="left"] .mainSvg'
step('city', 'Select routes', 'Lyon', 'Choose Lyon in City.', button('city'), 'city')
step('clear', 'Select routes', 'Select routes', 'Clear the selection, then choose A, B, C and D.', button('clear'), 'empty')
for letter in 'ABCD':
    step('route-'+letter, 'Select routes', 'Metro '+letter, 'Tick the highlighted route (use Find route if needed).', '@route:'+letter, 'route', letter=letter, fallback='[aria-label="Find route"]')
step('apply-routes', 'Select routes', 'A · B · C · D', 'Click Done to load these four lines.', button('routes-done'), 'routes')
step('styled', 'Legend', 'Choose a style', 'Select soft.css (keep the left canvas active).', '[aria-label="Top left style"]', 'style-name', value='soft.css')
step('endpoints', 'Legend', 'Start → end', 'Keep both termini; these menus trim a route.', '.legendEndpoints', 'operation', operation='trim-route')
step('rename', 'Legend', 'Route name', 'Click A, then Enter to keep its name.', '@name:A', 'operation', operation='update-routes')
step('appearance', 'Legend', 'Color + width', 'Click the colored line sample.', '@sample:A', 'appearance')
step('legend-color', 'Legend', 'Metro A · pink', 'Set A to #df4086.', '.legendAppearance input[type="color"]', 'route-color', letter='A', value='#df4086')
step('legend-width', 'Legend', 'Route width', 'Set width to 8, then click Done.', '.legendAppearance', 'route-width', letter='A', value=8)
step('edit', 'Edit', 'Edit Mode', 'Turn on Edit Mode.', button('edit'), 'field', field='editing', value=True)
step('merge-select', 'Edit', 'Hôtel de Ville · A + C', 'Shift-click the two highlighted station points.', '@hotel', 'merge-selection', gesture='multi')
step('merge', 'Edit', 'Merge interchange', 'Click Merge to join the A/C station points.', '[aria-label="Merge selected points"]', 'operation', operation='merge-stations')
step('drag', 'Edit', 'Move a station', 'Drag the highlighted station a short distance.', '@station', 'operation', operation='move-node', gesture='drag')
step('station-select', 'Edit', 'Delete a station', 'Click the highlighted station.', '@station', 'node-selection')
step('station-delete', 'Edit', 'Delete', 'Delete this station; next we will undo.', '[aria-label="Delete selected points"]', 'operation', operation='delete-node')
step('segment-select-add', 'Edit', 'Add a station', 'Click the highlighted segment.', '@segment', 'segment-selection')
step('station-add', 'Edit', 'Add', 'Click Add to insert a station.', '[aria-label="Add station at selected point or segment"]', 'operation', operation='add-station')
step('segment-select-divide', 'Edit', 'Divide a segment', 'Click the highlighted segment.', '@segment', 'segment-selection')
step('segment-divide', 'Edit', 'Divide', 'Click Divide to insert a shape point.', '[aria-label="Insert midpoint in selected segment"]', 'operation', operation='split-segment')
step('segment-select-delete', 'Edit', 'Delete a segment', 'Click the highlighted segment.', '@segment', 'segment-selection')
step('segment-delete', 'Edit', 'Delete', 'Cut this segment; next we will undo.', '[aria-label="Cut all routes at selected segments"]', 'operation', operation='delete-segment')
for kind, title, label in [('route','New route','Draw route'),('line-feature','Line feature','Draw line feature'),('region-feature','Region feature','Draw region feature')]:
    step('draw-start-'+kind, 'Draw', title, 'Click the highlighted drawing tool.', f'[aria-label="{label}"]', 'drawing', operation='draw-'+kind)
    step('draw-'+kind, 'Draw', title, 'Click 3 points in a blank area; Enter to finish.', MAP, 'operation', operation='draw-'+kind, gesture='polygon' if kind=='region-feature' else 'draw')
step('octi', 'OCTI', 'Octilinear layout', 'Click OCTI; wait for the server layout.', button('octi'), 'operation', operation='loom')
step('edit-off', 'Displacement', 'Leave Edit Mode', 'Turn Edit Mode off.', button('edit'), 'field', field='editing', value=False)
step('displacement', 'Displacement', 'Compare positions', 'Turn on Displacement.', button('displacement'), 'field', field='visualization', value=True)
step('displacement-route', 'Displacement', 'Station displacement', 'Double-click the highlighted A route to open station displacement.', '@displacement:A', 'visit', gesture='double')
step('displacement-off', 'Displacement', 'Back to the map', 'Turn off Displacement.', button('displacement'), 'field', field='visualization', value=False)
step('params', 'Style', 'Style panel', 'Expand the style panel.', '.paramsDockToggle', 'field', field='paramsOpen', value=True)
step('background', 'Style', 'White background', 'Open Color and set #ffffff.', '[data-tutorial-group="Background"]', 'parameter', key='background-color', value='#ffffff')
step('osm', 'Style', 'OpenStreetMap', 'Turn on the geographic background.', '[aria-label="OpenStreetMap"]', 'parameter', key='background-osm', value='true', optional=True)
step('osm-off', 'Style', 'Clean background', 'Turn OpenStreetMap off again.', '[aria-label="OpenStreetMap"]', 'parameter', key='background-osm', value='false', default='false')
step('image', 'Style', 'Image background', 'Open Image and upload a practice image (optional).', '[data-tutorial-group="Background"]', 'image', optional=True)
step('image-remove', 'Style', 'Remove practice image', 'Click Remove image to return to white.', '[data-tutorial-group="Background"]', 'no-image')
step('obstacles', 'Style', 'River + park', 'Open Obstacles → River · Color; set #e3f6fc.', '[data-tutorial-group="Obstacles"]', 'parameter', key='water-color', value='#e3f6fc')
step('route-style', 'Style', 'Route', 'Open Route → Width; set 8.', '[data-tutorial-group="Route"]', 'parameter', key='route-width', value='8')
step('station-style', 'Style', 'Station', 'Open Normal station → Fill; set #ffffff.', '[data-tutorial-group="Normal station"]', 'parameter', key='normal-fill', value='#ffffff')
step('names', 'Style', 'Station labels', 'Turn Names on.', button('names'), 'field', field='labels', value=True)
step('label-style', 'Style', 'Label', 'Open Label → Color; set #222222.', '[data-tutorial-group="Label"]', 'parameter', key='label-color', value='#222222')
step('reference', 'Style', 'Lyon reference finish', 'Apply the reference palette, white dots and rounded bends.', MAP, 'recipe', action='reference')
step('save', 'Comparison', 'Save your Lyon map', 'Click Save, name it Lyon, then OK.', button('save'), 'operation', operation='save-version')
step('two', 'Comparison', 'Two windows', 'Click the two-window button.', '[aria-label="2 windows"]', 'field', field='windowCount', value=2)
step('compare-style', 'Comparison', 'Style comparison', 'Choose a different style on the right.', '[aria-label="Top right style"]', 'different-styles')
step('four', 'Comparison', 'Four windows', 'Click the four-window button.', '[aria-label="4 windows"]', 'field', field='windowCount', value=4)
step('divider', 'Comparison', 'Resize windows', 'Drag the center handle.', '[aria-label="Move window dividers"]', 'divider', gesture='drag')
step('version-menu', 'Comparison', 'Version comparison', 'Open Download to find your saved versions.', button('download'), 'field', field='downloadOpen', value=True)
step('compare-saved', 'Comparison', 'Saved Lyon version', 'Click compare beside the saved Lyon version.', '.versionCompareButton', 'saved-version')
step('compare-version', 'Comparison', 'Saved ↔ current', 'Click compare beside current to compare both versions.', '.versionMenu tbody tr:first-child .versionCompareButton', 'versions')
step('return-current', 'Download', 'Current map', 'Remove both versions using their compare − buttons.', '.versionCompareButton', 'current')
step('final-reference', 'Download', 'Final Lyon map', 'Restore the reference finish on the left canvas.', MAP, 'recipe', action='reference')
step('download-menu', 'Download', 'Download', 'Open Download.', button('download'), 'field', field='downloadOpen', value=True)
step('svg', 'Download', 'SVG', 'Click svg in the current row.', '.versionMenu tbody tr:first-child td:nth-child(3) a', 'download', format='svg')
step('json', 'Download', 'JSON', 'Click json in the current row.', '.versionMenu tbody tr:first-child td:nth-child(4) a', 'download', format='json')


def normalized(text):
    return ''.join(c for c in unicodedata.normalize('NFKD', str(text)).lower() if not unicodedata.combining(c))


def route_letter(route):
    name = normalized(route.get('name', ''))
    match = re.fullmatch(r'(?:(?:metro|ligne|line)\s*[-:]?\s*)?([abcd])', name.strip())
    return match.group(1).upper() if match else None


def targets(state):
    routes = state.get('routes', [])
    metro = {route_letter(r): r for r in routes if route_letter(r)}
    ids = {r['id'] for r in metro.values()}
    nodes = state.get('nodes', [])
    hotels = [n for n in nodes if 'hotel de ville' in normalized(n.get('name', '')) and set(n.get('routeIds', [])) & ids]
    stations = [n for n in nodes if n.get('isStation') and set(n.get('routeIds', [])) & ids]
    station = next((n for n in stations if 'cordeliers' in normalized(n.get('name', ''))), stations[0] if stations else None)
    edges = state.get('edges', [])
    segment = next((e for e in edges if station and station['id'] in (e.get('a'), e.get('b'))), None)
    return metro, hotels, station, segment


def css_value(value):
    # CSS attribute strings, including IDs from user datasets.
    return str(value).replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\a ')


def evaluate(body):
    index = max(0, min(len(STEPS)-1, int(body.get('index', 0))))
    current = deepcopy(STEPS[index])
    state, baseline = body.get('state', {}), body.get('baseline', {})
    events = body.get('events', [])
    metro, hotels, station, segment = targets(state)
    target = current['target']
    if target.startswith('@'):
        kind, _, letter = target[1:].partition(':')
        route = metro.get(letter)
        if kind in ('route','name','sample','endpoints','apply','displacement'):
            suffix = {'route':'input[type="checkbox"]','name':'.legendName','sample':'.legendSample','endpoints':'.legendEndpoints select','apply':'.legendEndpoints button','displacement':''}[kind]
            current['target'] = f'[data-tutorial-route="{css_value(route["id"])}"] {suffix}' if route else '.legend'
            if kind in ('endpoints','apply') and route:current['endpointTarget']=f'[data-tutorial-route="{css_value(route["id"])}"] .legendEndpoints'
            if kind=='displacement' and route:
                current['target']=f'[data-comparison-side="left"] [data-visualization-route="{css_value(route["id"])}"]'
                current['singleTarget']=True

        else:
            chosen = hotels if kind=='hotel' else [station] if kind=='station' and station else [segment] if segment else []
            if kind in ('station','segment'):current['singleTarget']=True
            attr = 'data-segment-id' if kind=='segment' else 'data-node-id'
            current['target'] = ','.join(f'[data-comparison-side="left"] [{attr}="{css_value(n["id"])}"]' for n in chosen) or MAP
    values = state.get('style', {}).get('values', {})
    selected = state.get('selection', {})
    rule = current['rule']
    completed = False
    if rule=='city': completed = 'lyon' in str(state.get('city','')).lower() and len(metro)==4
    elif rule=='empty': completed = state.get('draftRoutes')==[]
    elif rule=='route': completed = metro.get(current['letter'],{}).get('id') in (state.get('draftRoutes') or [])
    elif rule=='routes': completed = len(metro)==4 and set(state.get('visibleRoutes') or [])=={r['id'] for r in metro.values()} and state.get('ready',False)
    elif rule=='style-name': completed = state.get('selected')==current['value']
    elif rule=='visit': completed = any(e.get('type')=='target' for e in events)
    elif rule=='endpoint-draft':completed=state.get('endpointPending',False) or any(e.get('operation')=='trim-route' for e in events)
    elif rule=='appearance': completed = state.get('appearanceOpen',False)
    elif rule in ('route-color','route-width'):
        route = metro.get(current['letter'],{})
        override = state.get('style',{}).get('overrides',{}).get(route.get('name'),{})
        actual = override.get(rule, values.get(rule))
        completed = str(actual).lower()==str(current['value']).lower() and (rule!='route-width' or not state.get('appearanceOpen'))
    elif rule=='field': completed = state.get(current['field'])==current['value']
    elif rule=='merge-selection': completed = len(hotels)>1 and {n['id'] for n in hotels}.issubset(set(selected.get('nodes',[])))
    elif rule=='node-selection': completed = station is not None and station['id'] in selected.get('nodes',[])
    elif rule=='segment-selection': completed = segment is not None and selected.get('segments')==[segment['id']]
    elif rule=='operation': completed = any(e.get('operation')==current['operation'] for e in events)
    elif rule=='undo': completed = any(e.get('type')=='undo' for e in events)
    elif rule=='drawing': completed = state.get('drawing')==current['operation'] or any(e.get('operation')==current['operation'] for e in events)
    elif rule=='parameter': completed = str(values.get(current['key'],current.get('default',''))).lower()==current['value']
    elif rule=='image': completed = bool(values.get('background-image-base64'))
    elif rule=='no-image': completed = not values.get('background-image-base64')
    elif rule=='recipe': completed = any(e.get('type')=='recipe' for e in events)
    elif rule=='different-styles': completed = state.get('selections',{}).get('left')!=state.get('selections',{}).get('right')
    elif rule=='divider': completed = (state.get('split'),state.get('horizontalSplit'))!=(baseline.get('split'),baseline.get('horizontalSplit'))
    elif rule=='saved-version': completed = any(id!='current' for id in state.get('versionIds',[]))
    elif rule=='versions': completed = len(state.get('versionIds',[]))>=2
    elif rule=='current': completed = not state.get('versionIds') and not state.get('versionWindows')
    elif rule=='download': completed = any(e.get('type')=='download' and e.get('format')==current['format'] for e in events)
    if current['id']=='displacement-route':completed=state.get('displacementPanelVisible',False)
    if current['id']=='comment-draw' and state.get('commentCount',0)>0:completed=True
    selection_ops={'station-select':'delete-node','segment-select-add':'add-station','segment-select-divide':'split-segment','segment-select-delete':'delete-segment'}
    if current['id'] in selection_ops and any(e.get('operation')==selection_ops[current['id']] for e in events):completed=True
    if current['id'] in ('merge-select','merge') and len(hotels)==1 and len(set(hotels[0].get('routeIds',[])) & {metro[k]['id'] for k in ('A','C') if k in metro})==2:
        completed = True
        current['hint'] = 'A/C already share this station. Continue; no duplicate merge is needed.'
    if current['id']=='city' and 'lyon' in str(state.get('city','')).lower() and len(metro)!=4:
        current['hint'] = 'Load a Lyon dataset whose metro routes are named A, B, C and D.'
    watches={
        'empty':['draftRoutes'],'route':['draftRoutes'],'routes':['visibleRoutes'],
        'style-name':['selected'],'appearance':[], 'endpoint-draft':[],
        'route-color':['style'],'route-width':['style'], 'field':[current.get('field')],
        'merge-selection':['selection'],'node-selection':['selection'],'segment-selection':['selection'],
        'operation':[],'undo':[],'drawing':['drawing'],'visit':['displacementOpen'],
        'parameter':['style'],'image':['style'],'no-image':['style'],
        'different-styles':['selections'],'divider':['split','horizontalSplit'],
        'saved-version':['versionIds'],'versions':['versionIds'],'download':[],
    }
    current['watch']=[k for k in watches.get(rule,[]) if k]
    if current['id']=='displacement-panel':current['watch']=['displacementOpen']
    if current['id']=='save':
        current['target']='body:not(:has(.saveVersionDialog[open])) [data-tutorial="save"], .saveVersionDialog[open] input:invalid, .saveVersionDialog[open] button[type="submit"]:not(:disabled)'
        current['singleTarget']=True
    if current['id']=='comment-save':
        current['target']='.commentModal textarea, .commentModal button[type="submit"]:not(:disabled)'
        current['singleTarget']=True
    if current['id']=='compare-saved':
        current['target']='.versionMenu tbody tr:not(:first-child) .versionCompareButton'
        current['singleTarget']=True
    # Guide the exact control, then its value field when expanded.
    parameters={
        'background':('Background','Color','Background','input'),
        'obstacles':('River','Color','Obstacles','input'),
        'route-style':('Route','Width','Route','input[type="number"]'),
        'station-style':('Normal station','Fill','Normal station','input'),
        'label-style':('Label','Color','Label','input'),
        'image':('Background','Image','Background','input[type="file"]'),
        'image-remove':('Background','Image','Background','button[data-tutorial="remove-image"]'),
    }
    if current['id'] in parameters:
        group,name,parent,control=parameters[current['id']]
        section=f'[data-tutorial-parameter="{group}:{name}"]'
        current['target']=f'{section}:not([open]) > summary, {section}[open] .exactValues {control}'
        current['fallback']=f'[data-tutorial-group="{parent}"] > .componentHeading button'
        current['singleTarget']=True
    if current['id']=='legend-width':
        current['target']='.legendAppearance input[type="number"]'
        if str(state.get('style',{}).get('overrides',{}).get(metro.get('A',{}).get('name'),{}).get('route-width'))=='8':current['target']='.legendAppearance button'
        current['singleTarget']=True
    if current['id']=='displacement-panel' and state.get('displacementPanelVisible'):
        current['target']='#displacement-bottom-panel .routeDifference'
    return {**current, 'index':index, 'total':len(STEPS), 'complete':bool(completed)}


@router.post('/tutorial/step', include_in_schema=False)
def tutorial_step(body: dict):
    return evaluate(body)


@router.post('/tutorial/reference', include_in_schema=False)
def reference_style(body: dict):
    style = deepcopy(body.get('style', {}))
    style.setdefault('values', {}).update({
        'background-color':'#ffffff','background-osm':'false','background-image-base64':'',
        'water-color':'#e3f6fc','park-color':'#f2f2f2','water-outline-width':'0','park-outline-width':'0',
        'obstacle-outline-width':'0','route-color':'source','route-width':'8','route-spacing':'1',
        'bend-angles':'90, 135','bend-90-offset-in':'24','bend-90-offset-out':'24',
        'bend-135-offset-in':'16','bend-135-offset-out':'16','bend-angle-tolerance':'15',
        'normal-shape':'circle','normal-radius':'2.5','normal-fill':'#ffffff','normal-outline-width':'0','normal-glyph':'none',
        'interchange-shape':'circle','interchange-radius':'2.5','interchange-fill':'#ffffff','interchange-outline-width':'0','interchange-glyph':'none','interchange-merge':'true',
        'label-color':'#222222','label-size':'8','label-font-family':'"Frutiger.ttf", sans-serif',
    })
    colors = dict(A='#df4086', B='#49a6dd', C='#f5ad28', D='#2eaa65')
    style['interchanges'] = {}
    style['pairs'] = {}
    style['overrides'] = {r['name']:{'route-color':colors[route_letter(r)],'route-width':'8'} for r in body.get('routes',[]) if route_letter(r)}
    return {'style':style}
