import assert from 'node:assert/strict';
import { test } from 'node:test';
import { pbkdf2Sync } from 'node:crypto';
import { generateKeyPair, SignJWT } from 'jose';
import { D1Harness } from './d1-harness.mjs';
import { createHandler, TERMS_VERSION } from '../app.mjs';
import { createLimiterState, limiterTransition } from '../limiter.mjs';
import { digestToken, randomToken } from '../security.mjs';

const origin = 'https://app.example.test', issuer = 'https://team.example.test';
const password = 'Synthetic password 17!';
const record = { scheme:'pbkdf2-sha256', iterations:600000, salt:'01020304050607080910111213141516', hash:pbkdf2Sync(password, Buffer.from('01020304050607080910111213141516','hex'),600000,32,'sha256').toString('hex') };
const keys = await generateKeyPair('RS256');
const now = () => Math.floor(Date.now()/1000);

function harness(t,handlerOptions={}) {
  const db = new D1Harness(); t.after(() => db.close());
  const events = [], mail = []; let state = createLimiterState(), ip = 1;
  const env = { APP_ORIGIN:origin, ACCESS_TEAM_DOMAIN:issuer, ACCESS_AUDIENCE:'synthetic-audience',
    RATE_KEY_SECRET:'synthetic-only-key-with-at-least-thirty-two-characters', TURNSTILE_SECRET:'synthetic-secret', TURNSTILE_HOSTNAME:'app.example.test',
    DB:db, EMAIL_FROM:'no-reply@example.test', EMAIL:{send:async value => {events.push('email'); mail.push(value); return {id:'captured'};}},
    INVITE_ACCOUNTS:JSON.stringify([{email:'owner@example.test',handle:'deandre'},{email:'member@example.test',handle:'ali'}]),
    AUTH_LIMITER:{idFromName:name=>{assert.equal(name,'global-auth-v1'); return name;},get:()=>({fetch:async(url,options)=>{
      const route=new URL(url).pathname, command=JSON.parse(options.body); events.push(route.slice(1)==='result'?'outcome':route.slice(1));
      const output=limiterTransition(state,{type:route.slice(1),...command},Date.now(),crypto.randomUUID()); state=output.state;
      return Response.json(output.result,{status:output.status});
    }})}
  };
  t.mock.method(globalThis,'fetch',async(url,options)=>{
    assert.equal(String(url),'https://challenges.cloudflare.com/turnstile/v0/siteverify');
    events.push('turnstile'); const body=JSON.parse(options.body);
    return Response.json({success:true,hostname:'app.example.test',action:body.response});
  });
  const handler=createHandler({accessKeys:async()=>{events.push('access'); return keys.publicKey;},...handlerOptions});
  async function jwt(subject='owner-sub', claims={}) {
    return new SignJWT({email:`${subject}@identity.example.test`,type:'app',...claims}).setProtectedHeader({alg:'RS256'}).setIssuer(issuer).setAudience('synthetic-audience').setSubject(subject).setIssuedAt().setExpirationTime('10m').sign(keys.privateKey);
  }
  async function call(path,{method='GET',body,subject='owner-sub',cookie,csrf,headers={},token,sourceIp}={}) {
    const h={'cf-access-jwt-assertion':token??await jwt(subject),'CF-Connecting-IP':sourceIp??`192.0.2.${ip++}`,...headers};
    if(method!=='GET') {h.Origin??=origin;h['Content-Type']??='application/json';}
    if(cookie) h.Cookie=cookie; if(csrf) h['X-CSRF-Token']=csrf;
    return handler(new Request(`${origin}/api${path}`,{method,headers:h,...(body!==undefined?{body:JSON.stringify(body)}:{})}),env);
  }
  async function seed(handle='deandre',{verified=true,role=handle==='deandre'?'owner':'member',subject=handle==='deandre'?'owner-sub':'member-sub'}={}) {
    const id=handle==='deandre'?'11111111-1111-4111-8111-111111111111':'22222222-2222-4222-8222-222222222222',email=handle==='deandre'?'owner@example.test':'member@example.test';
    await db.prepare('INSERT INTO users(id,email,handle,display_name,role,access_sub,access_email,password_scheme,password_iterations,password_salt,password_hash,verified_at,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)').bind(id,email,handle,handle,role,subject,`${subject}@identity.example.test`,record.scheme,record.iterations,record.salt,record.hash,verified?now():null,now(),now()).run();
    return {id,email,subject};
  }
  async function login(email='owner@example.test',subject='owner-sub') {
    const response=await call('/auth/login',{method:'POST',subject,body:{email,password,turnstile:'login'}});
    assert.equal(response.status,200,await response.clone().text());
    const result=await response.json(); return {...result,cookie:response.headers.get('Set-Cookie').split(';')[0],subject};
  }
  async function accept(session) {
    const response=await call('/terms/accept',{method:'POST',...session,body:{version:TERMS_VERSION}});
    assert.equal(response.status,200,await response.clone().text()); return response.json();
  }
  function mailed(kind) {const message=mail.at(-1); const link=message.text.match(/https:\/\/\S+/)[0]; return new URL(link).hash.slice(kind.length+2);}
  return {db,events,mail,env,call,jwt,seed,login,accept,mailed,get state(){return state;}};
}

