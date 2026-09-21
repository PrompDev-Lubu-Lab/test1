import test from 'node:test';
import assert from 'node:assert/strict';
import {createAccountClient} from '../public/account-client.js';
import {createAccountUI} from '../public/account-ui.js';
import {APP_NAME} from '../public/config.js';

const token='x'.repeat(43),email='invited@example.test';
// Event/markup harness only: these checks do not claim browser or Turnstile QA.
function harness(t,{link=null,respond}={}) {
  const previousDocument=globalThis.document,previousFormData=globalThis.FormData;
  const listeners=new Map(),view={innerHTML:''},calls=[],challenges=[];
  globalThis.document={addEventListener(kind,handler){listeners.set(kind,handler);},removeEventListener(kind,handler){if(listeners.get(kind)===handler)listeners.delete(kind);}};
  globalThis.FormData=class {constructor(form){this.values=form.values;}*[Symbol.iterator](){yield* Object.entries(this.values);}};
  const root={hidden:true,contains:element=>element?.owned===true,querySelector:selector=>selector==='#account-view'?view:selector==='[data-account-heading]'?{focus(){}}:null};
  const client=createAccountClient({fetchImpl:async(url,options)=>{
    const body=options.body?JSON.parse(options.body):null;calls.push({url,options,body});
    if(url==='/api/config')return Response.json({account_service:true,turnstile_site_key:'synthetic-site-key',identityEmail:'access-identity@example.test',features:{}});
    const custom=await respond?.(url,body);if(custom)return custom;
    if(url==='/api/auth/link-context')return Response.json({kind:body.kind,email});
    if(url==='/api/me')return Response.json({error:'login_required',detail:'Sign in again.'},{status:401});
    throw new Error('Unexpected local test request');
  }});
  const ui=createAccountUI({root,incomingLink:link,client,turnstileFactory:()=>({cancel(){},async challenge(action){challenges.push(action);return `synthetic-${action}`;}})});
  t.after(()=>{ui.dispose();if(previousDocument===undefined)delete globalThis.document;else globalThis.document=previousDocument;globalThis.FormData=previousFormData;});
  return {ui,calls,challenges,html:()=>view.innerHTML,listeners,
    input(value){listeners.get('input')({target:{owned:true,name:'email',value,readOnly:false}});},
    async submit(kind,values={}) {
      const feedback={textContent:'',setAttribute(){}},controls=Object.entries(values).map(([name,value])=>({name,value,type:name.includes('password')?'password':'text',disabled:false}));
      const form={owned:true,isConnected:true,dataset:{accountForm:kind},values,reportValidity:()=>true,closest:()=>form,setAttribute(){},querySelector:selector=>selector==='[data-account-feedback]'?feedback:{isConnected:true},querySelectorAll:()=>controls};
      await listeners.get('submit')({target:form,preventDefault(){}});
      return {feedback,controls};
    }
  };
}

test('invitation waits for protected context and renders its confirmed address read-only without a token',async t=>{
  const h=harness(t,{link:{kind:'invite',token}});await h.ui.start();
  assert.deepEqual(h.calls.map(call=>call.url),['/api/config','/api/auth/link-context']);
  assert.match(h.html(),/Your invitation\./);assert.match(h.html(),/value="invited@example\.test" required readonly aria-readonly="true"/);
  assert.equal(h.html().includes(token),false);assert.equal(h.html().includes('access-identity@example.test'),false);
  assert.deepEqual(h.challenges,[]);
});

test('bare origin explains invitation recovery before login and does not invent an app account from Access',async t=>{
  const h=harness(t);await h.ui.start();
  assert.equal(APP_NAME,'Clawdie');assert.match(h.html(),/Sign in to Clawdie\./);
  assert.ok(h.html().indexOf('reopen the original invitation')<h.html().indexOf('data-account-form="login"'));
  assert.match(h.html(),/Cloudflare sign-in alone does not create an app account or password/);
  assert.equal(h.html().includes('access-identity@example.test'),false);assert.equal(h.html().includes('Welcome back'),false);
  assert.equal(h.calls.some(call=>call.url==='/api/auth/link-context'),false);
});

