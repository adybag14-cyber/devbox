"""Exercise the actual Rust/extension bridge on the explicitly granted local fixture."""
import argparse,base64,hashlib,json,pathlib,re,subprocess,time,uuid
p=argparse.ArgumentParser();p.add_argument('binary');p.add_argument('tab_id',type=int);p.add_argument('output');a=p.parse_args()
binary=pathlib.Path(a.binary).resolve(strict=True);out=pathlib.Path(a.output);out.mkdir(exist_ok=False)
run=uuid.uuid4().hex[:12];calls=[]
def call(action,**args):
    payload={'action':action,'tab_id':a.tab_id,**args};started=time.perf_counter()
    process=subprocess.run([str(binary),'--request'],input=json.dumps(payload),capture_output=True,text=True,encoding='utf-8',timeout=35,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    result=json.loads(process.stdout);elapsed=(time.perf_counter()-started)*1000
    image=result.pop('image',None)
    number=len(calls)+1
    if image:(out/f'{number:02d}-{action}.jpg').write_bytes(base64.b64decode(image['base64']))
    (out/f'{number:02d}-{action}.json').write_text(json.dumps({'request':payload,'response':result,'elapsedMs':elapsed},indent=2),encoding='utf-8')
    calls.append({'action':action,'ok':result.get('ok'),'elapsedMs':elapsed,'bridgeMs':result.get('elapsed_ms')})
    return result
def good(result):assert result.get('ok'),result;return result['data']
def element(data,label):return next(e for e in data['elements'] if e['label']==label)
def count(data):return int(re.search(r'Counter:\s*(\d+)',data['text'])[1])
try:
    current=good(call('observe'));assert current['title']=='Devbox Bridge Qualification'
    assert current['url'].startswith('http://127.0.0.1:');initial=count(current)
    html=good(call('read_html',observation_id=current['observation_id'],offset=0,max_bytes=800))
    assert html['bytes']==800 and len(base64.b64decode(html['content_base64']))==800
    assert html['retained_sha256']==current['html']['retained_sha256']
    click={'observation_id':current['observation_id'],'target_id':element(current,'Increment counter')['id'],'request_id':run+'-click','screenshot':False}
    current=good(call('click',**click));assert count(current)==initial+1
    replay=call('click',**click);assert replay['ok'] and replay['replayed']
    conflict=call('click',**{**click,'x':0});assert not conflict['ok'] and conflict['error']['code']=='REQUEST_ID_CONFLICT'
    current=good(call('observe',screenshot=False));assert count(current)==initial+1
    stale=call('click',**{**click,'request_id':run+'-stale'});assert not stale['ok'] and stale['error']['code']=='OBSERVATION_STALE' and not stale['input_sent']
    typed='Bridge verified — Δ 🌍'
    current=good(call('type',observation_id=current['observation_id'],target_id=element(current,'Qualification text')['id'],text=typed,replace=True,request_id=run+'-type',screenshot=False))
    assert element(current,'Qualification text')['value']==typed
    current=good(call('select_option',observation_id=current['observation_id'],target_id=element(current,'Phone variant')['id'],text='512',request_id=run+'-variant',screenshot=False))
    assert element(current,'Phone variant')['value']=='512'
    current=good(call('scroll',observation_id=current['observation_id'],x=250,y=400,delta_y=500,request_id=run+'-nested',screenshot=True))
    assert current['movement']['movement_observed']
    assert current['scroll']['y']==0
    panel=next(e for e in current['movement']['after']['nested'] if e['id']=='panel');assert panel['y']>0
    verified=good(call('observe'));assert verified['scroll']['y']==0 and count(verified)==initial+1
    denied=call('navigate',observation_id=verified['observation_id'],request_id=run+'-denied',url='https://example.com/')
    assert not denied['ok'] and denied['error']['code']=='ORIGIN_NOT_GRANTED' and not denied['input_sent']
    current=good(call('observe',screenshot=False));assert current['url']==verified['url']
    result={'ok':True,'run':run,'binarySha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'calls':calls,'checks':['DOM and screenshot','bounded HTML bytes/hash','exactly-once click receipt','conflicting receipt rejection','stale observation rejection','Unicode input','select option','nested scroll without document movement','independent observation','cross-origin navigation rejected before input'],'initialCounter':initial,'finalCounter':count(current),'nativeInputUsed':False}
except BaseException as error:
    result={'ok':False,'run':run,'error':str(error),'calls':calls}
(out/'result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print(json.dumps(result));raise SystemExit(0 if result['ok'] else 1)