test('unconfigured and invalid transport fail closed without email, KDF admission or audit amplification',async t=>{
  const h=harness(t);
  assert.equal((await createHandler()(new Request(origin+'/api/me'),{})).status,503);
  for(let i=0;i<20;i++) assert.equal((await h.call('/auth/login',{method:'POST',headers:{Origin:'https://untrusted.example.test'},body:{email:'owner@example.test',password,turnstile:'login'}})).status,403);
  assert.equal(h.events.length,0);
  assert.equal((await h.db.prepare('SELECT count(*) AS n FROM audit_events').first()).n,0);
});

test('signup binds the signed subject despite differing email; verification is required and single-use',async t=>{
  const h=harness(t), invite=randomToken();
  await h.db.prepare('INSERT INTO invites(token_hash,email,handle,role,expires_at,created_at) VALUES(?,?,?,?,?,?)').bind(digestToken(invite),'owner@example.test','deandre','owner',now()+600,now()).run();
  const signup=await h.call('/auth/signup',{method:'POST',body:{email:'owner@example.test',display_name:'Owner',password,invite,turnstile:'signup'}});
  assert.equal(signup.status,201,await signup.clone().text()); assert.equal(signup.headers.get('Set-Cookie'),null);
  assert.deepEqual(h.events.slice(0,4),['ingress','access','reserve','turnstile']);
  const user=await h.db.prepare('SELECT * FROM users').first(); assert.equal(user.access_sub,'owner-sub'); assert.notEqual(user.email,user.access_email);
  assert.equal((await h.call('/auth/login',{method:'POST',body:{email:user.email,password,turnstile:'login'}})).status,401);
  const token=h.mailed('verify');
  assert.equal((await h.call('/auth/verify',{method:'POST',subject:'member-sub',body:{email:user.email,token,turnstile:'verify'}})).status,400);
  assert.equal((await h.call('/auth/verify',{method:'POST',body:{email:user.email,token,turnstile:'verify'}})).status,200);
  assert.equal((await h.call('/auth/verify',{method:'POST',body:{email:user.email,token,turnstile:'verify'}})).status,400);
  const session=await h.login(); assert.equal(session.user.role,'owner'); assert.equal(session.terms_required,true);
});

test('ten admitted wrong passwords lock login, audit failures and leave recovery available',async t=>{
  const h=harness(t); await h.seed(); const session=await h.login();
  for(let i=0;i<10;i++) assert.equal((await h.call('/auth/login',{method:'POST',sourceIp:'198.51.100.7',body:{email:'owner@example.test',password:'Wrong synthetic password',turnstile:'login'}})).status,401);
  const before=h.events.filter(e=>e==='turnstile').length;
  const limited=await h.call('/auth/login',{method:'POST',body:{email:'owner@example.test',password,turnstile:'login'}}); assert.equal(limited.status,429); assert.ok(Number(limited.headers.get('Retry-After'))>0);
  assert.equal(h.events.filter(e=>e==='turnstile').length,before);
  assert.equal((await h.db.prepare("SELECT count(*) AS n FROM audit_events WHERE action='authentication' AND outcome='denied'").first()).n,10);
  assert.equal((await h.call('/auth/logout-all',{...session,method:'POST',sourceIp:'198.51.100.7',body:{}})).status,200,'An exhausted IP must still be able to revoke a valid session.');
  assert.equal((await h.call('/auth/forgot',{method:'POST',body:{email:'owner@example.test',turnstile:'forgot'}})).status,200); assert.equal(h.mail.length,1);
});

