// Executed only in a Chrome isolated world. Page text is untrusted evidence.
export function pageTask(request) {
  const root=globalThis;
  if(!root.__devboxNodes) root.__devboxNodes=new Map();
  const visible=e=>{const r=e.getBoundingClientRect(),s=getComputedStyle(e);return e.isConnected&&r.width>0&&r.height>0&&s.visibility!=='hidden'&&s.display!=='none';};
  const rect=e=>{const r=e.getBoundingClientRect();return {x:r.x,y:r.y,width:r.width,height:r.height};};
  const target=()=>{const e=root.__devboxNodes.get(request.target_id)?.deref();if(!e||!visible(e))throw new Error('TARGET_STALE');return e;};
  const active=()=>{let e=document.activeElement;while(e?.shadowRoot?.activeElement)e=e.shadowRoot.activeElement;return e;};
  const containsComposed=(container,node)=>{for(let steps=0;node&&steps<256;++steps){if(node===container)return true;node=node.parentNode??node.host;}return false;};
  const accessibleLabel=e=>{
    const explicit=e.getAttribute('aria-label');if(explicit)return explicit.slice(0,240);
    const ids=(e.getAttribute('aria-labelledby')||'').trim().split(/\s+/).slice(0,8),scope=e.getRootNode();
    const labelled=ids.map(id=>scope.getElementById?.(id)?.textContent?.slice(0,240)||'').join(' ').replace(/\s+/g,' ').trim();if(labelled)return labelled.slice(0,240);
    return [...(e.labels||[])].slice(0,8).map(label=>label.textContent.slice(0,240)).join(' ').replace(/\s+/g,' ').trim().slice(0,240);
  };
  function htmlPrefix(){
    const maximum=2*1024*1024,encoder=new TextEncoder();let buffer=new Uint8Array(65536),used=0,truncated=false,nodes=0;
    const emit=text=>{
      let read=0;
      while(read<text.length){
        const part=encoder.encodeInto(text.slice(read),buffer.subarray(used));read+=part.read;used+=part.written;
        if(read===text.length)return true;
        if(buffer.length===maximum){truncated=true;return false;}
        const grown=new Uint8Array(Math.min(maximum,buffer.length*2));grown.set(buffer.subarray(0,used));buffer=grown;
      }return true;
    };
    const escaped=(text,attribute=false)=>{
      for(let start=0;start<text.length;){let end=Math.min(text.length,start+4096);if(end<text.length&&text.charCodeAt(end-1)>=0xd800&&text.charCodeAt(end-1)<=0xdbff)++end;
        let part=text.slice(start,end).replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('>','&gt;');if(attribute)part=part.replaceAll('"','&quot;');
        if(!emit(part))return false;start=end;
      }return true;
    };
    const voids=new Set(['area','base','br','col','embed','hr','img','input','link','meta','param','source','track','wbr']);
    const start=document.documentElement,stack=[];let node=start;
    capture:while(node){
      if(++nodes>20000||stack.length>256){truncated=true;break;}
      if(node.nodeType===1){
        const tag=node.localName,isVoid=node.namespaceURI==='http://www.w3.org/1999/xhtml'&&voids.has(tag);
        if(!emit('<'+tag))break;
        for(const attribute of node.attributes){if(!emit(' '+attribute.name+'="')||!escaped(attribute.value,true)||!emit('"'))break capture;}
        if(!emit('>'))break;
        const child=tag==='template'&&node.content?node.content.firstChild:node.firstChild;
        if(child&&!isVoid){stack.push(node);node=child;continue;}
        if(!isVoid&&!emit('</'+tag+'>'))break;
      }else if(node.nodeType===3){
        const raw=['script','style','xmp','iframe','noembed','noframes','plaintext'].includes(node.parentElement?.localName);
        if(!(raw?emit(node.data):escaped(node.data)))break;
      }else if(node.nodeType===8){if(!emit('<!--')||!emit(node.data)||!emit('-->'))break;}
      while(node&&!node.nextSibling){
        if(!stack.length){node=null;break;}
        node=stack.pop();if(!emit('</'+node.localName+'>'))break capture;
        if(node===start){node=null;break;}
      }
      if(node===start)break;if(node)node=node.nextSibling;
    }
    let encoded='';for(let offset=0;offset<used;offset+=32766)encoded+=btoa(String.fromCharCode(...buffer.subarray(offset,Math.min(used,offset+32766))));
    return {base64:encoded,metadata:{capture_kind:'bounded_dom_serialization',truncated,retained_bytes:used,document_bytes:truncated?null:used,nodes_visited:nodes}};
  }
  if(request.action==='inspect') {
    root.__devboxNodes.clear();let id=0,visited=0,truncated=false;const elements=[],frames=[],roots=[document];
    scan:for(let index=0;index<roots.length;++index){
      const walker=document.createTreeWalker(roots[index],NodeFilter.SHOW_ELEMENT);let e;
      while((e=walker.nextNode())){
        if(++visited>20000||elements.length>=180){truncated=true;break scan;}
        if(e.shadowRoot){if(roots.length<32)roots.push(e.shadowRoot);else truncated=true;}
        if(e.localName==='iframe'&&frames.length<16)frames.push({title:e.title.slice(0,240),src:e.src.slice(0,2048),rect:rect(e)});
        const overflow=e.clientHeight>0&&(e.scrollHeight>e.clientHeight+1||e.scrollWidth>e.clientWidth+1);
        const scrollable=overflow&&/auto|scroll/.test(getComputedStyle(e).overflowY+' '+getComputedStyle(e).overflowX);
        if(!e.matches('a,button,input,textarea,select,[role="button"],[role="link"],[contenteditable="true"],[tabindex],[draggable="true"],summary')&&!scrollable)continue;
        if(!visible(e))continue;
        const r=rect(e);if(r.y+r.height<0||r.y>innerHeight||r.x+r.width<0||r.x>innerWidth)continue;
        const key=String(++id);root.__devboxNodes.set(key,new WeakRef(e));
        elements.push({id:key,tag:e.localName,role:e.getAttribute('role'),label:(accessibleLabel(e)||(scrollable?(e.id||e.localName):e.innerText)||e.getAttribute('placeholder')||e.getAttribute('name')||'').slice(0,240),type:e.getAttribute('type'),disabled:e.matches(':disabled'),rect:r,href:e.localName==='a'?String(e.href?.baseVal??e.href??'').slice(0,2048):undefined,value:'value'in e&&!['password','file'].includes(e.type)?String(e.value).slice(0,2048):undefined,checked:'checked'in e?e.checked:undefined,focused:e===active(),scroll:scrollable?{x:e.scrollLeft,y:e.scrollTop,width:e.scrollWidth,height:e.scrollHeight,client_width:e.clientWidth,client_height:e.clientHeight}:undefined});
      }
    }
    const textWalker=document.createTreeWalker(document.body??document.documentElement,NodeFilter.SHOW_TEXT);let node,text='',textNodes=0;
    while((node=textWalker.nextNode())&&text.length<12000&&++textNodes<20000){const e=node.parentElement;if(!e||['script','style','template','noscript'].includes(e.localName)||!visible(e))continue;const normalized=node.data.replace(/\s+/g,' ').trim();if(!normalized)continue;const separator=text?'\n':'';text+=separator+normalized.slice(0,Math.max(0,12000-text.length-separator.length));}
    const html=htmlPrefix();
    return {url:location.href,title:document.title.slice(0,512),text,text_scope:'rendered_document_prefix',text_truncated:!!node,elements,elements_truncated:truncated,viewport:{width:innerWidth,height:innerHeight,deviceScaleFactor:devicePixelRatio},scroll:{x:scrollX,y:scrollY},frames,html_base64:html.base64,html:html.metadata};
  }
  if(request.action==='point') {
    if(request.target_id) {const e=target(),r=rect(e);const x=Math.max(0,Math.min(innerWidth-1,r.x+r.width/2)),y=Math.max(0,Math.min(innerHeight-1,r.y+r.height/2));let hit=document.elementFromPoint(x,y);while(hit?.shadowRoot){const child=hit.shadowRoot.elementFromPoint(x,y);if(!child||child===hit)break;hit=child;}const label=hit?.closest('label');if(!hit||!(containsComposed(e,hit)||label?.control===e))throw new Error('TARGET_OBSCURED');return {x,y};}
    if(!Number.isFinite(request.x)||!Number.isFinite(request.y)||request.x<0||request.y<0||request.x>=innerWidth||request.y>=innerHeight)throw new Error('POINT_OUTSIDE_VIEWPORT');return {x:request.x,y:request.y};
  }
  if(request.action==='focus') {const e=target();if(e.type==='password')throw new Error('PASSWORD_FIELD_REQUIRES_USER');e.focus();if(active()!==e)throw new Error('FOCUS_FAILED');if(request.replace){if(typeof e.select==='function')e.select();else if(e.isContentEditable){const range=document.createRange();range.selectNodeContents(e);const selection=getSelection();selection.removeAllRanges();selection.addRange(range);}else throw new Error('REPLACE_TARGET_UNSUPPORTED');}return {focused:true};}
  if(request.action==='editable') {const e=active();if(!e||['password','file','checkbox','radio','button','submit'].includes(e.type)||e.disabled||e.readOnly||!(e.isContentEditable||['INPUT','TEXTAREA'].includes(e.tagName)))throw new Error('EDITABLE_FOCUS_REQUIRED');return {editable:true};}
  if(request.action==='scroll_state') {
    let e=document.elementFromPoint(request.x,request.y);const nested=[];
    while(e&&nested.length<8){if(e.scrollHeight>e.clientHeight||e.scrollWidth>e.clientWidth)nested.push({tag:e.tagName,id:e.id,x:e.scrollLeft,y:e.scrollTop});e=e.parentElement;}
    return {page:{x:scrollX,y:scrollY},nested};
  }
  if(request.action==='select_text') {
    const e=target();if(e.type==='password')throw new Error('PASSWORD_FIELD_REQUIRES_USER');const source='value'in e?e.value:e.textContent;const i=source.indexOf(request.text);if(i<0||source.indexOf(request.text,i+1)>=0)throw new Error('TEXT_NOT_UNIQUE');
    if(typeof e.setSelectionRange==='function'){e.focus();e.setSelectionRange(i,i+request.text.length);return {selected:true};}
    if(e.isContentEditable){const walker=document.createTreeWalker(e,NodeFilter.SHOW_TEXT);let node,offset=0,start,end,visited=0;
      while((node=walker.nextNode())&&++visited<20000){const next=offset+node.length;if(!start&&i<next)start=[node,i-offset];if(start&&i+request.text.length<=next){end=[node,i+request.text.length-offset];break;}offset=next;}
      if(!start||!end)throw new Error('SELECTION_LIMIT');const range=document.createRange();range.setStart(...start);range.setEnd(...end);e.focus();const selection=getSelection();selection.removeAllRanges();selection.addRange(range);return {selected:true};}
    throw new Error('UNSUPPORTED_SELECTION_TARGET');
  }
  if(request.action==='select_option') {const e=target();if(e.tagName!=='SELECT'||![...e.options].some(o=>o.value===request.text&&!o.disabled))throw new Error('OPTION_NOT_AVAILABLE');e.value=request.text;e.dispatchEvent(new Event('input',{bubbles:true}));e.dispatchEvent(new Event('change',{bubbles:true}));return {selected:true};}
  throw new Error('UNSUPPORTED_PAGE_OPERATION');
}