test('signup uses confirmed identity and keeps email through verification instructions and sign-in',async t=>{
  const h=harness(t,{link:{kind:'invite',token},respond:async url=>url==='/api/auth/signup'?Response.json({verification_required:true,email_accepted_by_provider:true},{status:201}):null});
  await h.ui.start();
  await h.submit('signup',{email:'altered@example.test',display_name:'Synthetic user',password:'synthetic long password',confirm_password:'synthetic long password'});
  assert.equal(h.calls.at(-1).body.email,email);assert.equal(h.calls.at(-1).body.invite,token);assert.deepEqual(h.challenges,['signup']);
  assert.match(h.html(),/Check your email to continue/);assert.match(h.html(),/invited@example\.test/);
  h.ui.show('login');assert.match(h.html(),/value="invited@example\.test"/);
  assert.equal(h.html().includes(token),false);
});

test('verification and reset retain the confirmed address on the next sign-in form',async t=>{
  const h=harness(t,{link:{kind:'verify',token},respond:async url=>url==='/api/auth/verify'?Response.json({verified:true}):url==='/api/auth/reset'?Response.json({reset:true,sessions_revoked:true}):null});
  await h.ui.start();await h.submit('verify',{email:'altered@example.test'});
  assert.equal(h.calls.at(-1).body.email,email);assert.match(h.html(),/Your email is verified/);assert.match(h.html(),/value="invited@example\.test"/);
  await h.ui.handleLink({kind:'reset',token:'r'.repeat(43)});
  await h.submit('reset',{email:'altered@example.test',password:'new synthetic password',confirm_password:'new synthetic password'});
  assert.equal(h.calls.at(-1).body.email,email);assert.match(h.html(),/value="invited@example\.test"/);assert.deepEqual(h.challenges,['verify','reset']);
});

test('typed email stays in memory between account views and is cleared by a different link and disposal',async t=>{
  const h=harness(t);await h.ui.start();h.input('typed@example.test');
  for(const view of ['forgot','resend','login']){h.ui.show(view);assert.match(h.html(),/value="typed@example\.test"/);}
  await h.ui.handleLink({kind:'invite',token});assert.equal(h.html().includes('typed@example.test'),false);assert.match(h.html(),/invited@example\.test/);
  h.ui.dispose();assert.equal(h.listeners.size,0);
});

test('unavailable context is retried explicitly and invalid tokens never render a signup form',async t=>{
  let state='unavailable';
  const h=harness(t,{link:{kind:'invite',token},respond:async url=>{
    if(url!=='/api/auth/link-context')return null;
    return state==='unavailable'?Response.json({error:'configuration_required',detail:'Temporarily unavailable'},{status:503}):Response.json({error:'invalid_token',detail:'Invalid or expired link'},{status:400});
  }});
  await h.ui.start();assert.match(h.html(),/Retry secure link/);assert.equal(h.html().includes('data-account-form="signup"'),false);
  state='invalid';await h.ui.show('retry-link');assert.match(h.html(),/This link cannot be used/);assert.equal(h.html().includes(email),false);
  assert.equal(h.calls.filter(call=>call.url==='/api/auth/link-context').length,2);assert.deepEqual(h.challenges,[]);
});

test('a delayed context response cannot replace a newer email link identity',async t=>{
  let finishOld,started;const ready=new Promise(resolve=>started=resolve);
  const h=harness(t,{link:{kind:'invite',token},respond:async(url,body)=>{
    if(url!=='/api/auth/link-context')return null;
    if(body.token===token){started();return new Promise(resolve=>finishOld=resolve);}
    return Response.json({kind:'invite',email:'new-invite@example.test'});
  }});
  const first=h.ui.start();await ready;
  await h.ui.handleLink({kind:'invite',token:'n'.repeat(43)});finishOld(Response.json({kind:'invite',email:'old-invite@example.test'}));await first;
  assert.match(h.html(),/new-invite@example\.test/);assert.equal(h.html().includes('old-invite@example.test'),false);
});

test('a Turnstile service failure explains availability, clears passwords and preserves the typed address',async t=>{
  const h=harness(t,{respond:async url=>url==='/api/auth/login'?Response.json({error:'turnstile_unavailable',detail:'This capability is temporarily unavailable or not configured.'},{status:503}):null});
  await h.ui.start();const result=await h.submit('login',{email:'typed@example.test',password:'synthetic login password'});
  assert.match(result.feedback.textContent,/Cloudflare security check is temporarily unavailable/);
  assert.equal(result.controls.find(control=>control.type==='password').value,'');
  h.ui.show('login');assert.match(h.html(),/value="typed@example\.test"/);assert.equal(h.ui.getUser(),undefined);
});