test('terms gate, CSRF, subject binding and direct member requests enforce permissions',async t=>{
  const h=harness(t); await h.seed(); await h.seed('ali'); const session=await h.login('member@example.test','member-sub');
  assert.equal((await h.call('/runs',session)).status,403);
  assert.equal((await h.call('/terms/accept',{...session,csrf:null,method:'POST',body:{version:TERMS_VERSION}})).status,403);
  const receipt=await h.accept(session); const again=await h.accept(session); assert.deepEqual(receipt,again); assert.equal(receipt.version,TERMS_VERSION); assert.ok(receipt.accepted_at>0);
  for(const path of ['/admin/users','/admin/audit','/admin/settings']) assert.equal((await h.call(path,session)).status,403);
  assert.equal((await h.call('/admin/users/11111111-1111-4111-8111-111111111111',{...session,method:'PATCH',body:{role:'member',disabled:true}})).status,403);
  assert.equal((await h.call('/me',{...session,subject:'owner-sub'})).status,401);
  assert.equal((await h.call('/me',{...session,method:'PATCH',body:{role:'owner'}})).status,400);
});

test('password reset consumes the token and revokes every old session',async t=>{
  const h=harness(t); await h.seed(); const first=await h.login(), second=await h.login();
  assert.equal((await h.call('/auth/forgot',{method:'POST',body:{email:'owner@example.test',turnstile:'forgot'}})).status,200);
  const token=h.mailed('reset'), body={email:'owner@example.test',password:'A different synthetic password 28!',token,turnstile:'reset'};
  assert.equal((await h.call('/auth/reset',{method:'POST',body})).status,200);
  assert.equal((await h.call('/auth/reset',{method:'POST',body})).status,400);
  assert.equal((await h.call('/me',first)).status,401); assert.equal((await h.call('/me',second)).status,401);
  assert.equal((await h.call('/auth/login',{method:'POST',body:{email:body.email,password,turnstile:'login'}})).status,401);
  assert.equal((await h.call('/auth/login',{method:'POST',body:{email:body.email,password:body.password,turnstile:'login'}})).status,200);
});

test('sign-out-everywhere revokes both sessions and clears a secure cookie',async t=>{
  const h=harness(t); await h.seed(); const first=await h.login(), second=await h.login();
  const response=await h.call('/auth/logout-all',{...first,method:'POST',body:{}}); assert.equal(response.status,200); assert.match(response.headers.get('Set-Cookie'),/Secure; HttpOnly; SameSite=Strict; Max-Age=0/);
  assert.equal((await h.call('/me',first)).status,401); assert.equal((await h.call('/me',second)).status,401);
});

test('changed email requests use the current account allowance, not a caller-selected destination',async t=>{
  const h=harness(t); await h.seed(); const session=await h.login(); await h.accept(session);
  const bad=await h.call('/me/email',{...session,method:'POST',body:{email:'rotated@example.test',new_email:'destination@example.test',turnstile:'email'}}); assert.equal(bad.status,400); assert.equal(h.mail.length,0);
  const good=await h.call('/me/email',{...session,method:'POST',body:{email:'owner@example.test',new_email:'new-owner@example.test',turnstile:'email'}}); assert.equal(good.status,200); assert.equal(h.mail[0].to,'new-owner@example.test');
});

test('invalid Access assertions cannot charge or lock a named account',async t=>{
  const h=harness(t); await h.seed();
  for(let i=0;i<12;i++) assert.equal((await h.call('/auth/login',{method:'POST',token:'invalid.assertion.token',sourceIp:'198.51.100.11',body:{email:'owner@example.test',password,turnstile:'login'}})).status,403);
  assert.deepEqual(h.state.accounts,{}); assert.equal(h.state.global.length,0);
  assert.equal(h.events.includes('reserve'),false); assert.equal(h.events.includes('turnstile'),false);
  assert.equal((await h.db.prepare('SELECT count(*) AS n FROM audit_events').first()).n,0);
  const response=await h.call('/auth/login',{method:'POST',sourceIp:'198.51.100.11',body:{email:'owner@example.test',password,turnstile:'login'}});
  assert.equal(response.status,200);
});

