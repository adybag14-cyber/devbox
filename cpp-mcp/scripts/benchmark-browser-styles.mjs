// Development-only comparison of the exact C++ embedded observation scripts.
// Uses an owned headless Chrome profile, without extensions or signed-in state.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {spawnSync} from 'node:child_process';
import {existsSync, lstatSync, mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {basename, dirname, join, resolve} from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';

const repo=resolve(dirname(fileURLToPath(import.meta.url)),'../..');
const revision=process.argv[2];
assert(revision&&/^[0-9a-f]{40}$/.test(revision),'Pass the full baseline commit');
const old=spawnSync('git',['show',`${revision}:cpp-mcp/src/browser.cpp`],{cwd:repo,encoding:'utf8',timeout:10000,maxBuffer:4*1024*1024,windowsHide:true});
assert.ifError(old.error);assert.equal(old.status,0,old.stderr);
const extract=source=>{const script=source.match(/constexpr auto observe_script = R"JS\(([\s\S]*?)\)JS";/)?.[1];assert(script,'Exact native observation source missing');return script;};
const baseline=extract(old.stdout),candidate=extract(readFileSync(join(repo,'cpp-mcp/src/browser.cpp'),'utf8'));
const chrome=[process.env.DEVBOX_TEST_CHROME,process.env.ProgramFiles&&join(process.env.ProgramFiles,'Google/Chrome/Application/chrome.exe'),'/usr/bin/google-chrome','/usr/bin/chromium'].find(p=>p&&existsSync(p));
assert(chrome,'Set DEVBOX_TEST_CHROME to an existing Chrome executable');
const parent=resolve(tmpdir()),directory=mkdtempSync(join(parent,'devbox-style-benchmark-'));
const fixture=String.raw`
  const sources=${JSON.stringify({baseline,candidate})};
  const evaluate=Object.fromEntries(Object.entries(sources).map(([name,source])=>[name,new Function('return '+source)]));
  const originalStyle=window.getComputedStyle.bind(window);
  // Hit-point metadata is intentionally new. Compare every existing value;
  // dedicated compiled fixtures verify the new coordinates and probe limits.
  const comparable=result=>{const {controls_note,interaction_points_truncated,...value}=result;return JSON.stringify({...value,controls:value.controls.map(({interaction_point,...control})=>control)});};
  const inspect=name=>{let styleCalls=0;const previous=window.getComputedStyle;
    window.getComputedStyle=(...args)=>{++styleCalls;return originalStyle(...args);};
    try{return {result:evaluate[name](),calls:styleCalls};}finally{window.getComputedStyle=previous;}
  };
  const records=[];
  function page(kind){
    for(const modal of document.querySelectorAll(':modal'))modal.close();
    document.body.replaceChildren();
    document.body.style.cssText='margin:0;font:12px sans-serif';
    if(kind==='small-form'){
      document.body.innerHTML='<h1>Product selection</h1><p>Choose the required size and colour.</p>'+Array.from({length:12},(_,i)=>'<button>Variant '+i+'</button>').join('');
    }else if(kind==='catalogue'){
      const list=document.createElement('div');list.style.cssText='width:900px';
      list.innerHTML=Array.from({length:2500},(_,i)=>'<div><span>Phone '+i+'</span><button>Select '+i+'</button></div>').join('');document.body.append(list);
    }else if(kind==='deep-visible-text'){
      let root=document.body;
      for(let i=0;i<64;i++){const node=document.createElement('div');root.append(node);root=node;}
      root.style.cssText='font:1px/1px monospace;width:800px';
      for(let i=0;i<1000;i++){const span=document.createElement('span');span.textContent='x ';root.append(span);}
    }else if(kind==='native-modal'){
      document.body.textContent='Background catalogue '.repeat(1000);
      const modal=document.createElement('dialog');modal.style.cssText='width:500px;height:250px';
      modal.innerHTML='<h2>Details</h2><p>Review the selected variant.</p><button>Close</button><div style="height:100px;overflow:auto"><div style="height:1200px">Specifications</div></div>';document.body.append(modal);modal.showModal();
    }else if(kind==='transparent-ancestor'){
      document.body.innerHTML='<div style="opacity:0"><p>Unpainted text</p><button>Unpainted control</button></div><p>Visible product details</p>';
    }
    document.body.getBoundingClientRect();
  }
  for(const kind of ['small-form','catalogue','deep-visible-text','native-modal','transparent-ancestor']){
    page(kind);
    const a=inspect('baseline'),b=inspect('candidate');
    if(comparable(a.result)!==comparable(b.result))throw new Error('Observation differs: '+kind);
    // Timed calls use the original browser API, without the counting wrapper.
    // Warm both functions on the same layout, then interleave ABBA blocks.
    for(let i=0;i<4;i++){evaluate.baseline();evaluate.candidate();}
    const trials=[],iterations=kind==='catalogue'||kind==='deep-visible-text'?4:64;
    for(let block=0;block<8;block++)for(const name of ['baseline','candidate','candidate','baseline']){
      const start=performance.now();for(let i=0;i<iterations;i++)evaluate[name]();
      trials.push({name,elapsed_ms:performance.now()-start,iterations});
    }
    records.push({fixture:kind,baseline_style_calls:a.calls,candidate_style_calls:b.calls,result:a.result,trials});
  }
  // Each call must observe changes to ancestor opacity, layout and control state.
  document.body.innerHTML='<div id="changing"><p>Changing text</p><button>Change</button></div>';
  const changes=[];
  for(const state of ['opacity:1','opacity:0','opacity:1;display:none','opacity:1;display:block']){
    document.querySelector('#changing').style.cssText=state;
    const a=inspect('baseline'),b=inspect('candidate');
    if(comparable(a.result)!==comparable(b.result))throw new Error('Fresh style differs: '+state);
    changes.push({state,viewport_text:b.result.viewport_text,controls:b.result.controls.length});
  }
  const depthChecks=[];
  for(const count of [252,253,254,255,256,257,258]){
    document.body.replaceChildren();let node=document.body,shorter;
    for(let i=0;i<count;i++){const child=document.createElement('div');node.append(child);node=child;if(i===250)shorter=node;}
    const deep=document.createElement('span');deep.textContent='Deep leaf';node.append(deep);
    // DOM order first tests the deepest leaf, then a shorter suffix of that path.
    const short=document.createElement('span');short.textContent='Shorter leaf';shorter.append(short);
    const a=inspect('baseline'),b=inspect('candidate');
    if(comparable(a.result)!==comparable(b.result))throw new Error('Depth-bound output differs: '+count);
    depthChecks.push({ancestors:count,viewport_text:b.result.viewport_text});
  }
  document.body.replaceChildren();
  // More than 8,192 distinct opacity ancestors exercises the cache's fallback.
  for(let i=0;i<1500;i++){
    let node=document.body;
    for(let j=0;j<6;j++){const child=document.createElement('span');node.append(child);node=child;}
    node.textContent='x ';
  }
  document.body.style.font='1px/1px monospace';
  const boundedA=inspect('baseline'),boundedB=inspect('candidate');
  if(comparable(boundedA.result)!==comparable(boundedB.result))throw new Error('Cache-bound output differs');
  const bounded={baseline_style_calls:boundedA.calls,candidate_style_calls:boundedB.calls,viewport_chars:boundedB.result.viewport_text.length};
  return {user_agent:navigator.userAgent,records,changes,depth_checks:depthChecks,cache_bound:bounded};
`;
const run=`let value;try{value=(()=>{${fixture}})();}catch(e){value={error:String(e.stack||e)};}const out=document.createElement('pre');out.id='result';out.textContent=btoa(unescape(encodeURIComponent(JSON.stringify(value))));document.body.replaceChildren(out);`;
try{
  const html=join(directory,'fixture.html');writeFileSync(html,'<!doctype html><meta charset="utf-8"><body><script>'+run.replaceAll('</script','<\\/script')+'</script>');
  const result=spawnSync(chrome,['--headless=new','--no-first-run','--no-default-browser-check','--disable-extensions','--disable-sync','--disable-background-networking','--disable-component-update','--window-size=1000,800',`--user-data-dir=${join(directory,'profile')}`,'--dump-dom',pathToFileURL(html).href],{encoding:'utf8',timeout:90000,maxBuffer:4*1024*1024,windowsHide:true});
  assert.ifError(result.error);assert.equal(result.status,0,result.stderr?.slice(-2000));
  const encoded=result.stdout.match(/<pre id="result">([A-Za-z0-9+/=]+)<\/pre>/)?.[1];assert(encoded,'Browser result missing');
  const output=JSON.parse(Buffer.from(encoded,'base64').toString('utf8'));assert.equal(output.error,undefined,output.error);
  const hash=value=>createHash('sha256').update(value).digest('hex');
  const median=values=>{const v=[...values].sort((a,b)=>a-b);return(v[(v.length-1)>>1]+v[v.length>>1])/2;};
  for(const record of output.records){
    // File URLs contain the private temporary path, so exclude only that field.
    delete record.result.url;
    record.observation_sha256=hash(JSON.stringify(record.result));delete record.result;
    for(const name of ['baseline','candidate'])record[name+'_median_ms']=median(record.trials.filter(t=>t.name===name).map(t=>t.elapsed_ms/t.iterations));
  }
  assert.equal(output.changes[0].viewport_text,output.changes[3].viewport_text);
  assert.equal(output.changes[1].viewport_text,'');assert.equal(output.changes[2].viewport_text,'');
  console.log(JSON.stringify({baseline_revision:revision,baseline_script_sha256:hash(baseline),candidate_script_sha256:hash(candidate),comparison_excludes:['controls_note','interaction_points_truncated','controls[].interaction_point'],scope:'Embedded native observation only; excludes CDP, image capture, network and model time',...output},null,2));
}finally{
  assert.equal(dirname(resolve(directory)),parent);assert(basename(directory).startsWith('devbox-style-benchmark-'));assert(!lstatSync(directory).isSymbolicLink());
  rmSync(directory,{recursive:true,force:true,maxRetries:5,retryDelay:100});
}
