import test from 'node:test';
import assert from 'node:assert/strict';
import {createServer} from 'node:http';
import {createAppServer} from '../server.mjs';
async function listen(server) { await new Promise(resolve => server.listen(0,'127.0.0.1',resolve)); return `http://127.0.0.1:${server.address().port}`; }
async function close(server) { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); }
test('default mode gates all API reads and has no synthetic bypass',async () => {
  const server=createAppServer(),url=await listen(server);
  try { assert.equal((await fetch(url+'/api/runs')).status,503); assert.deepEqual(await (await fetch(url+'/runtime-config.json')).json(),{syntheticPreview:false,access:'closed'}); assert.equal((await fetch(url+'/')).status,200); }
  finally { await close(server); }
});
test('explicit preview proxies only bounded read endpoints with exact money strings',async () => {
  let request='';
  const upstream=createServer((req,res) => {request=req.url;res.setHeader('Content-Type','application/json');res.end(JSON.stringify({equity:'10000.00000001'}));});
  const apiOrigin=await listen(upstream),server=createAppServer({syntheticPreview:true,apiOrigin}),url=await listen(server);
  try {const response=await fetch(url+'/api/runs/example/equity?every=4');assert.equal(response.status,200);assert.equal(request,'/runs/example/equity?every=4');assert.equal((await response.json()).equity,'10000.00000001');assert.equal((await fetch(url+'/api/runs',{method:'POST',body:'{}'})).status,405);assert.equal((await fetch(url+'/api/live/enable')).status,404);assert.equal((await fetch(url+'/api/runs?url=http://example.com')).status,400);}
  finally {await close(server);await close(upstream);}
});
test('cross-site reads, private file paths and unsupported external API origins are rejected',async () => {
  const server=createAppServer({syntheticPreview:true}),url=await listen(server);
  try { assert.equal((await fetch(url+'/api/runs',{headers:{Origin:'https://example.com'}})).status,403);assert.equal((await fetch(url+'/%2eenv')).status,404);assert.equal((await fetch(url+'/server.mjs')).status,404);assert.equal((await fetch(url+'/unknown')).status,404);const page=await fetch(url+'/');assert.match(page.headers.get('Content-Security-Policy'),/frame-ancestors 'none'/);assert.equal(page.headers.get('Cache-Control'),'no-store'); }
  finally {await close(server);}
  assert.throws(() => createAppServer({apiOrigin:'https://example.com'}),/loopback/);
});
test('validation and bounded journal pages preserve raw legacy integers and source metadata',async () => {
  const journal=Buffer.from('{"time":1710460800123456789,"price":"3012.50000001","reason":"fixture ✓"}\r\n','utf8');
  const upstream=createServer((req,res) => {
    res.setHeader('X-Data-Source','synthetic-fixtures');
    if(req.url==='/runs/sample/validation') {res.setHeader('Content-Type','application/json');res.end('{"verdict":{"pass":true,"failures":[]}}');return;}
    assert.equal(req.url,'/instances/sample/journal?after=0&limit=1');
    res.setHeader('Content-Type','application/x-ndjson; charset=utf-8');
    res.setHeader('X-Next-After','1');res.setHeader('X-Has-More','true');res.end(journal);
  });
  const apiOrigin=await listen(upstream),server=createAppServer({syntheticPreview:true,apiOrigin}),url=await listen(server);
  try {
    const validation=await fetch(url+'/api/runs/sample/validation');assert.equal(validation.status,200);assert.equal((await validation.json()).verdict.pass,true);
    const response=await fetch(url+'/api/instances/sample/journal?after=0&limit=1');
    assert.equal(response.status,200);assert.deepEqual(Buffer.from(await response.arrayBuffer()),journal);
    assert.match(response.headers.get('Content-Type'),/^application\/x-ndjson/);
    assert.equal(response.headers.get('X-Data-Source'),'synthetic-fixtures');assert.equal(response.headers.get('X-Next-After'),'1');assert.equal(response.headers.get('X-Has-More'),'true');
    for(const query of ['limit=0','limit=10001','limit=-1','limit=1&limit=2','after=1000000001','every=0']) assert.equal((await fetch(url+'/api/instances/sample/journal?'+query)).status,400);
  } finally {await close(server);await close(upstream);}
});