test('ordinary successful edits neither exhaust login nor clear their own write allowance',async t=>{
  const h=harness(t); await h.seed(); const session=await h.login(); await h.accept(session);
  for(let i=0;i<12;i++) assert.equal((await h.call('/me',{...session,sourceIp:'198.51.100.20',method:'PATCH',body:{display_name:`Owner ${i}`}})).status,200);
  const writes=Object.entries(h.state.accounts).filter(([key])=>key.startsWith('write:'));
  assert.equal(writes.length,1); assert.equal(writes[0][1].times.length,13);
  assert.equal((await h.call('/auth/login',{sourceIp:'198.51.100.20',method:'POST',body:{email:'owner@example.test',password,turnstile:'login'}})).status,200);
  assert.equal((await h.call('/me',{...session,method:'PATCH',csrf:'a'.repeat(64),body:{display_name:'Invalid CSRF'}})).status,403);
});

test('successful recovery and reauthentication report success to their own account windows',async t=>{
  const h=harness(t); await h.seed(); const session=await h.login(); await h.accept(session);
  assert.equal((await h.call('/auth/forgot',{method:'POST',body:{email:'owner@example.test',turnstile:'forgot'}})).status,200);
  assert.equal(Object.entries(h.state.accounts).filter(([key])=>key.startsWith('recovery:')).length,0);
  assert.equal((await h.call('/auth/reauth',{...session,method:'POST',body:{email:'owner@example.test',password,turnstile:'reauth'}})).status,200);
  assert.equal(Object.entries(h.state.accounts).filter(([key])=>key.startsWith('reauth:')).length,0);
});

test('a resumed session receives only its authenticated avatar URL, never the private object key',async t=>{
  const h=harness(t), user=await h.seed(); const session=await h.login();
  await h.db.prepare('INSERT INTO avatars(user_id,object_key,mime_type,width,height,content_hash,updated_at) VALUES(?,?,?,?,?,?,?)').bind(user.id,'avatars/private-synthetic-key.webp','image/webp',128,128,'b'.repeat(64),now()).run();
  const result=await (await h.call('/me',session)).json();
  assert.deepEqual(result.user.avatar,{url:`/api/avatars/${user.id}`,width:128,height:128});
  assert.equal(JSON.stringify(result).includes('private-synthetic-key'),false);
});

function configuredBoard(h,fetchHook=async()=>{},instructions='Synthetic instructions.') {
  Object.assign(h.env,{BOARD_READY:'staging',BOARD_BRANCH:'board/staging',BOARD_REPO:'example-owner/example-project',BOARD_GITHUB_TOKEN:'github_pat_synthetic_test_token_never_a_real_secret'});
  let puts=0,lastText=null;
  const fetchImpl=async(input,options)=>{
    const url=new URL(input),path=url.pathname.split('/contents/')[1];
    assert.equal(url.origin,'https://api.github.com');assert.ok(['board/notes.md','board/tasks.json'].includes(path));
    await fetchHook(options.method);
    if(options.method==='GET') {
      const text=path.endsWith('.json')?JSON.stringify({tasks:[{id:'T-001',title:'Synthetic task',status:'todo',priority:'normal',assigned_by:'deandre',assigned_to:null,assigned_on:'2026-09-21',due:null,area:'platform',depends_on:[],instructions,log:[]}]}):'# Notes\n\nExisting history.\n';
      return Response.json({type:'file',path,sha:'a'.repeat(40),size:Buffer.byteLength(text),encoding:'base64',content:Buffer.from(text).toString('base64')});
    }
    assert.equal(options.method,'PUT');puts++;const body=JSON.parse(options.body);assert.equal(body.branch,'board/staging');lastText=Buffer.from(body.content,'base64').toString('utf8');
    return Response.json({content:{type:'file',path,sha:'b'.repeat(40)},commit:{sha:'c'.repeat(40)}});
  };
  return {fetchImpl,get puts(){return puts;},get text(){return lastText;}};
}

