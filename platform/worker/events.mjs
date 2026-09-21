import { SecurityError } from './security.mjs';

const MAX_LIFETIME_MS = 300000, CHECK_MS = 15000, FLUSH_MS = 1000;
const HINT = '{"kind":"refresh"}';
const fail = () => { throw new SecurityError('event_stream_unavailable',503); };
async function boundedAuthorize(authorize,timers=globalThis) {
  let timeout;
  try {return await Promise.race([Promise.resolve().then(authorize),new Promise((_,reject)=>{timeout=timers.setTimeout(()=>reject(new Error('Session check timed out')),5000);})]);}
  finally {timers.clearTimeout(timeout);}
}

export function eventReady(env) {
  try {
    const origin=new URL(env.RUN_ORIGIN);
    return env.EVENTS_READY==='verified' && origin.protocol==='https:' && origin.origin===env.RUN_ORIGIN
      && !origin.username && !origin.password && Boolean(env.ORIGIN_CLIENT_ID && env.ORIGIN_CLIENT_SECRET);
  } catch {return false;}
}

/** Only coalesced invalidation hints cross this channel. Values are read over authenticated HTTP. */
export function bridgeEvents(upstream, downstream, {authorize,accessExpiresAt,sessionExpiresAt,now=Date.now,timers=globalThis}={}) {
  const start=now(), deadline=Math.min(start+MAX_LIFETIME_MS,accessExpiresAt,sessionExpiresAt);
  if(typeof authorize!=='function'||!Number.isFinite(deadline)||deadline<=start) throw new SecurityError('event_session_expired',401);
  let closed=false,dirty=false,checking=false,lastCheck=start,messages=0;
  let interval,expiry;
  const close=(code=1000,reason='Stream closed')=>{
    if(closed)return;closed=true;timers.clearInterval(interval);timers.clearTimeout(expiry);
    for(const socket of [upstream,downstream])try{socket.close(code,reason);}catch{}
  };
  const expire=()=>close(deadline===accessExpiresAt?4403:deadline===sessionExpiresAt?4401:4408,'Refresh secure stream');
  async function tick() {
    if(closed)return;
    const time=now();
    if(time>=deadline){expire();return;}
    if(checking)return;
    if(time-lastCheck>=CHECK_MS) {
      checking=true;
      try {if(!await boundedAuthorize(authorize,timers)){close(4401,'Session changed');return;}lastCheck=now();}
      catch {close(1011,'Session check unavailable');return;}
      finally {checking=false;}
    }
    if(closed)return;
    // Expiry is checked again after the asynchronous authorization operation.
    if(now()>=deadline){expire();return;}
    if(dirty) {
      dirty=false;
      if(++messages>300){close(4408,'Refresh secure stream');return;}
      try {downstream.send(HINT);} catch {close(1011,'Stream unavailable');}
    }
  }
  upstream.addEventListener('message',event=>{
    // No parsing or forwarding of large status records, unsafe integers or binary frames.
    // A bounded hint asks the client to reread authorized source values instead.
    if(typeof event.data!=='string'){close(1003,'Text stream required');return;}
    dirty=true;
  });
  downstream.addEventListener('message',()=>close(1008,'Read-only stream'));
  upstream.addEventListener('close',()=>close(1000,'Origin stream closed'));
  downstream.addEventListener('close',()=>close(1000,'Client stream closed'));
  upstream.addEventListener('error',()=>close(1011,'Origin stream unavailable'));
  downstream.addEventListener('error',()=>close(1011,'Client stream unavailable'));
  interval=timers.setInterval(()=>{void tick();},FLUSH_MS);
  expiry=timers.setTimeout(expire,deadline-start);
  return Object.freeze({close,tick});
}

export async function openEventStream(request,env,{authorize,accessExpiresAt,sessionExpiresAt},
  {fetchImpl=globalThis.fetch,pairFactory=()=>new WebSocketPair(),responseFactory=client=>new Response(null,{status:101,webSocket:client})}={}) {
  const url=new URL(request.url);
  if(!eventReady(env))fail();
  if(request.method!=='GET'||request.headers.get('Upgrade')?.toLowerCase()!=='websocket'||url.search
    ||request.headers.get('Origin')!==env.APP_ORIGIN) throw new SecurityError('invalid_event_upgrade',400);
  if(typeof authorize!=='function'||!Number.isFinite(accessExpiresAt)||!Number.isFinite(sessionExpiresAt)||Math.min(accessExpiresAt,sessionExpiresAt)<=Date.now()) throw new SecurityError('event_session_expired',401);
  const controller=new AbortController(),handshakeTimer=setTimeout(()=>controller.abort(),10000);
  let upstreamResponse;
  // A timeout signal left armed after 101 also terminates the upgraded socket.
  try {upstreamResponse=await fetchImpl(new URL('/events',env.RUN_ORIGIN),{redirect:'manual',headers:{Upgrade:'websocket','CF-Access-Client-Id':env.ORIGIN_CLIENT_ID,'CF-Access-Client-Secret':env.ORIGIN_CLIENT_SECRET},signal:controller.signal});}
  catch {fail();}
  finally {clearTimeout(handshakeTimer);}
  const upstream=upstreamResponse.webSocket;
  if(upstreamResponse.status!==101||!upstream){await upstreamResponse.body?.cancel().catch(()=>{});fail();}
  let downstream,bridge;
  try {
    upstream.accept();
    if(!await boundedAuthorize(authorize)){upstream.close(4401,'Session changed');throw new SecurityError('event_session_expired',401);}
    if(upstream.readyState!==1)fail();
    const pair=pairFactory(),client=pair[0];downstream=pair[1];
    downstream.accept();
    bridge=bridgeEvents(upstream,downstream,{authorize,accessExpiresAt,sessionExpiresAt});
    return responseFactory(client);
  } catch (error) {
    bridge?.close(1011,'Stream unavailable');
    for(const socket of [upstream,downstream])try{socket?.close(1011,'Stream unavailable');}catch{}
    if(error instanceof SecurityError)throw error;
    fail();
  }
}
