import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';
import {join,resolve} from 'node:path';
import {writeFile} from 'node:fs/promises';

// Use an explicitly installed Wrangler toolchain; no install or deploy is performed.
if(!process.env.PROBE_RUNTIME_ROOT)throw new Error('Set PROBE_RUNTIME_ROOT to the directory containing a pinned Miniflare/esbuild installation.');
const require=createRequire(join(resolve(process.env.PROBE_RUNTIME_ROOT),'package.json'));
const {Miniflare,convertV4MiniflareOptions}=require('miniflare'),{build}=require('esbuild');
const bundled=await build({entryPoints:[fileURLToPath(new URL('../probes/events-runtime.mjs',import.meta.url))],bundle:true,format:'esm',platform:'browser',target:'es2022',external:['node:*'],write:false,logLevel:'silent'});
const origin='https://app.example.test';
const options={host:'127.0.0.1',port:0,cf:false,workers:[{
  name:'bridge',modules:true,script:bundled.outputFiles[0].text,compatibilityDate:'2026-09-21',compatibilityFlags:['nodejs_compat'],
  bindings:{APP_ORIGIN:origin,EVENTS_READY:'verified',RUN_ORIGIN:'https://origin.example.test',ORIGIN_CLIENT_ID:'synthetic-only',ORIGIN_CLIENT_SECRET:'synthetic-only'},
  serviceBindings:{SYNTHETIC_ORIGIN:'source'},
},{name:'source',modules:true,compatibilityDate:'2026-09-21',script:`
  export default {fetch(request){
    if(request.headers.get('Upgrade')!=='websocket')return new Response('upgrade required',{status:400});
    const pair=new WebSocketPair();pair[1].accept();const timer=setInterval(()=>pair[1].send('{"kind":"status","money":"123.456789","private":"not-forwarded"}'),500);pair[1].addEventListener('close',()=>{clearInterval(timer);});
    return new Response(null,{status:101,webSocket:pair[0]});
  }};
`}]};
const mf=new Miniflare(convertV4MiniflareOptions ? convertV4MiniflareOptions(options) : options);
const event=(socket,type,ms=25000)=>new Promise((resolve,reject)=>{
  const timeout=setTimeout(()=>{socket.removeEventListener(type,done);reject(new Error(`Timed out waiting for ${type}`));},ms);
  function done(value){clearTimeout(timeout);socket.removeEventListener(type,done);resolve(value);}
  socket.addEventListener(type,done);
});
const request=path=>mf.dispatchFetch(origin+path,{method:'POST'});
async function connect(){const response=await mf.dispatchFetch(origin+'/api/events',{headers:{Upgrade:'websocket',Origin:origin}});assert.equal(response.status,101);response.webSocket.accept();return response.webSocket;}
try {
  const invalid=await mf.dispatchFetch(origin+'/api/events',{headers:{Upgrade:'websocket'}});assert.equal(invalid.status,400);
  const client=await connect();const hint=event(client,'message');assert.equal((await hint).data,'{"kind":"refresh"}');
  const revoked=event(client,'close');await request('/test/revoke');assert.equal((await revoked).code,4401);
  await request('/test/reset');const writer=await connect();const readOnly=event(writer,'close');writer.send('start bot');assert.equal((await readOnly).code,1008);
  const third=await connect();const clientClosed=event(third,'close');third.close(1000,'Finished');await clientClosed;
  const result={checked_at:new Date().toISOString(),scope:'local synthetic workerd, no accounts, external origin or deployment',node:process.version,
    miniflare:require('miniflare/package.json').version,workerd:require('workerd/package.json').version,
    checks:{missing_origin_denied:true,actual_upgrade_101:true,values_replaced_with_refresh_hint:true,revoked_session_closed_4401:true,client_write_closed_1008:true,normal_close_completed:true}};
  await writeFile(new URL('../../docs/events-runtime-results.json',import.meta.url),JSON.stringify(result,null,2)+'\n');
  console.log(JSON.stringify(result,null,2));
} finally {await mf.dispose();}