test('board routes enforce feature, terms, CSRF and session actor before a signed member write',async t=>{
  let adapter;const h=harness(t,{boardFetch:(...args)=>adapter.fetchImpl(...args)});
  await h.seed('ali');const session=await h.login('member@example.test','member-sub');
  assert.equal((await h.call('/board/notes',session)).status,403);
  await h.accept(session);assert.equal((await h.call('/board/notes',session)).status,503);
  adapter=configuredBoard(h);
  assert.equal((await h.call('/board/tasks',session)).status,200);
  const body={expected_sha:'a'.repeat(40),operation_id:'10000000-0000-4000-8000-000000000001',to:['deandre-fable'],text:'Synthetic board integration check.'};
  assert.equal((await h.call('/board/notes',{...session,method:'POST',csrf:null,body})).status,403);
  assert.equal((await h.call('/board/notes',{...session,method:'POST',body:{...body,from:'deandre-fable'}})).status,400);
  assert.equal(adapter.puts,0);
  const response=await h.call('/board/notes',{...session,method:'POST',body});assert.equal(response.status,200,await response.clone().text());
  const result=await response.json();assert.equal(result.commit_sha,'c'.repeat(40));assert.equal(result.operation_id,body.operation_id);assert.equal(adapter.puts,1);
  assert.match(adapter.text,/platform-board-operation: [0-9a-f-]+ ali /);assert.match(adapter.text,/Existing history/);
  const audit=await h.db.prepare("SELECT actor_id,outcome FROM audit_events WHERE action='board-notes'").first();
  assert.equal(audit.actor_id,session.user.id);assert.equal(audit.outcome,'committed');
});

test('a session revoked while a board file is being read cannot commit to GitHub',async t=>{
  let adapter;const h=harness(t,{boardFetch:(...args)=>adapter.fetchImpl(...args)});await h.seed();const session=await h.login();await h.accept(session);
  adapter=configuredBoard(h,async method=>{if(method==='GET')await h.db.prepare('UPDATE users SET session_version=session_version+1 WHERE id=?').bind(session.user.id).run();});
  const response=await h.call('/board/notes',{...session,method:'POST',body:{expected_sha:'a'.repeat(40),operation_id:'10000000-0000-4000-8000-000000000002',to:['all'],text:'Must remain unsent.'}});
  assert.equal(response.status,401,await response.clone().text());assert.equal(adapter.puts,0);
});

test('event upgrades require feature, current terms, exact Origin and a signed session before origin fetch',async t=>{
  let contacted=0;
  const h=harness(t,{eventOptions:{fetchImpl:async()=>{contacted++;return new Response('unavailable',{status:503});}}});
  await h.seed();const session=await h.login();
  Object.assign(h.env,{EVENTS_READY:'verified',RUN_ORIGIN:'https://origin.example.test',ORIGIN_CLIENT_ID:'synthetic-id',ORIGIN_CLIENT_SECRET:'synthetic-secret'});
  const upgrade={...session,headers:{Upgrade:'websocket',Origin:origin}};
  assert.equal((await h.call('/events',upgrade)).status,403);
  await h.accept(session);
  assert.equal((await h.call('/events',{...upgrade,token:'bad.jwt.assertion'})).status,403);
  assert.equal((await h.call('/events',{...upgrade,headers:{Upgrade:'websocket',Origin:'https://other.example.test'}})).status,403);
  assert.equal((await h.call('/events',{...session,headers:{Upgrade:'websocket'}})).status,400);
  assert.equal(contacted,0);
  assert.equal((await h.call('/events',upgrade)).status,503);assert.equal(contacted,1);
});


test('board-only sixty-four-KiB transport accepts long text plus its base while auth stays at eight KiB',async t=>{
  let adapter;const h=harness(t,{boardFetch:(...args)=>adapter.fetchImpl(...args)});await h.seed();const session=await h.login();await h.accept(session);
  adapter=configuredBoard(h,undefined,'a'.repeat(16384));
  const body={expected_sha:'a'.repeat(40),operation_id:'10000000-0000-4000-8000-000000000003',action:'update',task_id:'T-001',changes:{instructions:'b'.repeat(16384)},base:{instructions:'a'.repeat(16384)}};
  assert.ok(Buffer.byteLength(JSON.stringify(body))>32768);
  const changed=await h.call('/board/tasks',{...session,method:'POST',body});assert.equal(changed.status,200,await changed.clone().text());assert.equal(adapter.puts,1);
  assert.equal((await h.call('/board/tasks',{...session,method:'POST',body:{...body,padding:'x'.repeat(65536)}})).status,413);assert.equal(adapter.puts,1);
  assert.equal((await h.call('/auth/login',{method:'POST',body:{email:'owner@example.test',password,turnstile:'login',padding:'x'.repeat(8192)}})).status,413);
});


