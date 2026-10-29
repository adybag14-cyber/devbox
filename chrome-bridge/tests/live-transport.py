import hashlib,json,pathlib,statistics,subprocess,sys,time
binary=pathlib.Path(sys.argv[1]).resolve(strict=True);out=pathlib.Path(sys.argv[2])
times=[]
for i in range(50):
 start=time.monotonic();r=subprocess.run([str(binary),'--request'],input='{"action":"status"}',capture_output=True,text=True,timeout=8,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
 value=json.loads(r.stdout);assert r.returncode==0 and value['ok'] and value['data']['connected'],(i,r.returncode,value)
 assert value['data']['version']=='0.1.1'
 times.append((time.monotonic()-start)*1000)
record={'ok':True,'requests':50,'exeSha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'version':'0.1.1','latencyMs':{'median':statistics.median(times),'min':min(times),'max':max(times)},'scope':'Actual Chrome extension plus Rust native host, new CLI process for every request; no navigation/input','allMs':times}
out.write_text(json.dumps(record,indent=2),encoding='utf-8');print(json.dumps({k:v for k,v in record.items() if k!='allMs'}))
