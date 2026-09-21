import test from 'node:test';
import assert from 'node:assert/strict';
import {consumeTokenFragment,createAccountClient,parseAccountConfig,parseAccountLinkContext,requiresSecureAccess,turnstileAction,validateAvatarResponse,validatePassword} from '../public/account-client.js';

const token='x'.repeat(43),csrf='a'.repeat(64);
const user={id:'test-user',email:'person@example.test',display_name:'Test account',handle:'member-test',role:'member',verified:true};
const session={user,csrf,terms_required:true};
function mockClient(responses) {
  const calls=[];
  const client=createAccountClient({fetchImpl:async (url,options) => {
    calls.push({url,options});
    const next=responses.shift();
    if(!next)throw new Error('Unexpected request');
    return next.response ?? Response.json(next.body,{status:next.status||200,headers:next.headers||{}});
  }});
  return {client,calls};
}
test('credential fragments are consumed and scrubbed before use, including malformed and duplicate tokens',() => {
  for(const kind of ['invite','verify','reset','email-change']) {
    const location={pathname:'/',search:'?view=account',hash:`#${kind}=${token}`},calls=[];
    const value=consumeTokenFragment(location,{replaceState(...args){calls.push(args);}});
    assert.deepEqual(value,{kind,token});assert.deepEqual(calls,[[null,'','/?view=account']]);
    assert.equal(JSON.stringify(calls).includes(token),false);
    assert.equal(Object.hasOwn(value,'email'),false);
  }
  for(const hash of ['#verify=short',`#invite=${token}&invite=${token}`,`#reset=${token}&email=person@example.test`]) {
    const calls=[];assert.deepEqual(consumeTokenFragment({pathname:'/',search:'',hash},{replaceState(...args){calls.push(args);}}),{kind:'invalid',token:null});assert.equal(calls.length,1);
  }
  assert.equal(consumeTokenFragment({hash:'#overview'},{replaceState(){throw Error('Ordinary navigation must not be scrubbed');}}),null);
});
test('only configured account service enables optional features; missing or false flags stay false',() => {
  assert.deepEqual(parseAccountConfig({account_service:true,turnstile_site_key:'public-test-key',features:{avatars:true,board:'true',downloads:false}}).features,{avatars:true,events:false,board:false,downloads:false});
  for(const value of [{},{account_service:false,turnstile_site_key:'key'},{account_service:true,turnstile_site_key:''}])assert.throws(()=>parseAccountConfig(value),/not available/);
});

test('protected link context sends credentials only in its body and returns a matching confirmed email',async()=>{
  for(const kind of ['invite','verify','reset','email-change']) {
    const {client,calls}=mockClient([{body:{kind,email:user.email}}]);
    assert.deepEqual(await client.linkContext({kind,token}),{kind,email:user.email});
    assert.equal(calls[0].url,'/api/auth/link-context');assert.equal(calls[0].options.method,'POST');
    assert.equal(calls[0].options.credentials,'same-origin');assert.equal(calls[0].options.cache,'no-store');
    assert.equal(calls[0].options.redirect,'manual');assert.equal(calls[0].options.referrerPolicy,'no-referrer');
    assert.deepEqual(JSON.parse(calls[0].options.body),{kind,token});
    assert.equal(calls[0].options.headers['X-CSRF-Token'],undefined);assert.equal(calls[0].options.headers.Authorization,undefined);
    assert.equal(client.getSession(),null);assert.throws(()=>turnstileAction('/auth/link-context'),/does not use/);
  }
});

