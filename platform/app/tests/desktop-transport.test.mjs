import test from 'node:test';
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {EventEmitter} from 'node:events';
const require=createRequire(import.meta.url);
const {createUpdateTransport}=require('../desktop/transport.cjs');
const {validateSettings}=require('../desktop/policy.cjs');
const settings=validateSettings({schema:1,channel:'review',appOrigin:'https://app.example.test',accessOrigin:'https://example.cloudflareaccess.com',externalOrigins:[],updatesEnabled:false,publisherNames:[],certificateThumbprints:[]});
function fixture() {
  let cookies=['CF_Authorization','__Host-platform-session'].map(name=>({name,value:'synthetic',path:'/',domain:'app.example.test',hostOnly:true,secure:true,httpOnly:true}));
  let user={id:'owner',role:'owner',verified:true}, terms=false, fetchStatus=200, gets=0;
  const hooks={};const jar=new EventEmitter();jar.get=async()=>{gets++;return cookies;};
  const appSession={cookies:jar,fetch:async(url,options)=>{assert.equal(url,settings.appOrigin+'/api/me');assert.equal(options.redirect,'error');assert.equal(options.credentials,'include');return Response.json({user,terms_required:terms,authentication_expires_at:Math.floor(Date.now()/1000)+60},{status:fetchStatus});}};
  const updateSession={webRequest:{onBeforeRequest:f=>hooks.before=f,onBeforeSendHeaders:f=>hooks.headers=f,onHeadersReceived:f=>hooks.response=f,onCompleted:f=>hooks.completed=f,onErrorOccurred:f=>hooks.error=f}};
  let invalidations=0;
  const bridge=createUpdateTransport({appSession,updateSession,settings,onInvalidated:()=>invalidations++});
  const invoke=(which,details)=>new Promise(resolve=>{const input={id:1,method:'GET',url:settings.feedUrl+'latest.yml',requestHeaders:{},...details};if(which==='headers'||which==='response')hooks.before(input,()=>{});hooks[which](input,resolve);});
  return {bridge,invoke,jar,appSession,hooks,changeUser:v=>{user=v;},revoke:()=>{fetchStatus=401;},terms:()=>{terms=true;},replace:()=>{cookies=cookies.map(c=>({...c,value:'newSession'}));jar.emit('changed',{},cookies[1]);},get gets(){return gets;},get invalidations(){return invalidations;}};
}
test('update requests fail closed before sign-in and unapproved artifact selection',async()=>{
  const f=fixture();assert.deepEqual(await f.invoke('before'),{cancel:true});await f.bridge.begin();
  assert.deepEqual(await f.invoke('before'),{cancel:false});
  assert.deepEqual(await f.invoke('before',{url:settings.feedUrl+'clawdie-platform-0.1.1-win-x64.exe'}),{cancel:true});
  f.bridge.permitArtifact('clawdie-platform-0.1.1-win-x64.exe');
  assert.deepEqual(await f.invoke('before',{url:settings.feedUrl+'clawdie-platform-0.1.1-win-x64.exe'}),{cancel:false});f.bridge.dispose();
});
test('only current allowlisted cookies are attached and external requests read no cookies',async()=>{
  const f=fixture();await f.bridge.begin();const count=f.gets;
  assert.deepEqual(await f.invoke('headers',{url:'https://foreign.example.test/latest.yml'}),{cancel:true});assert.equal(f.gets,count);
  const result=await f.invoke('headers',{requestHeaders:{Authorization:'stale',Cookie:'other=private','CF-Access-Client-Secret':'unwanted',Accept:'*/*'}});
  assert.equal(result.requestHeaders.Cookie,'CF_Authorization=synthetic; __Host-platform-session=synthetic');
  assert.equal(result.requestHeaders.Authorization,undefined);assert.equal(result.requestHeaders['CF-Access-Client-Secret'],undefined);assert.equal(result.requestHeaders.Origin,settings.appOrigin);f.bridge.dispose();
});
test('all redirects and auth failures invalidate the operation before header replay',async()=>{
  for(const statusCode of [301,302,307,308,401,403,503]) {const f=fixture();await f.bridge.begin();assert.deepEqual(await f.invoke('response',{statusCode}),{cancel:true});assert.deepEqual(await f.invoke('before'),{cancel:true});f.bridge.dispose();}
});
test('sign-out/account change during async cookie retrieval cancels the pending request',async()=>{
  const f=fixture();const receipt=await f.bridge.begin();let finish;
  f.jar.get=()=>new Promise(resolve=>{finish=resolve;});
  const pending=f.invoke('headers');f.replace();finish([]);
  assert.deepEqual(await pending,{cancel:true});assert.equal(f.bridge.isCurrent(receipt),false);f.bridge.dispose();
});
test('current server session, account identity and terms are rechecked before installation',async()=>{
  for(const change of [f=>f.revoke(),f=>f.changeUser({id:'different',verified:true,role:'member'}),f=>f.terms()]) {const f=fixture();await f.bridge.begin();change(f);await assert.rejects(f.bridge.validate());assert.deepEqual(await f.invoke('before'),{cancel:true});f.bridge.dispose();}
});
test('an old authorization completion cannot invalidate a new update operation',async()=>{
  const f=fixture();await f.bridge.begin();const original=f.appSession.fetch;let finish,reached;
  const waiting=new Promise(resolve=>{reached=resolve;});
  f.appSession.fetch=()=>{reached();return new Promise(resolve=>{finish=resolve;});};
  const old=f.bridge.validate();await waiting;f.appSession.fetch=original;
  const receipt=await f.bridge.begin();finish(Response.json({user:{id:'owner',role:'owner',verified:true},terms_required:false,authentication_expires_at:Math.floor(Date.now()/1000)+60}));
  await assert.rejects(old);assert.equal(f.bridge.isCurrent(receipt),true);f.bridge.dispose();
});
test('responses from an older network request cannot revoke a newer update receipt',async()=>{
  const f=fixture();await f.bridge.begin();await f.invoke('before',{id:42});const receipt=await f.bridge.begin();
  const result=await new Promise(resolve=>f.hooks.response({id:42,method:'GET',url:settings.feedUrl+'latest.yml',statusCode:403},resolve));
  assert.deepEqual(result,{cancel:true});assert.equal(f.bridge.isCurrent(receipt),true);
  assert.equal(f.bridge.isCurrent({...receipt,validUntil:Date.now()-1}),false);f.bridge.dispose();
});
