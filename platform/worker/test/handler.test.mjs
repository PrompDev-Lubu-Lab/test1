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

function configuredBoard(h,fetchHook=async()=>{}) {
  Object.assign(h.env,{BOARD_READY:'staging',BOARD_BRANCH:'board/staging',BOARD_REPO:'example-owner/example-project',BOARD_GITHUB_TOKEN:'github_pat_synthetic_test_token_never_a_real_secret'});
  let puts=0,lastText=null;
  const fetchImpl=async(input,options)=>{
    const url=new URL(input),path=url.pathname.split('/contents/')[1];
    assert.equal(url.origin,'https://api.github.com');assert.ok(['board/notes.md','board/tasks.json'].includes(path));
    await fetchHook(options.method);
    if(options.method==='GET') {
      const text=path.endsWith('.json')?' {"tasks":[]}':'# Notes\n\nExisting history.\n';
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