test('link context rejects malformed credentials and unconfirmed or mismatched response identities',async()=>{
  const {client,calls}=mockClient([]);
  for(const link of [null,{kind:'login',token},{kind:'invite',token:'short'}])await assert.rejects(client.linkContext(link),/cannot be used/);
  assert.equal(calls.length,0);
  for(const value of [null,{kind:'verify',email:user.email},{kind:'invite',email:' Person@example.test '},{kind:'invite',email:'x<y@example.test'},{kind:'invite',email:'bad..local@example.test'},{kind:'invite',email:'x@-bad.example'}])assert.throws(()=>parseAccountLinkContext(value,'invite'),/did not confirm/);
  assert.deepEqual(parseAccountLinkContext({kind:'invite',email:"o'owner@example.test",untrusted:'ignored'},'invite'),{kind:'invite',email:"o'owner@example.test"});
});
test('same-origin requests use cookies, no redirects and transient CSRF without returning it to view callbacks',async () => {
  const {client,calls}=mockClient([{body:session},{body:{user:{...user,display_name:'Changed'}}}]);
  const result=await client.request('/auth/login',{body:{email:user.email,password:' a long test password '},challengeToken:'login-challenge'});
  assert.equal(result.csrf,undefined);assert.equal(client.getSession().csrf,undefined);
  await client.request('/me',{method:'PATCH',body:{display_name:'Changed'}});
  assert.equal(calls[0].url,'/api/auth/login');assert.equal(calls[0].options.credentials,'same-origin');assert.equal(calls[0].options.redirect,'manual');assert.equal(calls[0].options.referrerPolicy,'no-referrer');
  assert.equal(calls[0].options.headers.Authorization,undefined);assert.equal(calls[0].options.headers['X-CSRF-Token'],undefined);
  assert.equal(JSON.parse(calls[0].options.body).password,' a long test password ');
  assert.equal(calls[1].options.headers['X-CSRF-Token'],csrf);assert.equal(JSON.parse(calls[1].options.body).csrf,undefined);
  assert.equal(client.getSession().user.display_name,'Changed');
});
test('mutations without session CSRF or an auth challenge fail before any network request',async () => {
  const {client,calls}=mockClient([]);
  await assert.rejects(client.request('/me',{method:'PATCH',body:{display_name:'x'}}),/sign-in/);
  await assert.rejects(client.request('/auth/login',{body:{email:user.email,password:'long password'}}),/security check/);
  for(const route of ['https://example.test/auth/login','//example.test/auth/login','/auth/login?token=secret','/admin/users'])await assert.rejects(client.request(route),/unavailable/);
  assert.equal(calls.length,0);
});
test('sensitive password changes use the current email, one-use proof and a separate challenge for each action',async () => {
  const {client,calls}=mockClient([{body:session},{body:{proof:token,expires_in:120}},{body:{changed:true,sessions_revoked:true}}]);
  await client.request('/me');
  const confirmed=await client.request('/auth/reauth',{body:{email:user.email,password:'current test password'},challengeToken:'reauth-fresh'});
  await client.request('/me/password',{body:{email:user.email,proof:confirmed.proof,password:'replacement test password'},challengeToken:'password-fresh'});
  const first=JSON.parse(calls[1].options.body),second=JSON.parse(calls[2].options.body);
  assert.equal(first.turnstile,'reauth-fresh');assert.equal(second.turnstile,'password-fresh');assert.equal(second.proof,token);assert.equal(second.email,user.email);
  assert.equal(client.getSession(),null);
});
test('email change separates the current account address from the target and confirmation needs no app CSRF',async () => {
  const {client,calls}=mockClient([{body:session},{body:{verification_required:true}},{body:{changed:true,sessions_revoked:true}}]);
  await client.request('/me');
  await client.request('/me/email',{body:{email:user.email,new_email:'new@example.test'},challengeToken:'email-fresh'});
  client.clear();
  await client.request('/me/email/verify',{body:{email:user.email,token},challengeToken:'verification-fresh'});
  assert.deepEqual(JSON.parse(calls[1].options.body),{email:user.email,new_email:'new@example.test',turnstile:'email-fresh'});
  assert.equal(calls[1].options.headers['X-CSRF-Token'],csrf);assert.equal(calls[2].options.headers['X-CSRF-Token'],undefined);
  assert.equal(JSON.parse(calls[2].options.body).email,user.email);
  assert.equal(turnstileAction('/me/email/verify'),'verify');assert.equal(turnstileAction('/me/password'),'password');assert.equal(turnstileAction('/auth/reauth'),'reauth');
});
test('unavailable terms and invalid sessions never become successful acceptance or sign-in',async () => {
  const {client}=mockClient([{body:session},{status:503,body:{error:'configuration_required',detail:'Unavailable'}},{body:{...session,user:{...user,verified:false}}}]);
  await client.request('/me');await assert.rejects(client.request('/terms/accept',{body:{version:'v1',accepted:true}}),/Unavailable/);
  assert.equal(client.getSession().terms_required,true);
  await assert.rejects(client.request('/me'),/incomplete session/);assert.equal(client.getSession(),null);
});
test('password validation counts Unicode characters and preserves spaces without weakening the server policy',() => {
  assert.equal(validatePassword('123456789012'),true);assert.equal(validatePassword(' 1234567890 '),true);assert.equal(validatePassword('😀'.repeat(12)),true);
  for(const value of ['short','a'.repeat(129),'😀'.repeat(129),'a'.repeat(11)+'\ud800',null])assert.equal(validatePassword(value),false);
});
test('avatar uploads send raw bytes, their actual MIME and CSRF without a Turnstile token',async () => {
  const result={url:'/api/avatars/test-user',width:128,height:128};
  const {client,calls}=mockClient([{body:session},{body:result}]);
  await client.request('/me');
  const picture=new Blob([new Uint8Array([137,80,78,71])],{type:'image/png'});
  assert.deepEqual(await client.request('/me/avatar',{method:'PUT',body:picture}),result);
  assert.equal(calls[1].url,'/api/me/avatar');assert.equal(calls[1].options.method,'PUT');
  assert.equal(calls[1].options.body,picture);assert.equal(calls[1].options.headers['Content-Type'],'image/png');
  assert.equal(calls[1].options.headers['X-CSRF-Token'],csrf);assert.equal(calls[1].options.credentials,'same-origin');
});
test('invalid avatar files are rejected before upload and a disabled service never reports success',async () => {
  const {client,calls}=mockClient([{body:session},{status:503,body:{error:'configuration_required',detail:'Picture processing is unavailable'}}]);
  const picture=new Blob(['test'],{type:'image/webp'});
  await assert.rejects(client.request('/me/avatar',{method:'PUT',body:picture}),/sign-in/);
  assert.equal(calls.length,0);await client.request('/me');
  for(const value of [new Blob(['test'],{type:'image/gif'}),new Blob([],{type:'image/png'}),new Blob([new Uint8Array(2*1024*1024+1)],{type:'image/jpeg'})])await assert.rejects(client.request('/me/avatar',{method:'PUT',body:value}),/PNG, JPEG or WebP/);
  assert.equal(calls.length,1);await assert.rejects(client.request('/me/avatar',{method:'PUT',body:picture}),/unavailable/);
  assert.equal(client.getSession().user.id,user.id);
});
test('avatar responses must identify the current user at a plain same-origin 128-pixel URL',() => {
  const result={url:'/api/avatars/test-user',width:128,height:128};
  assert.deepEqual(validateAvatarResponse(result,user.id),result);
  for(const url of ['https://example.test/api/avatars/test-user','//example.test/picture','/api/avatars/another-user','/api/avatars/test-user?token=x','/api/avatars/../test-user'])assert.throws(()=>validateAvatarResponse({...result,url},user.id),/safe profile picture/);
  assert.throws(()=>validateAvatarResponse({...result,width:256},user.id),/safe profile picture/);
});
test('403 access_denied requests renewal and preserves the app session, while 401 expires it',async () => {
  const {client}=mockClient([{body:session},{status:403,body:{error:'access_denied',detail:'Renew secure access'}},{status:401,body:{error:'login_required',detail:'Sign in again'}}]);
  await client.request('/me');
  await assert.rejects(client.request('/me'),error=>requiresSecureAccess(error));
  assert.equal(client.getSession().user.id,user.id);
  await assert.rejects(client.request('/me'),error=>error.status===401&&!requiresSecureAccess(error));
  assert.equal(client.getSession(),null);
  assert.equal(requiresSecureAccess({status:403,code:'role_required'}),false);
});

