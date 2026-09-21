import assert from 'node:assert/strict';
import { test } from 'node:test';
import { bridgeEvents,eventReady,openEventStream } from '../events.mjs';

class Socket extends EventTarget {
  sent=[];closed=[];readyState=1;accept(){}send(value){this.sent.push(value);}close(code,reason){this.closed.push({code,reason});}
  message(data){this.dispatchEvent(new MessageEvent('message',{data}));}
}
function setup({authorize=async()=>true,accessExpiresAt=999999,sessionExpiresAt=999999}={}) {
  let time=1000;const jobs=new Set(),timers={setInterval:fn=>{jobs.add(fn);return fn;},setTimeout:(fn,delay)=>{fn.delay=delay;jobs.add(fn);return fn;},clearInterval:id=>jobs.delete(id),clearTimeout:id=>jobs.delete(id)};
  const source=new Socket(),client=new Socket();const bridge=bridgeEvents(source,client,{authorize,accessExpiresAt,sessionExpiresAt,now:()=>time,timers});
  return {source,client,bridge,jobs,setTime:value=>time=value};
}
test('bursts and oversized records coalesce to one small hint without rewriting or disclosing source fields',async()=>{
  const h=setup();for(let i=0;i<1000;i++)h.source.message('{"status":{"money":"123.456789"}}');h.source.message('x'.repeat(100000));
  await h.bridge.tick();assert.deepEqual(h.client.sent,['{"kind":"refresh"}']);await h.bridge.tick();assert.equal(h.client.sent.length,1);
  h.bridge.close();assert.equal(h.jobs.size,0);
});
test('revocation or failed authorization closes both sockets and prevents another hint',async()=>{
  for(const authorize of [async()=>false,async()=>{throw new Error('private failure');}]) {
    const h=setup({authorize});h.source.message('changed');h.setTime(16000);await h.bridge.tick();
    assert.equal(h.client.sent.length,0);assert.equal(h.client.closed.length,1);assert.equal(h.source.closed.length,1);assert.equal(h.jobs.size,0);
  }
});
test('Access/session expiry and five-minute rotation terminate streams independently',async()=>{
  for(const limits of [{accessExpiresAt:5000,sessionExpiresAt:9000},{accessExpiresAt:9000,sessionExpiresAt:5000},{}]) {
    const h=setup(limits);h.setTime(limits.accessExpiresAt?5000:301000);await h.bridge.tick();
    assert.equal(h.client.closed[0].code,limits.accessExpiresAt===5000?4403:limits.sessionExpiresAt===5000?4401:4408);assert.equal(h.jobs.size,0);
  }
});
test('client writes and source binary messages fail closed and clean up timers',()=>{
  const write=setup();write.client.message('start bot');assert.equal(write.client.closed[0].code,1008);assert.equal(write.source.sent.length,0);assert.equal(write.jobs.size,0);
  const binary=setup();binary.source.message(new ArrayBuffer(10));assert.equal(binary.client.closed[0].code,1003);
});
test('upgrade remains closed until exact verified origin and separate service credentials exist',async()=>{
  const env={EVENTS_READY:'verified',RUN_ORIGIN:'https://origin.example.test',APP_ORIGIN:'https://app.example.test',ORIGIN_CLIENT_ID:'synthetic-id',ORIGIN_CLIENT_SECRET:'synthetic-secret'};
  assert.equal(eventReady(env),true);assert.equal(eventReady({...env,EVENTS_READY:''}),false);assert.equal(eventReady({...env,RUN_ORIGIN:'http://origin.example.test'}),false);
  for(const headers of [{},{Upgrade:'websocket',Origin:'https://other.example.test'}]) await assert.rejects(openEventStream(new Request(env.APP_ORIGIN+'/api/events',{headers}),env,{}),error=>error.code==='invalid_event_upgrade');
  let contacted=0;
  await assert.rejects(openEventStream(new Request(env.APP_ORIGIN+'/api/events',{headers:{Upgrade:'websocket',Origin:env.APP_ORIGIN}}),env,{accessExpiresAt:1,sessionExpiresAt:1},{fetchImpl:()=>{contacted++;}}),error=>error.code==='event_session_expired');assert.equal(contacted,0);
});


test('hard expiry closes independently of an authorization call that has not settled',async()=>{
  let resolve;
  const h=setup({authorize:()=>new Promise(done=>resolve=done),accessExpiresAt:18000});
  h.source.message('changed');h.setTime(16000);const pending=h.bridge.tick();await Promise.resolve();
  h.setTime(18000);[...h.jobs].find(fn=>fn.delay===17000)();
  assert.equal(h.client.closed[0].code,4403);assert.equal(h.source.closed.length,1);assert.equal(h.client.sent.length,0);
  resolve(true);await pending;assert.equal(h.jobs.size,0);assert.equal(h.client.sent.length,0);
});

test('authorization timeout closes the stream and late success cannot send a hint',async()=>{
  let resolve;const h=setup({authorize:()=>new Promise(done=>resolve=done)});
  h.source.message('changed');h.setTime(16000);const pending=h.bridge.tick();await Promise.resolve();
  [...h.jobs].find(fn=>fn.delay===5000)();await pending;
  assert.equal(h.client.closed[0].code,1011);assert.equal(h.jobs.size,0);resolve(true);await Promise.resolve();assert.equal(h.client.sent.length,0);
});

test('expiry after a delayed authorization preserves the reason for rotation or app-session expiry',async()=>{
  for(const limits of [{accessExpiresAt:900000,sessionExpiresAt:301000,code:4401},{accessExpiresAt:900000,sessionExpiresAt:900000,code:4408}]) {
    let resolve;const h=setup({...limits,authorize:()=>new Promise(done=>resolve=done)});h.source.message('changed');h.setTime(300000);
    const pending=h.bridge.tick();await Promise.resolve();h.setTime(301000);resolve(true);await pending;
    assert.equal(h.client.closed[0].code,limits.code);assert.equal(h.jobs.size,0);assert.equal(h.client.sent.length,0);
  }
});

test('an upstream that closes while authorization runs cannot become a successful client upgrade',async()=>{
  const upstream=new Socket();let pairs=0;
  const env={EVENTS_READY:'verified',RUN_ORIGIN:'https://origin.example.test',APP_ORIGIN:'https://app.example.test',ORIGIN_CLIENT_ID:'synthetic-id',ORIGIN_CLIENT_SECRET:'synthetic-secret'};
  const request=new Request(env.APP_ORIGIN+'/api/events',{headers:{Upgrade:'websocket',Origin:env.APP_ORIGIN}});
  await assert.rejects(openEventStream(request,env,{authorize:async()=>{upstream.readyState=3;return true;},accessExpiresAt:Date.now()+60000,sessionExpiresAt:Date.now()+60000},
    {fetchImpl:async()=>({status:101,webSocket:upstream}),pairFactory:()=>{pairs++;}}),error=>error.code==='event_stream_unavailable');
  assert.equal(pairs,0);assert.equal(upstream.closed.length,1);
});
