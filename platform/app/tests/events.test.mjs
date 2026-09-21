import assert from 'node:assert/strict';
import {test} from 'node:test';
import {AccountError} from '../public/account-client.js';
import {createEventClient,eventUrl} from '../public/event-client.js';

const drain=async()=>{for(let i=0;i<5;i++)await Promise.resolve();};
function harness(options={}) {
  const jobs=new Map(),sockets=[],states=[],errors=[],hints=[];let nextId=1,authCalls=0;
  class Socket extends EventTarget {
    constructor(url){super();this.url=url;sockets.push(this);}close(){this.closed=true;}
    event(type,detail={}){const event=new Event(type);Object.assign(event,detail);this.dispatchEvent(event);}
  }
  const timers={setTimeout(fn,ms){const id=nextId++;jobs.set(id,{fn,ms});return id;},clearTimeout(id){jobs.delete(id);}};
  const client=createEventClient({origin:'https://app.example.test',authorize:async()=>{authCalls++;return {terms_required:false};},onState:value=>states.push(value),onHint:()=>hints.push(true),onAuthError:error=>errors.push(error),WebSocketImpl:Socket,timers,random:()=>0,...options});
  return {client,sockets,states,errors,hints,jobs,authCalls:()=>authCalls,async run(){const [id,job]=jobs.entries().next().value;jobs.delete(id);job.fn();await drain();}};
}
test('event URL is fixed, secure and contains no session or token parameters',()=>{
  assert.equal(eventUrl('https://app.example.test'),'wss://app.example.test/api/events');
  for(const origin of ['http://app.example.test','https://app.example.test/path','https://name:secret@app.example.test'])assert.throws(()=>eventUrl(origin));
});
test('hints coalesce; client never sends data and pause releases the connection and timers',async()=>{
  const h=harness();h.client.start();await drain();const socket=h.sockets[0];assert.equal(h.authCalls(),1);
  socket.event('open');for(let i=0;i<100;i++)socket.event('message',{data:'{"kind":"refresh"}'});
  assert.equal(h.jobs.size,1);await h.run();assert.equal(h.hints.length,1);
  socket.event('message',{data:'{"kind":"refresh"}'});h.client.stop();assert.equal(h.jobs.size,0);assert.equal(socket.closed,true);
  socket.event('close',{code:1000});assert.equal(h.jobs.size,0);
});
test('failed streams retry with backoff and recheck HTTP session before every connection',async()=>{
  const h=harness();h.client.start();await drain();h.sockets[0].event('error');assert.equal(h.jobs.size,1);
  assert.equal([...h.jobs.values()][0].ms,2000);await h.run();assert.equal(h.authCalls(),2);
  h.sockets[1].event('close',{code:1006});assert.equal([...h.jobs.values()][0].ms,4000);h.client.stop();
});
test('session/Access expiry and a terms gate stop retries and route to the account view',async()=>{
  for(const [code,errorCode] of [[4401,'login_required'],[4403,'access_denied']]) {
    const h=harness();h.client.start();await drain();h.sockets[0].event('close',{code});assert.equal(h.errors[0].code,errorCode);assert.equal(h.jobs.size,0);
  }
  for(const authorize of [async()=>({terms_required:true}),async()=>{throw new AccountError('access_denied','Renew',403); }]) {
    const h=harness({authorize});h.client.start();await drain();assert.equal(h.sockets.length,0);assert.equal(h.errors.length,1);assert.equal(h.jobs.size,0);
  }
});
test('in-flight session checks cannot reconnect after sign-out, and unexpected data is not rendered',async()=>{
  let resolve;const h=harness({authorize:()=>new Promise(done=>resolve=done)});h.client.start();h.client.stop();resolve({terms_required:false});await drain();assert.equal(h.sockets.length,0);
  const other=harness();other.client.start();await drain();other.sockets[0].event('message',{data:'{"email":"private"}'});assert.equal(other.hints.length,0);assert.equal(other.jobs.size,0);assert.equal(other.sockets[0].closed,true);
});