test('an Access edge redirect requests full navigation without following it or discarding the app session',async()=>{
  const {client}=mockClient([{body:session},{response:{type:'opaqueredirect',status:0}},{response:{type:'basic',status:302}}]);
  await client.request('/auth/login',{body:{email:user.email,password:'Long synthetic password'},challengeToken:'test'});
  for(let i=0;i<2;i++) {await assert.rejects(client.request('/me'),requiresSecureAccess);assert.deepEqual(client.getSession(),{user,terms_required:true});}
});

test('a saved profile picture is restored only from validated current-user session metadata',async()=>{
  const avatar={url:'/api/avatars/test-user',width:128,height:128};
  const {client}=mockClient([{body:{...session,user:{...user,avatar}}},{body:{...session,user:{...user,avatar:{...avatar,url:'/api/avatars/another-user'}}}}]);
  const restored=await client.request('/me');assert.deepEqual(restored.user.avatar,avatar);
  await assert.rejects(client.request('/me'),/safe profile picture/);assert.equal(client.getSession(),null);
});


test('a delayed session response cannot restore account state after local sign-out',async()=>{
  let complete;const client=createAccountClient({fetchImpl:()=>new Promise(resolve=>complete=resolve)});
  const pending=client.request('/me');client.clear();
  complete(Response.json({csrf:'a'.repeat(64),terms_required:false,user:{id:'old-user',email:'owner@example.test',display_name:'Owner',handle:'deandre',role:'owner',verified:true}}));
  await assert.rejects(pending,error=>error.code==='account_changed');assert.equal(client.getSession(),null);
});
