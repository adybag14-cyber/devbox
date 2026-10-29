"""Bounded protocol checks; no input or navigation is performed."""
import json,pathlib,subprocess,sys,time
binary=pathlib.Path(sys.argv[1]).resolve(strict=True)
messages=[{'jsonrpc':'2.0','id':1,'method':'initialize','params':{'protocolVersion':'2025-11-25','capabilities':{},'clientInfo':{'name':'bridge-smoke','version':'1'}}},
 {'jsonrpc':'2.0','method':'notifications/initialized'},
 {'jsonrpc':'2.0','id':2,'method':'tools/list'},
 {'jsonrpc':'2.0','id':3,'method':'tools/call','params':{'name':'chrome_bridge','arguments':{'action':'not_allowed'}}},
 {'jsonrpc':'2.0','id':4,'method':'tools/call','params':{'name':'native_chrome','arguments':{'action':'click','window_id':1,'observation_id':'stale','x':0,'y':0}}}]
started=time.monotonic()
result=subprocess.run([str(binary),'--mcp'],input=''.join(json.dumps(m)+'\n' for m in messages),capture_output=True,text=True,timeout=15,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
assert result.returncode==0,result.stderr
rows=[json.loads(line) for line in result.stdout.splitlines()]
assert [r['id'] for r in rows]==[1,2,3,4]
assert rows[0]['result']['serverInfo']['name']=='devbox-chrome-bridge'
assert [t['name'] for t in rows[1]['result']['tools']]==['chrome_bridge','native_chrome']
assert rows[2]['result']['isError'] and rows[2]['result']['structuredContent']['error']['code']=='INVALID_ARGUMENTS'
assert rows[3]['result']['isError'] and rows[3]['result']['structuredContent']['input_sent']==False
print(json.dumps(dict(ok=True,elapsedSeconds=time.monotonic()-started,checks=['initialize','notifications_have_no_response','tool_discovery','unsupported_browser_action_rejected','native_input_without_observation_rejected'],nativeInputSent=False)))
