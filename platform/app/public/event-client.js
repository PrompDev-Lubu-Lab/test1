import { AccountError, requiresSecureAccess } from './account-client.js';

// Cookies authenticate the fixed same-origin upgrade. No credentials enter a URL.
export function eventUrl(origin) {
  const url=new URL(origin);
  if(url.protocol!=='https:' || url.origin!==origin || url.username || url.password) throw new Error('A secure application origin is required.');
  url.protocol='wss:';url.pathname='/api/events';return url.href;
}

export function createEventClient({origin,authorize,onHint=()=>{},onAuthError=()=>{},onState=()=>{},
  WebSocketImpl=globalThis.WebSocket,timers=globalThis,random=Math.random}={}) {
  let active=false,generation=0,socket=null,retry=null,flush=null,attempts=0,dirty=false,connecting=false;
  const clearTimers=()=>{timers.clearTimeout(retry);timers.clearTimeout(flush);retry=flush=null;dirty=false;};
  function stop() {
    active=false;generation++;connecting=false;clearTimers();
    const previous=socket;socket=null;try{previous?.close(1000,'View paused');}catch{}
    onState('paused');
  }
  function authError(error) {
    if(requiresSecureAccess(error)||error?.code==='login_required'||error?.code==='terms_required') {stop();onAuthError(error);return true;}
    return false;
  }
  function schedule() {
    if(!active||retry!==null)return;
    onState('polling');
    const delay=Math.min(30000,2000*2**Math.min(attempts++,4))+Math.floor(random()*1000);
    retry=timers.setTimeout(()=>{retry=null;void connect();},delay);
  }
  function hint() {
    dirty=true;if(flush!==null)return;
    // Keep automatic reads below one refresh per five seconds during bursts.
    flush=timers.setTimeout(()=>{flush=null;if(!active||!dirty)return;dirty=false;Promise.resolve().then(onHint).catch(()=>{});},5000);
  }
  async function connect() {
    if(!active||socket||connecting)return;
    const revision=generation;connecting=true;
    try {
      const session=await authorize();
      if(!active||revision!==generation)return;
      if(session?.terms_required)throw new AccountError('terms_required','Accept the current terms to continue.',403);
      const next=new WebSocketImpl(eventUrl(origin));socket=next;
      const current=()=>active&&revision===generation&&socket===next;
      next.addEventListener('open',()=>{if(current()){attempts=0;onState('connected');}});
      next.addEventListener('message',event=>{
        if(!current())return;
        if(event.data!=='{"kind":"refresh"}') {stop();onState('polling');return;}
        hint();
      });
      next.addEventListener('close',event=>{
        if(!current())return;socket=null;
        if(event.code===4401){authError(new AccountError('login_required','Your session has expired. Sign in again.',401));return;}
        if(event.code===4403){authError(new AccountError('access_denied','Renew secure access to continue.',403));return;}
        schedule();
      });
      next.addEventListener('error',()=>{
        if(!current())return;socket=null;try{next.close();}catch{}schedule();
      });
    } catch(error) {if(active&&revision===generation&&!authError(error))schedule();}
    finally {if(revision===generation)connecting=false;}
  }
  return Object.freeze({start(){if(active)return;active=true;attempts=0;void connect();},stop});
}
