import {AccountError,createAccountClient,requiresSecureAccess,turnstileAction,validatePassword} from './account-client.js';
import {createTurnstile} from './turnstile.js';
import {APP_NAME} from './config.js';

const esc=value => String(value ?? '').replace(/[&<>"']/g,char => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[char]));
const field=(label,name,{type='text',autocomplete='off',value='',help='',readonly=false}={}) => `<div class="account-field"><label for="account-${name}">${esc(label)}</label><input id="account-${name}" name="${name}" type="${type}" autocomplete="${autocomplete}" value="${esc(value)}" required ${readonly?'readonly aria-readonly="true"':''} ${type==='password'?'maxlength="256"':name==='display_name'?'maxlength="80"':'maxlength="320"'} ${type==='email'?'autocapitalize="off" spellcheck="false"':''} ${help?`aria-describedby="account-${name}-hint"`:''}>${help?`<p id="account-${name}-hint" class="field-hint">${esc(help)}</p>`:''}</div>`;
const password=(label='Password',name='password',existing=false) => field(label,name,{type:'password',autocomplete:existing?'current-password':'new-password',help:existing?'':'Use 12–128 characters. Your password is not trimmed or changed.'});
const email=(label='Account email',value='',readonly=false) => field(label,'email',{type:'email',autocomplete:'username',value,readonly});
const action=(view,label,style='text-button') => `<button type="button" class="${style}" data-account-view="${view}">${esc(label)}</button>`;
const notice=(text,tone='info') => text?`<p class="account-notice ${tone}" role="${tone==='error'?'alert':'status'}">${esc(text)}</p>`:'';
const form=(kind,body,submit,extra='') => `<form data-account-form="${kind}" class="account-form">${body}<div class="account-challenge" data-challenge-host></div><p class="account-feedback" data-account-feedback role="status" aria-live="polite"></p><div class="account-form-actions"><button class="primary" type="submit">${esc(submit)}</button>${extra}</div></form>`;

export function createAccountUI({root,incomingLink=null,onShowGate=()=>{},onAuthenticated=()=>{},onSignedOut=()=>{},onUnavailable=()=>{},onProfile=()=>{},onPreview=()=>{},client=createAccountClient(),turnstileFactory=createTurnstile}={}) {
  let config=null,turnstile=null,link=incomingLink,proof=null,busy=false,disposed=false,revision=0,allowPreview=false;
  let activeView='login',profileTarget=null,currentTerms=null,avatarUrl=null,avatarOwner=null,linkContext=null,accountEmail='';
  const session=() => client.getSession();
  const user=() => session()?.user;
  function paint(content,{title,overline='PRIVATE ACCOUNT',message='',tone='info'}={}) {
    revision++;turnstile?.cancel();onShowGate();root.hidden=false;
    root.querySelector('#account-view').innerHTML=`<section class="entry-card access-card account-card"><span class="overline">${esc(overline)}</span><h1 tabindex="-1" data-account-heading>${esc(title)}</h1>${notice(message,tone)}${content}</section>`;
    root.querySelector('[data-account-heading]')?.focus({preventScroll:true});
  }
  function footerLinks() {return `<div class="account-links">${action('login','Sign in')}${action('forgot','Forgot password?')}${action('resend','Resend verification')}</div>`;}
  function rememberEmail(value) {if(typeof value==='string'&&value.length<=320&&!/[\r\n\x00]/.test(value))accountEmail=value.trim();}
  async function resolveLink() {
    const selectedLink=link;
    if(!selectedLink?.token){show('invalid');return;}
    paint('<p class="gate-intro">Checking your secure link and its account address…</p>',{title:'Opening your email link.'});
    const attempt=revision;
    try {
      const context=await client.linkContext(selectedLink);
      if(disposed||attempt!==revision||link!==selectedLink)return;
      linkContext=context;accountEmail=context.email;
      show(selectedLink.kind==='invite'?'signup':selectedLink.kind);
    } catch(error) {
      if(disposed||attempt!==revision||link!==selectedLink)return;
      if(requiresSecureAccess(error)){show('access');return;}
      if(['invalid_token','invalid_invite'].includes(error.code)){show('invalid','This link has expired, was already used, or does not match this access identity.');return;}
      paint(`<p class="gate-intro">${esc(publicMessage(error))}</p><p class="account-small">Your link is still held privately in this tab. Retry when the account service is available.</p><div class="account-links">${action('retry-link','Retry secure link','primary')}${action('login','Back to sign in')}</div>`,{title:'Your link could not be opened.',tone:'error'});
    }
  }
  function show(view='login',message='',tone='info') {
    if(disposed)return;
    activeView=view;
    if(view!=='password-new')proof=null;
    if(view==='renew-access') {window.location.reload();return;}
    if(view==='retry-link') return resolveLink();
    if(view==='access') {paint(`<p class="gate-intro">Cloudflare secure access must be renewed before this workspace can continue. Your app session may still be valid; this does not sign it out.</p>${link?'<p class="account-small">After renewing secure access, reopen your email link to continue that request.</p>':''}<div class="account-links">${action('renew-access','Renew secure access','primary')}</div>`,{title:'Renew secure access.',message,tone});return;}
    if(view==='terms') {void showTerms();return;}
    if(view==='workspace') {if(session()){root.hidden=true;onAuthenticated(session());}return;}
    if(view==='preview') {turnstile?.cancel();proof=null;root.hidden=true;onPreview();return;}
    if(view==='invalid') {paint(`<p class="gate-intro">Open the complete link from your email again, or request a new link. This tab has removed the unusable token from its address.</p>${footerLinks()}`,{title:'This link cannot be used.',message,tone:'error'});return;}
    if(view==='login') {
      paint(`<div class="account-notice" role="note"><strong>First time here?</strong> Now that secure access is complete, reopen the original invitation button from your email in this same browser to create your account. Cloudflare sign-in alone does not create an app account or password.</div>${link?.kind==='invite'?`<div class="account-links">${action('signup','Return to your invitation','primary')}</div>`:''}<p class="account-small">Already created and verified your app account? Sign in below.</p>${form('login',email('Account email',accountEmail)+password('Password','password',true),'Sign in')}<div class="account-links">${action('forgot','Forgot password?')}${action('resend','Resend verification')}</div>${allowPreview?`<div class="account-preview-option">${action('preview','Open local synthetic preview')}</div>`:''}`,{title:`Sign in to ${APP_NAME}.`,message,tone});return;
    }
    if(view==='signup') {
      if(link?.kind!=='invite'||!link.token){show('invalid');return;}
      if(linkContext?.kind!=='invite')return resolveLink();
      paint(`<p class="gate-intro">Your invitation confirms the address below. Choose a display name and your app password; your role comes from the invitation.</p>${form('signup',email('Invited email',linkContext.email,true)+field('Display name','display_name',{autocomplete:'nickname'})+password()+password('Confirm password','confirm_password'),'Create invited account')}${footerLinks()}`,{title:'Your invitation.',message,tone});return;
    }
    if(view==='forgot'||view==='resend') {
      paint(`<p class="gate-intro">${view==='forgot'?'Request a one-use password reset link.':'Request a new verification link for your account.'}</p>${form(view,email('Account email',accountEmail),view==='forgot'?'Request reset link':'Request verification link')}${footerLinks()}`,{title:view==='forgot'?'Reset your password.':'Verify your email.',message,tone});return;
    }
    if(['verify','reset','email-change'].includes(view)) {
      if(link?.kind!==view||!link.token){show('invalid');return;}
      if(linkContext?.kind!==view)return resolveLink();
      const reset=view==='reset',change=view==='email-change';
      paint(`<p class="gate-intro">${change?'The link confirms the new address requested by the account below. The displayed address is the current one, before this change.':reset?'Choose a new password for the confirmed account below. Completed reset revokes your existing sessions.':'The link identifies the account below. Select Verify email to complete verification.'}</p>${form(view,email(change?'Current account email (before the change)':'Account email',linkContext.email,true)+(reset?password('New password')+password('Confirm new password','confirm_password'):''),reset?'Reset password':change?'Confirm email change':'Verify email')}${footerLinks()}`,{title:reset?'Choose a new password.':change?'Confirm your new email.':'Complete verification.',message,tone});return;
    }
    if(view==='verification-pending') {
      paint(`<p class="gate-intro">Your next step is to open the verification link sent to <strong>${esc(accountEmail)}</strong>. Then sign in using the app password you just chose.</p><p class="account-small">Email verification is still required. Requesting the message does not confirm it reached your inbox.</p>${footerLinks()}`,{title:'Check your email to continue.',message,tone});return;
    }
    if(!user()){show('login','Sign in again to manage your account.');return;}
    if(view==='password-current') {
      paint(`<p class="gate-intro">Confirm your current password. The server then issues a one-use, session-bound proof valid for 120 seconds.</p>${form('reauth',email('Current account email',user().email,true)+password('Current password','password',true),'Confirm current password',action('workspace','Back to workspace','secondary'))}`,{title:'Change your password.',message,tone});return;
    }
    if(view==='password-new') {
      if(!proof||proof.expiresAt<=Date.now()){show('password-current','Password confirmation expired. Confirm your current password again.','error');return;}
      paint(`<p class="gate-intro">Choose the replacement password before the confirmation expires. A successful change signs out all existing sessions.</p><p class="account-small">Confirmation expires at ${esc(new Date(proof.expiresAt).toLocaleTimeString())}.</p>${form('password',password('New password')+password('Confirm new password','confirm_password'),'Change password',action('password-current','Confirm again','secondary'))}`,{title:'Your new password.',message,tone});return;
    }
    if(view==='email') {
      paint(`<p class="gate-intro">A verification link will be requested for the new address. Your account email stays unchanged until that link is confirmed.</p>${form('email',email('Current account email',user().email,true)+field('New email','new_email',{type:'email',autocomplete:'email'}),'Request email change',action('workspace','Back to workspace','secondary'))}`,{title:'Change account email.',message,tone});return;
    }
    if(view==='logout-all') {
      paint(`<p class="gate-intro">This revokes this account’s app sessions on every device. It does not sign you out of Cloudflare Access or your identity provider.</p>${form('logout-all','','Sign out everywhere',action('workspace','Keep this session','secondary'))}`,{title:'Sign out everywhere?',message,tone});return;
    }
    show('login');
  }
  async function showTerms() {
    const review=session()?.terms_required===false;
    paint('<p class="gate-intro">Loading the current terms from the account service…</p>',{title:'Before you enter.',overline:'CURRENT ACCOUNT TERMS'});
    const attempt=revision;
    try {
      const result=await client.request('/terms');
      if(attempt!==revision||disposed)return;
      if(typeof result?.version!=='string'||result.version.length>128||typeof result.text!=='string'||!result.text||result.text.length>100000||!/^[a-f0-9]{64}$/.test(result.content_hash||''))throw new AccountError('invalid_terms','Current account terms are unavailable.');
      currentTerms=Object.freeze({version:result.version,content_hash:result.content_hash});
      const agreement=review?'':`<label class="account-agreement"><input name="accepted" type="checkbox" required disabled><span>I have read and agree to these terms.</span></label>`;
      paint(`<p class="account-small">Version ${esc(result.version)}${review?' · accepted by your account':''}</p><div class="terms-reader account-terms-reader" tabindex="0" aria-label="Current account terms"><p class="account-terms-text">${esc(result.text)}</p></div><p class="account-small" id="account-reading-status">${review?'Review the terms recorded for your account.':'Read the terms before selecting the agreement.'}</p>${review?`<div class="account-links">${action('workspace','Back to workspace','secondary')}</div>`:form('terms',agreement,'Accept and enter',action('login','Back to sign in','secondary'))}`,{title:'Your account terms.',overline:'VERSIONED TERMS'});
      if(review)return;
      const reader=root.querySelector('.account-terms-reader'),checkbox=root.querySelector('[name="accepted"]');
      const enable=() => {checkbox.disabled=false;root.querySelector('#account-reading-status').textContent='Confirm your agreement to continue. The server records the current version and content hash.';};
      requestAnimationFrame(() => {if(reader.isConnected&&reader.scrollHeight<=reader.clientHeight+2)enable();});
      reader.addEventListener('scroll',() => {if(reader.scrollTop+reader.clientHeight>=reader.scrollHeight-4)enable();});
    } catch(error) {if(attempt===revision){if(requiresSecureAccess(error)){show('access');return;}paint(`<p class="gate-intro">${esc(publicMessage(error))}</p><div class="account-links">${action('terms','Retry terms','secondary')}${action('login','Back to sign in')}</div>`,{title:'Terms could not load.'});}}
  }
  function publicMessage(error) {
    if(!(error instanceof AccountError))return 'This action could not be completed. No success has been confirmed.';
    const wait=error.retryAfter?` Try again in ${Math.ceil(error.retryAfter)} seconds.`:'';
    if(error.code==='turnstile_unavailable'||(error.status===503&&typeof error.code==='string'&&error.code.startsWith('turnstile_')))return `The Cloudflare security check is temporarily unavailable. This request could not be completed.${wait}`;
    return `${error.message}${wait}`;
  }
  function mountProfile(target,message='',tone='info') {
    profileTarget=target;
    const current=user();
    if(!current){show('login');return;}
    if(avatarOwner!==current.id)avatarUrl=null;
    if(!avatarUrl&&current.avatar){avatarUrl=current.avatar.url;avatarOwner=current.id;}
    const picture=avatarUrl&&avatarOwner===current.id?`<img class="account-avatar" src="${esc(avatarUrl)}" alt="Your saved profile picture" width="128" height="128" referrerpolicy="no-referrer">`:'<div class="account-avatar account-avatar-placeholder" aria-hidden="true">128 × 128</div>';
    const avatarBody=config?.features.avatars===true?`${picture}${form('avatar','<div class="account-field"><label for="account-avatar">Choose a profile picture</label><input id="account-avatar" name="avatar" type="file" accept="image/png,image/jpeg,image/webp" aria-describedby="account-avatar-hint" required><p id="account-avatar-hint" class="field-hint">PNG, JPEG or WebP, up to 2 MiB. The server checks the picture and saves a 128×128 WebP. Animated pictures are not supported.</p></div>','Save profile picture')}`:'<p class="account-small">Profile picture processing is not enabled. Uploads will become available when the account service reports that processing is ready.</p>';
    target.innerHTML=`${notice(message,tone)}<div class="two-column"><section class="panel"><div class="panel-head"><div><h2>Your profile</h2><p>${esc(current.email)} · ${esc(current.role)}</p></div><span class="tag">VERIFIED EMAIL</span></div><div class="panel-body">${form('profile',field('Display name','display_name',{value:current.display_name,autocomplete:'nickname'}),'Save display name')}</div></section><section class="panel"><div class="panel-head"><h2>Account security</h2></div><div class="setting-list"><div class="setting-row"><div><strong>Password</strong><p>Confirm your current password before changing it.</p></div>${action('password-current','Change','secondary')}</div><div class="setting-row"><div><strong>Email address</strong><p>The new address must be verified before it replaces this one.</p></div>${action('email','Change','secondary')}</div><div class="setting-row"><div><strong>Terms</strong><p>Review the current server version.</p></div>${action('terms','Review','secondary')}</div></div></section><section class="panel"><div class="panel-head"><h2>Profile picture</h2><span class="tag">${config?.features.avatars===true?'PRIVATE':'UNAVAILABLE'}</span></div><div class="panel-body account-avatar-body">${avatarBody}</div></section><section class="panel"><div class="panel-head"><h2>Your sessions</h2></div><div class="panel-body">${form('logout','','Sign out of this session',action('logout-all','Sign out everywhere','secondary'))}</div></section><section class="panel"><div class="panel-head"><h2>Workspace preferences</h2></div><div class="panel-body"><p class="account-small">The footer changes the scene timezone for this tab only. Live weather and saved scene preferences are not connected.</p></div></section></div>`;
  }
  async function authenticate(result) {
    if(!result?.user)throw new AccountError('invalid_response','The account service did not confirm a session.');
    accountEmail=result.user.email;
    if(result.terms_required){show('terms');return;}
    root.hidden=true;onAuthenticated(result);
  }
  function owns(element) {return root.contains(element)||Boolean(profileTarget?.contains(element));}
  async function submit(event) {
    const formElement=event.target.closest('[data-account-form]');
    if(!formElement||!owns(formElement))return;
    event.preventDefault();
    if(busy||!config||!formElement.reportValidity())return;
    const kind=formElement.dataset.accountForm,values=Object.fromEntries(new FormData(formElement)),feedback=formElement.querySelector('[data-account-feedback]');
    if(['signup','verify','reset','email-change'].includes(kind)) {
      const expectedKind=kind==='signup'?'invite':kind;
      if(linkContext?.kind!==expectedKind||link?.kind!==expectedKind||!link.token){feedback.textContent='Reopen the complete email link before continuing.';return;}
      values.email=linkContext.email;
    }
    if(typeof values.email==='string')rememberEmail(values.email);
    if(['signup','reset','password'].includes(kind)&&(!validatePassword(values.password)||values.password!==values.confirm_password)){feedback.textContent='Use 12–128 characters and enter the same new password twice.';feedback.setAttribute('role','alert');return;}
    if(kind==='terms'&&(values.accepted!=='on'||!currentTerms)){feedback.textContent='Read the current terms and select the agreement to continue.';return;}
    if(kind==='password'&&(!proof||proof.expiresAt<=Date.now())){show('password-current','Password confirmation expired. Confirm your current password again.','error');return;}
    const specs={login:['/auth/login',{email:values.email,password:values.password}],signup:['/auth/signup',{email:values.email,display_name:values.display_name,password:values.password,invite:link?.token}],forgot:['/auth/forgot',{email:values.email}],resend:['/auth/resend',{email:values.email}],verify:['/auth/verify',{email:values.email,token:link?.token}],reset:['/auth/reset',{email:values.email,token:link?.token,password:values.password}],
      'email-change':['/me/email/verify',{email:values.email,token:link?.token}],reauth:['/auth/reauth',{email:user()?.email,password:values.password}],password:['/me/password',{email:user()?.email,password:values.password,proof:proof?.token}],email:['/me/email',{email:user()?.email,new_email:values.new_email}],terms:['/terms/accept',{version:currentTerms?.version,content_hash:currentTerms?.content_hash,accepted:true}],profile:['/me',{display_name:values.display_name}],avatar:['/me/avatar',values.avatar],logout:['/auth/logout',{}],'logout-all':['/auth/logout-all',{}]};
    const spec=specs[kind];if(!spec)return;
    busy=true;formElement.setAttribute('aria-busy','true');
    const controls=[...formElement.querySelectorAll('input,button')];controls.forEach(control=>control.disabled=true);
    feedback.setAttribute('role','status');feedback.textContent='Completing this request…';
    const attempt=revision;
    try {
      let challengeToken;
      if(!['terms','profile','avatar','logout','logout-all'].includes(kind)) {
        feedback.textContent='Complete the Cloudflare security check to continue.';
        challengeToken=await turnstile.challenge(turnstileAction(spec[0]),formElement.querySelector('[data-challenge-host]'));
      }
      if(attempt!==revision||!formElement.isConnected)throw new AccountError('challenge_cancelled','This request was cancelled.');
      feedback.textContent='Waiting for the account service…';
      if(kind==='avatar'&&config.features.avatars!==true)throw new AccountError('feature_unavailable','Profile picture processing is not enabled.',503);
      const result=await client.request(spec[0],{method:kind==='profile'?'PATCH':kind==='avatar'?'PUT':'POST',body:spec[1],challengeToken});
      controls.filter(control=>control.type==='password').forEach(control=>control.value='');
      if(attempt!==revision||disposed)return;
      if(kind==='login'){await authenticate(result);return;}
      if(kind==='signup'){if(result.verification_required!==true)throw new AccountError('invalid_response','Account verification was not confirmed.');link=null;linkContext=null;show('verification-pending','Your account was created. Complete email verification before signing in.');return;}
      if(['forgot','resend'].includes(kind)){show(kind,'If the account is eligible, an email will be sent. Check your inbox and spam folder.','success');return;}
      if(kind==='verify'){if(result.verified!==true)throw new AccountError('invalid_response','Verification was not confirmed.');link=null;linkContext=null;show('login','Your email is verified. Sign in to continue.','success');return;}
      if(['reset','password','email-change'].includes(kind)) {
        const completed=kind==='reset'?result.reset===true:result.changed===true;
        if(!completed||result.sessions_revoked!==true)throw new AccountError('invalid_response','The credential change was not confirmed. Sign in again to check your account.');
        link=null;linkContext=null;proof=null;avatarUrl=null;avatarOwner=null;if(kind==='email-change')accountEmail='';onSignedOut();show('login',kind==='email-change'?'Your email was changed and existing sessions were revoked. Sign in with the new address.':'Your password was changed and existing sessions were revoked. Sign in with your new password.','success');return;
      }
      if(kind==='reauth'){if(!/^[A-Za-z0-9_-]{32,128}$/.test(result.proof||'')||!Number.isInteger(result.expires_in)||result.expires_in<1||result.expires_in>120)throw new AccountError('invalid_response','Current-password confirmation was not completed.');proof={token:result.proof,expiresAt:Date.now()+result.expires_in*1000};show('password-new');return;}
      if(kind==='email'){if(result.verification_required!==true)throw new AccountError('invalid_response','The email change request was not confirmed.');show('email','Verification was requested for the new address. Open its link, review the automatically confirmed account address, and confirm the change.','success');return;}
      if(kind==='terms'){const next=await client.request('/me');if(next.terms_required)throw new AccountError('terms_required','The server still requires acceptance of the current terms.');await authenticate(next);return;}
      if(kind==='profile'){onProfile(result.user);mountProfile(profileTarget,'Your display name was saved.','success');return;}
      if(kind==='avatar'){avatarUrl=result.url;avatarOwner=user().id;mountProfile(profileTarget,'Your 128×128 profile picture was saved.','success');return;}
      if(['logout','logout-all'].includes(kind)){if(result.signed_out!==true)throw new AccountError('invalid_response','Sign-out was not confirmed.');avatarUrl=null;avatarOwner=null;accountEmail='';link=null;linkContext=null;onSignedOut();show('login',kind==='logout-all'?'Your app sessions were revoked on every device.':'You signed out of this app session.','success');}
    } catch(error) {
      if(attempt!==revision||disposed)return;
      if(requiresSecureAccess(error)){show('access');return;}
      if(error.code==='login_required'){avatarUrl=null;avatarOwner=null;onSignedOut();show('login',publicMessage(error),'error');return;}
      feedback.textContent=publicMessage(error);feedback.setAttribute('role','alert');
      controls.filter(control=>control.type==='password').forEach(control=>control.value='');
      if(kind==='password'&&error.code==='invalid_token'){proof=null;feedback.textContent+=' Return to Confirm again to obtain a new proof.';}
    } finally {busy=false;controls.filter(control=>control.type==='file').forEach(control=>control.value='');if(formElement.isConnected){formElement.setAttribute('aria-busy','false');controls.forEach(control=>control.disabled=false);}}
  }
  function click(event) {
    const button=event.target.closest('[data-account-view]');
    if(!button||!owns(button)||busy)return;
    show(button.dataset.accountView);
  }
  function input(event) {if(event.target?.name==='email'&&owns(event.target)&&!event.target.readOnly)rememberEmail(event.target.value);}
  document.addEventListener('submit',submit);document.addEventListener('click',click);document.addEventListener('input',input);
  return Object.freeze({client,getUser:user,getConfig:()=>config,mountProfile,show,
    async start({preview=false}={}) {
      allowPreview=preview;
      try {config=await client.config();turnstile=turnstileFactory(config.turnstile_site_key);}
      catch(error){if(requiresSecureAccess(error)){show('access');return false;}config=null;proof=null;client.clear();turnstile?.cancel();onUnavailable(error,Boolean(link));return false;}
      if(disposed)return false;
      if(link){if(link.kind==='invalid')show('invalid');else await resolveLink();return true;}
      try {await authenticate(await client.request('/me'));}
      catch(error){if(requiresSecureAccess(error)){show('access');return false;}if(error.status===401)show('login');else{onUnavailable(error,false);return false;}}
      return true;
    },
    handleLink(next) {link=next;linkContext=null;accountEmail='';proof=null;if(config)return show(link?.kind==='invite'?'signup':link?.kind||'login');onUnavailable(new AccountError('configuration_required','Account access is not available yet.',503),Boolean(link));},
    secureAccessRequired() {show('access');},
    sessionExpired(message='Your session has expired. Sign in again.') {client.clear();proof=null;avatarUrl=null;avatarOwner=null;onSignedOut();show('login',message);},
    dispose(){disposed=true;revision++;link=null;linkContext=null;accountEmail='';proof=null;avatarUrl=null;avatarOwner=null;client.clear();turnstile?.cancel();document.removeEventListener('submit',submit);document.removeEventListener('click',click);document.removeEventListener('input',input);}
  });
}
