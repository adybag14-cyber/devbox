export const MAX_TEXT=16000, MAX_HTML=2*1024*1024, OBSERVATION_TTL_MS=30000;
export const READ_ACTIONS=new Set(['status','tabs','observe','screenshot','read_html']);
export const ACTIONS=new Set([...READ_ACTIONS,'open','navigate','back','forward','reload','focus','close','click','double_click','move','drag','scroll','type','key','select_text','select_option','wait']);
export function publicUrl(value) {
  const url=new URL(value);
  if(!['http:','https:'].includes(url.protocol)||url.username||url.password) throw new Error('URL_SCHEME_DENIED: use an ordinary HTTP(S) page');
  return url;
}
export function validate(request) {
  if(!request || typeof request!=='object' || Array.isArray(request) || !ACTIONS.has(request.action)) throw new Error('INVALID_REQUEST');
  const allowed=new Set(['action','id','_bridge_deadline_unix_ms','tab_id','observation_id','request_id','target_id','url','text','key','x','y','to_x','to_y','delta_x','delta_y','button','replace','screenshot','active','offset','max_bytes','wait_ms']);
  if(Object.keys(request).some(key=>!allowed.has(key)))throw new Error('UNKNOWN_ARGUMENT');
  for(const key of ['replace','screenshot','active'])if(request[key]!==undefined&&typeof request[key]!=='boolean')throw new Error('INVALID_BOOLEAN');
  for(const [key,maximum]of [['observation_id',128],['request_id',64],['target_id',64],['url',8192],['key',64]])if(request[key]!==undefined&&(typeof request[key]!=='string'||request[key].length>maximum))throw new Error('INVALID_STRING');
  for(const key of ['x','y','to_x','to_y','delta_x','delta_y']) if(request[key]!==undefined && (!Number.isFinite(request[key])||Math.abs(request[key])>100000)) throw new Error('INVALID_COORDINATE');
  if(request.text!==undefined && (typeof request.text!=='string'||request.text.length>MAX_TEXT)) throw new Error('TEXT_LIMIT');
  if(request.url!==undefined) publicUrl(request.url);
  if(['open','navigate'].includes(request.action)&&typeof request.url!=='string')throw new Error('URL_REQUIRED');
  if(['type','select_text','select_option'].includes(request.action)&&typeof request.text!=='string')throw new Error('TEXT_REQUIRED');
  if(request.action==='select_text'&&!request.text)throw new Error('SELECTION_TEXT_REQUIRED');
  if(request.action==='key'&&typeof request.key!=='string')throw new Error('KEY_REQUIRED');
  if(!READ_ACTIONS.has(request.action) && (!/^[a-zA-Z0-9_-]{1,64}$/.test(request.request_id??''))) throw new Error('REQUEST_ID_REQUIRED');
}
export function authorize(grant,tab,request,observation,now=Date.now()) {
  if(!grant || now>=grant.expires) throw new Error('TAB_NOT_GRANTED: grant this tab in the extension popup');
  if(publicUrl(tab.url).origin!==grant.origin) throw new Error('ORIGIN_CHANGED: grant the new origin explicitly');
  if(request.url && publicUrl(request.url).origin!==grant.origin) throw new Error('ORIGIN_NOT_GRANTED');
  if(request.action==='open'&&!grant.allow_new_tabs)throw new Error('NEW_TAB_PERMISSION_REQUIRED');
  if(!READ_ACTIONS.has(request.action) && (!observation || request.observation_id!==observation.id || now-observation.at>OBSERVATION_TTL_MS || observation.url!==tab.url)) throw new Error('OBSERVATION_STALE: observe again before input');
}
export function boundedInteger(value,fallback,min,max) {if(value===undefined)return fallback;if(!Number.isSafeInteger(value)||value<min||value>max)throw new Error('INTEGER_RANGE');return value;}
