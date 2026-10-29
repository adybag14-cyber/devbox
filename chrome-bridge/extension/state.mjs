export const LIMITS = Object.freeze({grants:16, observations:8, observationBytes:8*1024*1024,
  receipts:64, receiptBytes:2*1024*1024, queued:8, actionMs:20000});

// These are conservative retained-payload budgets, not a measurement of the
// browser's complete JavaScript heap. Entry counts also bound object overhead.
export class BudgetMap {
  constructor(countLimit, byteLimit) { this.countLimit=countLimit;this.byteLimit=byteLimit;this.map=new Map();this.bytes=0; }
  get size(){return this.map.size;}
  get(key){return this.map.get(key)?.value;}
  delete(key){const entry=this.map.get(key);if(!entry)return false;this.bytes-=entry.bytes;return this.map.delete(key);}
  clear(){this.map.clear();this.bytes=0;}
  set(key,value,bytes){
    if(!Number.isSafeInteger(bytes)||bytes<0||bytes>this.byteLimit)throw new Error('RETAINED_PAYLOAD_LIMIT');
    this.delete(key);
    while(this.map.size>=this.countLimit||this.bytes+bytes>this.byteLimit)this.delete(this.map.keys().next().value);
    this.map.set(key,{value,bytes});this.bytes+=bytes;
  }
  *entries(){for(const [key,entry]of this.map)yield [key,entry.value];}
}

export class BridgeState {
  constructor(){
    this.grants=new Map();
    this.observations=new BudgetMap(LIMITS.observations,LIMITS.observationBytes);
    this.receipts=new BudgetMap(LIMITS.receipts,LIMITS.receiptBytes);
  }
  grant(id,value){
    if(!this.grants.has(id)&&this.grants.size>=LIMITS.grants)throw new Error('TAB_GRANT_LIMIT: revoke an existing tab first');
    this.observations.delete(id);this.grants.set(id,value);
  }
  rememberObservation(id,value){this.observations.set(id,value,value.html.byteLength+8192);}
  rememberReceipt(id,value){
    const charge=JSON.stringify(value).length*2+1024;
    // Oversized diagnostic details are never required to prevent replay.
    if(charge>LIMITS.receiptBytes){value={...value,response:{ok:value.response.ok,data:{details_omitted:true,input_sent:value.response.data?.input_sent??value.response.input_sent??false}}};}
    this.receipts.set(id,value,JSON.stringify(value).length*2+1024);
  }
  statistics(){return {granted_tabs:this.grants.size,retained_observations:this.observations.size,
    observation_payload_bytes:this.observations.bytes,replay_receipts:this.receipts.size,
    receipt_payload_bytes:this.receipts.bytes,limits:LIMITS};}
}

export class ActionContext {
  constructor(request,valid,{wall=Date.now,mono=()=>performance.now(),timer=setTimeout,clearTimer=clearTimeout}={}){
    this.valid=valid;this.mono=mono;this.timer=(...args)=>timer.call(globalThis,...args);this.clearTimer=handle=>clearTimer.call(globalThis,handle);this.inputSent=false;this.cancelled=false;this.waiters=new Set();
    const deadline=request._bridge_deadline_unix_ms;
    if(!Number.isSafeInteger(deadline))throw new Error('REQUEST_DEADLINE_REQUIRED');
    this.end=mono()+Math.max(0,Math.min(LIMITS.actionMs,deadline-wall()));
  }
  cancel(){this.cancelled=true;for(const reject of this.waiters)reject(new Error('REQUEST_CANCELLED'));this.waiters.clear();}
  check(){if(this.cancelled)throw new Error('REQUEST_CANCELLED');if(this.mono()>=this.end)throw new Error('REQUEST_EXPIRED');if(!this.valid())throw new Error('GRANT_OR_CONNECTION_REVOKED');}
  async run(work,input=false,checkAfter=true){
    this.check();let timer,rejectCancellation;
    try{
      const result=await Promise.race([Promise.resolve().then(()=>{this.check();if(input)this.inputSent=true;return work();}),
        new Promise((_,reject)=>{rejectCancellation=reject;this.waiters.add(reject);}),
        new Promise((_,reject)=>{timer=this.timer(()=>reject(new Error('REQUEST_EXPIRED')),Math.max(1,this.end-this.mono()));})]);
      if(checkAfter)this.check();else if(this.mono()>=this.end)throw new Error('REQUEST_EXPIRED');return result;
    }finally{if(timer!==undefined)this.clearTimer(timer);this.waiters.delete(rejectCancellation);}
  }
}
