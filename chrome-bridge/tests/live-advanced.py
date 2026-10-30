"""Real Chrome checks of rich text, shadow DOM, drag, double-click and HTML bounds."""
import argparse,base64,hashlib,json,pathlib,subprocess,time,uuid
p=argparse.ArgumentParser();p.add_argument('binary');p.add_argument('tab_id',type=int);p.add_argument('output');a=p.parse_args()
binary=pathlib.Path(a.binary).resolve(strict=True);out=pathlib.Path(a.output);out.mkdir(exist_ok=False)
run=uuid.uuid4().hex[:12];calls=[];checks=[]
def call(action,**args):
    request={'action':action,'tab_id':a.tab_id,**args};started=time.perf_counter()
    process=subprocess.run([str(binary),'--request'],input=json.dumps(request),capture_output=True,text=True,encoding='utf-8',timeout=35,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    result=json.loads(process.stdout);image=result.pop('image',None);number=len(calls)+1
    if image:(out/f'{number:02d}-{action}.jpg').write_bytes(base64.b64decode(image['base64']))
    elapsed=(time.perf_counter()-started)*1000
    (out/f'{number:02d}-{action}.json').write_text(json.dumps({'request':request,'response':result,'elapsedMs':elapsed},indent=2),encoding='utf-8')
    calls.append({'action':action,'ok':result.get('ok'),'elapsedMs':elapsed})
    if args.get('request_id') in [run+'-blocked',run+'-linked-label']:
        assert not result.get('ok') and result['error']['code']=='TARGET_OBSCURED' and not result['input_sent'];return result
    assert result.get('ok'),result
    return result['data']
def element(data,label):return next(e for e in data['elements'] if e['label']==label)
def mutate(action,data,**args):return call(action,observation_id=data['observation_id'],request_id=run+'-'+str(len(calls)),screenshot=False,**args)
try:
    data=call('observe',screenshot=False);original=data['url'];assert original.startswith('http://127.0.0.1:')
    url=original.rsplit('/',1)[0]+'/advanced.html'
    mutate('navigate',data,url=url);data=call('observe',screenshot=False);assert data['title']=='Devbox Bridge Advanced Qualification'
    element(data,'Referenced control name');element(data,'Title-only action');checks.append('accessible name precedence and title fallback')
    data=mutate('select_text',data,target_id=element(data,'Rich editor')['id'],text='beta')
    data=mutate('type',data,text='BETA Δ');assert 'BETA Δ' in data['text'];checks.append('rich text selection and Unicode replacement')
    data=mutate('type',data,target_id=element(data,'Shadow editor')['id'],text='Shadow 🌍',replace=True)
    assert element(data,'Shadow editor')['value']=='Shadow 🌍';checks.append('open shadow root input')
    data=mutate('click',data,target_id=element(data,'Shadow host')['id']);assert 'SHADOW_HOST: 1' in data['text'];checks.append('shadow host hit testing')
    data=mutate('type',data,target_id=element(data,'Keyboard field')['id'],text='',replace=True)
    data=mutate('key',data,key='a');data=mutate('key',data,key='Shift+b');assert element(data,'Keyboard field')['value']=='aB'
    data=mutate('key',data,key='Ctrl+A');data=mutate('type',data,text='.-()');assert element(data,'Keyboard field')['value']=='.-()'
    data=mutate('key',data,key='Enter');assert 'SUBMITS: 1' in data['text'];checks.append('printable keys shortcuts punctuation typing and Enter submission')
    data=mutate('click',data,target_id=element(data,'Label covered radio')['id']);assert element(data,'Label covered radio')['checked'];checks.append('associated label can activate its covered radio')
    call('click',observation_id=data['observation_id'],target_id=element(data,'Actually blocked button')['id'],request_id=run+'-blocked',screenshot=False)
    data=call('observe',screenshot=False);checks.append('unrelated overlay rejects input')
    call('click',observation_id=data['observation_id'],target_id=element(data,'Radio behind linked label')['id'],request_id=run+'-linked-label',screenshot=False)
    data=call('observe',screenshot=False);assert not element(data,'Radio behind linked label')['checked'] and '#' not in data['url'];checks.append('interactive label descendant cannot substitute a link for a control')
    data=mutate('double_click',data,target_id=element(data,'Double click test')['id']);assert 'DOUBLE: 1' in data['text'];checks.append('double click once')
    target=element(data,'Drop target')['rect']
    data=mutate('drag',data,target_id=element(data,'Drag source')['id'],to_x=target['x']+target['width']/2,to_y=target['y']+target['height']/2)
    data=call('observe');assert 'DROP_CONFIRMED' in data['text'];checks.append('HTML5 drag and drop')
    data=mutate('click',data,target_id=element(data,'Create large DOM')['id']);assert data['html']['truncated'];assert data['html']['retained_bytes']==2*1024*1024;checks.append('large DOM truncation at 2 MiB')
    html=call('read_html',observation_id=data['observation_id'],offset=2*1024*1024-100,max_bytes=200)
    assert html['bytes']==100 and html['next_offset'] is None and len(base64.b64decode(html['content_base64']))==100;checks.append('exact final HTML byte range')
    mutate('navigate',data,url=original);data=call('observe',screenshot=False);assert data['url']==original
    result={'ok':True,'run':run,'binarySha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'calls':calls,'checks':checks,'restoredUrl':original}
except BaseException as error:result={'ok':False,'run':run,'error':str(error),'calls':calls,'checks':checks}
(out/'result.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(result));raise SystemExit(0 if result['ok'] else 1)
