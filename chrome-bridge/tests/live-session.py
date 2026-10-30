"""Optional development client for the Rust MCP server; never performs GUI input itself."""
import argparse,datetime,hashlib,json,pathlib,queue,subprocess,threading,time
p=argparse.ArgumentParser();p.add_argument('binary');p.add_argument('session');p.add_argument('control');args=p.parse_args()
binary=pathlib.Path(args.binary).resolve(strict=True);folder=pathlib.Path(args.session).resolve();control_path=pathlib.Path(args.control).resolve(strict=True)
folder.mkdir(exist_ok=False)
digest=hashlib.sha256(binary.read_bytes()).hexdigest()
def control():
    try:return json.loads(control_path.read_text(encoding='utf-8-sig'))
    except (OSError,ValueError):return {'stop':True,'allowNewWork':False}
initial=control();assert initial['allowNewWork'] and not initial['stop']
deadline=datetime.datetime.fromisoformat(initial['deadlineUtc'].replace('Z','+00:00')).timestamp()-60
def admitted():
    c=control();return c.get('allowNewWork') and not c.get('stop') and time.time()<deadline and not (folder/'stop').exists()
def save(path,value):
    tmp=path.with_suffix('.tmp');tmp.write_text(json.dumps(value),encoding='utf-8');tmp.replace(path)
responses=queue.Queue(maxsize=4);seen=set();failure=None
with (folder/'stderr.log').open('x',encoding='utf-8') as stderr:
    process=subprocess.Popen([str(binary),'--mcp'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=stderr,text=True,encoding='utf-8',creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0),bufsize=1)
    def read():
        while True:
            line=process.stdout.readline(12*1024*1024+1)
            if not line:return
            if len(line)>12*1024*1024:responses.put({'readerError':'response limit'});return
            try:responses.put(json.loads(line))
            except ValueError:responses.put({'readerError':'invalid JSON'});return
    threading.Thread(target=read,daemon=True).start()
    def send(value):
        assert admitted(),'test admission closed'
        process.stdin.write(json.dumps(value)+'\n');process.stdin.flush();expires=time.monotonic()+40
        while time.monotonic()<expires:
            if not admitted():raise RuntimeError('test admission closed')
            try:response=responses.get(timeout=.15)
            except queue.Empty:continue
            assert response.get('id')==value.get('id'),response.keys()
            return response
        raise RuntimeError('response deadline; do not replay uncertain input')
    try:
        send({'jsonrpc':'2.0','id':0,'method':'initialize','params':{'protocolVersion':'2025-11-25','capabilities':{},'clientInfo':{'name':'owned-live-qualification','version':'2'}}})
        save(folder/'ready.json',{'ready':True,'pid':process.pid,'binary':str(binary),'sha256':digest,'deadlineUtc':datetime.datetime.fromtimestamp(deadline,datetime.timezone.utc).isoformat()})
        while admitted():
            for request in sorted(folder.glob('request-*.json')):
                if request.name in seen:continue
                assert request.stat().st_size<=256*1024,'request limit'
                value=json.loads(request.read_text(encoding='utf-8-sig'));seen.add(request.name)
                response=send(value);save(folder/request.name.replace('request-','response-'),response)
            time.sleep(.1)
    except BaseException as e:
        failure=str(e)
    finally:
        process.stdin.close()
        try:process.wait(timeout=5)
        except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=5)
        save(folder/'completed.json',{'exitCode':process.returncode,'requests':len(seen),'error':failure,'binaryUnchanged':hashlib.sha256(binary.read_bytes()).hexdigest()==digest,'finishedAtUtc':datetime.datetime.now(datetime.timezone.utc).isoformat()})
