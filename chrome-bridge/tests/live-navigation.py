import json,pathlib,subprocess,sys,time,uuid
binary=pathlib.Path(sys.argv[1]);tab=int(sys.argv[2]);out=pathlib.Path(sys.argv[3]);out.mkdir(exist_ok=False);tag=uuid.uuid4().hex[:10];calls=[];child=None
def call(action,target=tab,**args):
    payload={'action':action,'tab_id':target,**args};start=time.monotonic()
    p=subprocess.run([str(binary),'--request'],input=json.dumps(payload),capture_output=True,text=True,encoding='utf-8',timeout=35,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    value=json.loads(p.stdout);value.pop('image',None);calls.append({'request':payload,'response':value,'elapsedMs':(time.monotonic()-start)*1000})
    return value
def observe(target=tab):
    for _ in range(5):
        value=call('observe',target,screenshot=False)
        if value.get('ok'):return value['data']
        if value.get('error',{}).get('code') not in ['PAGE_LOADING','NAVIGATION_CHANGED','ACTION_OUTCOME_UNKNOWN']:raise AssertionError(value)
        time.sleep(.1)
    raise AssertionError(value)
result={'ok':False}
try:
    initial=observe();origin=initial['url'].rsplit('/',1)[0];next_url=origin+'/next.html'
    created=call('open',observation_id=initial['observation_id'],request_id=tag+'-open',url=next_url,active=False);assert created['ok'],created;child=created['data']['tab_id']
    observed=observe(child);assert 'NAVIGATION_CONFIRMED' in observed['text']
    close_args={'observation_id':observed['observation_id'],'request_id':tag+'-close'}
    closed=call('close',child,**close_args);assert closed['ok'] and closed['data']['closed'];replay=call('close',child,**close_args);assert replay['ok'] and replay['replayed'];child=None
    current=observe();assert call('navigate',observation_id=current['observation_id'],request_id=tag+'-navigate',url=next_url)['ok']
    current=observe();assert current['url']==next_url
    assert call('reload',observation_id=current['observation_id'],request_id=tag+'-reload')['ok'];current=observe();assert 'NAVIGATION_CONFIRMED' in current['text']
    assert call('back',observation_id=current['observation_id'],request_id=tag+'-back')['ok'];current=observe();assert current['url']==initial['url']
    assert call('forward',observation_id=current['observation_id'],request_id=tag+'-forward')['ok'];current=observe();assert current['url']==next_url
    assert call('back',observation_id=current['observation_id'],request_id=tag+'-restore')['ok'];current=observe();assert current['url']==initial['url']
    result.update(ok=True,checks=['same-origin background tab','created tab observation','close receipt replay','navigation','reload','back','forward','original tab URL restored'])
except BaseException as error:result['error']=str(error)
finally:
    if child is not None:result['remainingOwnedTab']=child
    result['calls']=calls;(out/'result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in result.items() if k!='calls'}));raise SystemExit(0 if result['ok'] else 1)
