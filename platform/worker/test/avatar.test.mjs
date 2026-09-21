import assert from 'node:assert/strict';
import {test} from 'node:test';
import {readFile} from 'node:fs/promises';
import {transformAvatar,boundedBytes,avatarReady,uploadAvatar} from '../avatar.mjs';
import {AccountStore} from '../store.mjs';
import {D1Harness} from './d1-harness.mjs';

const png=await readFile(new URL('./fixtures/avatar-input.png',import.meta.url));
const webp=await readFile(new URL('./fixtures/avatar-output.webp',import.meta.url));
const request=(bytes=png,type='image/png')=>new Request('https://app.example.test/api/me/avatar',{method:'PUT',headers:{'Content-Type':type},body:bytes});
function images({badInput=false,badOutput=false}={}) {
  let infoCalls=0;
  return {async info(input){await new Response(input).arrayBuffer();infoCalls++;return infoCalls%2===1?{format:'image/png',width:badInput?20000:256,height:128}:{format:'image/webp',width:badOutput?127:128,height:128};},input(){return {transform(options){assert.deepEqual(options,{width:128,height:128,fit:'cover',metadata:'none'});return {async output(options){assert.deepEqual(options,{format:'image/webp',quality:85,anim:false});return {response:()=>new Response(webp,{headers:{'Content-Type':'image/webp'}})};}};}};}};
}

test('avatar transport limits reject oversized actual bytes, unsupported types and declared-type mismatch',async()=>{
  await assert.rejects(transformAvatar(request(Buffer.alloc(2*1024*1024+1)),images()),e=>e.status===413);
  await assert.rejects(transformAvatar(request(Buffer.from('<svg/>'),'image/svg+xml'),images()),e=>e.status===415);
  await assert.rejects(transformAvatar(request(png,'image/jpeg'),images()),e=>e.code==='image_type_mismatch');
  await assert.rejects(boundedBytes(new Response(new Uint8Array(21),{headers:{'Content-Length':'1'}}),20),e=>e.status===413);
});

test('animation markers and excessive decoded dimensions cannot reach avatar storage',async()=>{
  const chunk=Buffer.from('000000006163544c00000000','hex'),animated=Buffer.concat([png.subarray(0,8),chunk,png.subarray(8)]);
  await assert.rejects(transformAvatar(request(animated),images()),e=>e.code==='animated_image');
  await assert.rejects(transformAvatar(request(),images({badInput:true})),e=>e.code==='image_dimensions');
  await assert.rejects(transformAvatar(request(),images({badOutput:true})),e=>e.code==='image_transform_unavailable');
});

test('validated output must be a 128 square WebP with a content hash; binding stays opt-in',async()=>{
  const transformed=await transformAvatar(request(),images());
  assert.deepEqual(transformed.bytes,new Uint8Array(webp)); assert.equal(transformed.width,128); assert.equal(transformed.height,128); assert.equal(transformed.hash,'d6140b8c348415c98b1e290460918ac18e975c2c5f347e6e8b4965ea1bb8060c');
  assert.equal(avatarReady({IMAGES:images(),AVATARS:{}}),false);
  assert.equal(avatarReady({AVATAR_READY:'verified',IMAGES:images(),AVATARS:{}}),true);
});

async function setup(t) {
  const db=new D1Harness();t.after(()=>db.close());const store=new AccountStore(db);
  await db.prepare("INSERT INTO users(id,email,handle,display_name,role,access_sub,access_email,password_scheme,password_iterations,password_salt,password_hash,verified_at,created_at,updated_at) VALUES('user','owner@example.test','deandre','Owner','owner','subject','access@example.test','pbkdf2-sha256',600000,?,?,1,1,1)").bind('a'.repeat(32),'b'.repeat(64)).run();
  const now=Math.floor(Date.now()/1000),user=await store.userById('user'),sessionHash='c'.repeat(64);
  assert.equal(await store.newSession({hash:sessionHash,csrfHash:'d'.repeat(64),user,subject:'subject',accessEmail:user.access_email,now}),true);
  return {db,store,user,guard:{sessionHash,subject:'subject',version:0},now};
}

test('concurrent avatar replacements have one winner and a revoked session cannot save',async t=>{
  const {store,guard,now}=await setup(t);
  const results=await Promise.all(['first','second'].map(key=>store.saveAvatar('user',key,'e'.repeat(64),null,now,guard)));
  assert.deepEqual(results,[true,false]);
  await store.revoke('user',guard.sessionHash,true,now,guard);
  assert.equal(await store.saveAvatar('user','third','e'.repeat(64),'first',now,guard),false);
  assert.equal((await store.avatar('user')).object_key,'first');
});

test('only transformed bytes enter R2 and failed authorization removes the unreferenced new object',async t=>{
  const {store,user,guard,now}=await setup(t); const objects=new Map();
  const bucket={async put(key,value){objects.set(key,value);},async delete(key){objects.delete(key);}};
  const env={AVATAR_READY:'verified',IMAGES:images(),AVATARS:bucket};
  const result=await uploadAvatar(request(),env,store,user,guard);
  assert.equal(result.url,'/api/avatars/user'); assert.equal(objects.size,1); assert.deepEqual([...objects.values()][0],new Uint8Array(webp));
  await store.revoke('user',guard.sessionHash,true,now,guard);
  env.IMAGES=images();
  await assert.rejects(uploadAvatar(request(),env,store,user,guard),e=>e.code==='avatar_conflict');
  assert.equal(objects.size,1);
});