test('protected releases require signed Access, current session and accepted terms before touching R2',async t=>{
  const h=harness(t);let heads=0,gets=0;
  const bytes=new TextEncoder().encode('version: 0.1.1\n');
  const object={key:'releases/windows/x64/latest.yml',size:bytes.length,etag:'synthetic-etag',httpEtag:'"synthetic-etag"',version:'synthetic-object-v1'};
  h.env.DOWNLOADS_READY='verified';
  h.env.RELEASES={head:async()=>{heads++;return object;},get:async()=>{gets++;return {...object,body:new Response(bytes).body};}};
  await h.seed();const session=await h.login();
  assert.equal((await h.call('/updates/windows/x64/latest.yml',session)).status,403);assert.equal(heads,0);
  await h.accept(session);
  assert.equal((await h.call('/updates/windows/x64/latest.yml',{...session,token:'invalid'})).status,403);assert.equal(heads,0);
  const response=await h.call('/updates/windows/x64/latest.yml',session);assert.equal(response.status,200);assert.equal(await response.text(),'version: 0.1.1\n');assert.equal(gets,1);
  const head=await h.call('/updates/windows/x64/latest.yml',{...session,method:'HEAD'});assert.equal(head.status,200);assert.equal(await head.text(),'');assert.equal(gets,1);
  assert.equal((await h.call('/updates/windows/x64/latest.yml',{...session,method:'POST',body:{}})).status,405);
  await h.call('/auth/logout-all',{...session,method:'POST',body:{}});
  assert.equal((await h.call('/updates/windows/x64/latest.yml',session)).status,401);assert.equal(gets,1);
});

test('release authorization is rechecked after R2 metadata so a revoked session gets no bytes',async t=>{
  const h=harness(t);await h.seed();const session=await h.login();await h.accept(session);let gets=0;
  h.env.DOWNLOADS_READY='verified';
  h.env.RELEASES={head:async()=>{await h.db.prepare('DELETE FROM sessions').run();return {key:'releases/windows/x64/latest.yml',size:10,etag:'e',httpEtag:'"e"',version:'v'};},get:async()=>{gets++;throw new Error('must not fetch');}};
  assert.equal((await h.call('/updates/windows/x64/latest.yml',session)).status,401);assert.equal(gets,0);
});


test('session preflight reports the earlier Access and app-session expiry',async t=>{
  const h=harness(t);await h.seed();const session=await h.login();
  const token=await h.jwt('owner-sub');const accessExpiry=JSON.parse(Buffer.from(token.split('.')[1],'base64url').toString('utf8')).exp;
  const response=await h.call('/me',{...session,token});assert.equal(response.status,200);
  assert.equal((await response.json()).authentication_expires_at,accessExpiry);
  const sessionExpiry=now()+20;await h.db.prepare('UPDATE sessions SET expires_at=?').bind(sessionExpiry).run();
  assert.equal((await (await h.call('/me',{...session,token})).json()).authentication_expires_at,sessionExpiry);
});

async function pendingInvite(h) {
  const token=randomToken();
  await h.db.prepare('INSERT INTO invites(token_hash,email,handle,role,expires_at,created_at) VALUES(?,?,?,?,?,?)')
    .bind(digestToken(token),'owner@example.test','deandre','owner',now()+600,now()).run();
  return token;
}

test('link context returns only the active invitation email without consuming it or creating an account',async t=>{
  const h=harness(t),token=await pendingInvite(h);
  for(let index=0;index<2;index++) {
    const response=await h.call('/auth/link-context',{method:'POST',body:{kind:'invite',token}});
    assert.equal(response.status,200,await response.clone().text());
    assert.deepEqual(await response.json(),{kind:'invite',email:'owner@example.test'});
    assert.equal(response.headers.get('Cache-Control'),'no-store'); assert.equal(response.headers.has('Set-Cookie'),false);
  }
  assert.deepEqual(h.events,['ingress','access','reserve','outcome','ingress','access','reserve','outcome']);
  assert.deepEqual(h.state.global,[]); assert.equal(h.mail.length,0);
  for(const table of ['users','sessions','verification_tokens']) assert.equal((await h.db.prepare(`SELECT count(*) AS n FROM ${table}`).first()).n,0);
  const audit=(await h.db.prepare('SELECT actor_id,action,outcome,object_id,reason FROM audit_events ORDER BY id').all()).results;
  assert.deepEqual(audit,Array.from({length:2},()=>({actor_id:null,action:'link-context',outcome:'resolved',object_id:'invite',reason:null})));
  assert.equal(JSON.stringify(audit).includes(token),false); assert.equal(JSON.stringify(audit).includes('owner@example.test'),false);
  assert.equal((await h.db.prepare('SELECT accepted_at FROM invites').first()).accepted_at,null);
});

