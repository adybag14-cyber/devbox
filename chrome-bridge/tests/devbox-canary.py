"""Run an isolated C++ MCP frontend and exercise its pinned Rust bridge adapter."""
import argparse,base64,hashlib,json,os,pathlib,socket,subprocess,time,urllib.request,urllib.error
p=argparse.ArgumentParser();p.add_argument('devbox');p.add_argument('bridge');p.add_argument('output');p.add_argument('--tab',type=int);a=p.parse_args()
binary=pathlib.Path(a.devbox).resolve(strict=True);bridge=pathlib.Path(a.bridge).resolve(strict=True);root=pathlib.Path(a.output).resolve();root.mkdir(exist_ok=False)
workspace=root/'workspace';workspace.mkdir();digest=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
bridge_hash=digest(bridge);binary_hash=digest(binary)
with socket.socket() as reserve:reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
allowed={'PATH','SYSTEMROOT','WINDIR','SYSTEMDRIVE','TEMP','TMP','USERPROFILE','LOCALAPPDATA','APPDATA','PATHEXT','COMSPEC'}
env={k:v for k,v in os.environ.items() if k.upper() in allowed}
env.update(DEVBOX_PROJECT_ROOT=str(root),HOST='127.0.0.1',PORT=str(port),MCP_AUTH_MODE='none',PUBLIC_BASE_URL='',DEVBOX_RUNTIME_MODE='host',ENABLE_HOST_EXEC='true',DEVBOX_AUTO_START='false',ENABLE_GATEWAY_BRIDGE='false',HOST_WORKSPACE_PATH=str(workspace),DEVBOX_WORKSPACE_PATH=str(workspace),HOST_DEFAULT_WORKDIR=str(workspace),MCP_JOBS_ROOT=str(root/'jobs'),MCP_STATE_ROOT=str(root/'run/state'),MCP_EXEC_SLOT_ROOT=str(root/'slots'),MCP_STATE_BACKEND='legacy',MCP_PERFORMANCE_STATE_PATH=str(root/'run/performance.json'),DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE='1',DEVBOX_CHROME_BRIDGE_EXE=str(bridge),DEVBOX_CHROME_BRIDGE_SHA256=bridge_hash)
base=f'http://127.0.0.1:{port}';opener=urllib.request.build_opener(urllib.request.ProxyHandler({}));calls=[];record={'ok':False,'binarySha256':binary_hash,'bridgeSha256':bridge_hash,'productionRequests':0,'root':str(root)}
def request(path,body=None):
 req=urllib.request.Request(base+path,data=None if body is None else json.dumps(body).encode(),headers={'Content-Type':'application/json','Accept':'application/json'})
 with opener.open(req,timeout=40) as response:return json.load(response)
def call(args):
 start=time.perf_counter();value=request('/mcp',{'jsonrpc':'2.0','id':len(calls)+1,'method':'tools/call','params':{'name':'devbox_chrome_bridge','arguments':args}})
 result=value['result'];calls.append({'action':args['action'],'elapsedMs':(time.perf_counter()-start)*1000,'isError':result.get('isError')})
 (root/f'call-{len(calls)}.json').write_text(json.dumps(value),encoding='utf-8')
 assert not result.get('isError'),result
 for item in result.get('content',[]):
  if item['type']=='image':(root/f'call-{len(calls)}.jpg').write_bytes(base64.b64decode(item['data']))
 return result['structuredContent']['data']
with (root/'server.log').open('w',encoding='utf-8') as log:
 process=subprocess.Popen([str(binary)],cwd=root,env=env,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
 record['pid']=process.pid
 try:
  deadline=time.monotonic()+20
  while time.monotonic()<deadline:
   assert process.poll() is None,'candidate exited during startup'
   try:
    metadata=request('/');assert metadata['build']['binarySha256']==binary_hash;break
   except (urllib.error.URLError,ConnectionError):time.sleep(.1)
  else:raise RuntimeError('candidate readiness deadline')
  record['build']=metadata['build']
  status=call({'action':'status'});assert status['ok'] and status['data']['connected'] and status['data']['bridge_protocol']==2
  tabs=call({'action':'tabs'});assert tabs['ok']
  if a.tab:
   assert a.tab in [t['tab_id'] for t in tabs['data']['tabs']]
   observed=call({'action':'observe','tab_id':a.tab});assert observed['ok'] and observed['data']['url'].startswith('http://127.0.0.1:')
   html=call({'action':'read_html','tab_id':a.tab,'observation_id':observed['data']['observation_id'],'max_bytes':800});assert html['ok'] and html['data']['bytes']==800
   record['imageReturned']=True;record['htmlBytes']=800;record['retainedHtmlSha256']=html['data']['retained_sha256']
  record['ok']=True
 except BaseException as error:record['error']=str(error)
 finally:
  if process.poll() is None:process.terminate()
  process.wait(timeout=10);record['frontendStopped']=True;record['exitCode']=process.returncode
record['calls']=calls;record['binaryUnchanged']=digest(binary)==binary_hash and digest(bridge)==bridge_hash
(root/'result.json').write_text(json.dumps(record,indent=2),encoding='utf-8');print(json.dumps(record));raise SystemExit(0 if record['ok'] else 1)