test('credential link context requires exact purpose, current version and signed subject and returns current email',async t=>{
  const h=harness(t); await h.seed();
  const user=await h.db.prepare('SELECT * FROM users').first();
  const tokens={};
  for(const kind of ['verify','reset','email-change']) {
    tokens[kind]=randomToken();
    await h.db.prepare('INSERT INTO verification_tokens(token_hash,user_id,purpose,pending_email,session_hash,user_version,expires_at,created_at) VALUES(?,?,?,?,?,?,?,?)')
      .bind(digestToken(tokens[kind]),user.id,kind,kind==='email-change'?'new-address@example.test':null,kind==='email-change'?digestToken('synthetic-session'):null,0,now()+600,now()).run();
    const response=await h.call('/auth/link-context',{method:'POST',body:{kind,token:tokens[kind]}});
    assert.equal(response.status,200); assert.deepEqual(await response.json(),{kind,email:user.email});
  }
  const resolved=(await h.db.prepare("SELECT object_id FROM audit_events WHERE action='link-context' AND outcome='resolved' ORDER BY id").all()).results;
  assert.deepEqual(resolved.map(row=>row.object_id),['verify','reset','email-change']);
  const lookup=(kind='reset',subject='owner-sub')=>h.call('/auth/link-context',{method:'POST',subject,body:{kind,token:tokens.reset}});
  for(const response of [await lookup('reset','member-sub'),await lookup('verify')]) {
    assert.equal(response.status,400); assert.deepEqual(await response.json(),{error:'invalid_token',detail:'This token is invalid or expired.'});
  }
  await h.db.prepare('UPDATE users SET session_version=1').run(); assert.equal((await lookup()).status,400);
  await h.db.prepare('UPDATE users SET session_version=0,disabled_at=?').bind(now()).run(); assert.equal((await lookup()).status,400);
  await h.db.prepare('UPDATE users SET disabled_at=NULL').run();
  await h.db.prepare("UPDATE verification_tokens SET used_at=? WHERE purpose='reset'").bind(now()).run(); assert.equal((await lookup()).status,400);
  await h.db.prepare("UPDATE verification_tokens SET used_at=NULL,expires_at=? WHERE purpose='reset'").bind(now()-1).run(); assert.equal((await lookup()).status,400);
  assert.equal((await h.db.prepare("SELECT count(*) AS n FROM verification_tokens WHERE purpose<>'reset' AND used_at IS NOT NULL").first()).n,0);
  assert.equal(h.mail.length,0); assert.deepEqual(h.state.global,[]);
});

test('link context rejects stale invitations and caller-selected identities behind Origin and Access gates',async t=>{
  const h=harness(t),token=await pendingInvite(h),body={kind:'invite',token};
  for(let index=0;index<20;index++) assert.equal((await h.call('/auth/link-context',{method:'POST',body,headers:{Origin:'https://other.example.test'}})).status,403);
  assert.equal((await h.call('/auth/link-context',{method:'POST',body,token:'forged-jwt'})).status,403);
  assert.deepEqual(h.state.accounts,{}); assert.deepEqual(h.state.global,[]);
  assert.equal((await h.db.prepare('SELECT count(*) AS n FROM audit_events').first()).n,0);
  for(const input of [{...body,email:'guessed@example.test'},{...body,kind:'password-change'},{...body,token:'invalid'},{...body,token:randomToken()}]) {
    const response=await h.call('/auth/link-context',{method:'POST',body:input});
    assert.equal(response.status,400); assert.deepEqual(await response.json(),{error:'invalid_token',detail:'This token is invalid or expired.'});
  }
  await h.db.prepare('UPDATE invites SET accepted_at=?').bind(now()).run();
  assert.equal((await h.call('/auth/link-context',{method:'POST',body})).status,400);
  await h.db.prepare('UPDATE invites SET accepted_at=NULL,expires_at=?').bind(now()-1).run();
  assert.equal((await h.call('/auth/link-context',{method:'POST',body})).status,400);
  assert.equal((await h.db.prepare('SELECT count(*) AS n FROM users').first()).n,0);
  const audit=(await h.db.prepare('SELECT actor_id,action,outcome,object_id,reason FROM audit_events ORDER BY id').all()).results;
  assert.equal(audit.length,6);
  for(const row of audit) {
    assert.equal(row.actor_id,null); assert.equal(row.action,'link-context'); assert.equal(row.outcome,'denied');
    assert.equal(row.reason,'invalid_token'); assert.ok(row.object_id===null||row.object_id==='invite');
  }
  assert.doesNotMatch(JSON.stringify(audit),/guessed@example|owner@example|password-change/);
  assert.equal(JSON.stringify(audit).includes(token),false);
});

test('successful link lookups retain the recovery IP budget and do not charge password budgets',async t=>{
  const h=harness(t),token=await pendingInvite(h);
  const call=()=>h.call('/auth/link-context',{method:'POST',sourceIp:'198.51.100.20',body:{kind:'invite',token}});
  for(let index=0;index<10;index++) assert.equal((await call()).status,200);
  for(let index=0;index<20;index++) {
    const limited=await call(); assert.equal(limited.status,429); assert.ok(Number(limited.headers.get('Retry-After'))>0);
  }
  assert.deepEqual(h.state.global,[]); assert.equal(h.events.includes('turnstile'),false); assert.equal(h.mail.length,0);
  assert.equal((await h.db.prepare('SELECT count(*) AS n FROM audit_events').first()).n,10);
});

test('admitted Turnstile diagnostics reach safe audit reasons without creating accounts or leaking provider content',async t=>{
  const h=harness(t),token=await pendingInvite(h);
  const privateText='private-provider-body-with-synthetic-token-and-secret';
  let respond;
  t.mock.method(globalThis,'fetch',async()=>respond());
  for(const [impl,code] of [
    [()=>Response.json({success:false,'error-codes':['invalid-input-secret'],private:privateText},{status:400}),'turnstile_provider_configuration'],
    [()=>new Response(privateText,{headers:{'Content-Type':'text/plain'}}),'turnstile_response_media_type'],
    [()=>{throw new DOMException(privateText,'TimeoutError');},'turnstile_timeout'],
  ]) {
    respond=impl;
    const response=await h.call('/auth/login',{method:'POST',body:{email:'owner@example.test',password,turnstile:'synthetic-only'}});
    assert.equal(response.status,503); assert.deepEqual(await response.json(),{error:code,detail:'This capability is temporarily unavailable or not configured.'});
    const audit=await h.db.prepare('SELECT reason FROM audit_events ORDER BY id DESC LIMIT 1').first(); assert.equal(audit.reason,code);
  }
  assert.equal((await h.db.prepare('SELECT count(*) AS n FROM users').first()).n,0);
  assert.equal((await h.db.prepare('SELECT accepted_at FROM invites WHERE token_hash=?').bind(digestToken(token)).first()).accepted_at,null);
  assert.equal(h.mail.length,0);
});

test('HTTP400 token rejections deny signup with a fresh-challenge message and preserve its invitation',async t=>{
  const h=harness(t),invite=await pendingInvite(h); let providerCode;
  t.mock.method(globalThis,'fetch',async()=>Response.json({success:false,'error-codes':[providerCode]},{status:400}));
  for(providerCode of ['missing-input-response','invalid-input-response','timeout-or-duplicate']) {
    const response=await h.call('/auth/signup',{method:'POST',body:{email:'owner@example.test',display_name:'Owner',password,invite,turnstile:'synthetic-only'}});
    assert.equal(response.status,403); const body=await response.json(); assert.equal(body.error,'turnstile_failed'); assert.match(body.detail,/fresh check/);
    assert.equal(JSON.stringify(body).includes(providerCode),false);
    const audit=await h.db.prepare('SELECT reason FROM audit_events ORDER BY id DESC LIMIT 1').first(); assert.equal(audit.reason,'turnstile_failed');
  }
  assert.equal((await h.db.prepare('SELECT count(*) AS n FROM users').first()).n,0);
  assert.equal((await h.db.prepare('SELECT accepted_at FROM invites').first()).accepted_at,null);
  assert.equal(h.mail.length,0);
});
