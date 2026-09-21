const TOKEN = /^[A-Za-z0-9_-]{32,128}$/;
const CSRF = /^[a-f0-9]{64}$/;
const LINK_KINDS = new Set(['invite','verify','reset','email-change']);
const ROUTES = new Map([
  ['/config',{method:'GET'}],['/me',{method:'GET',session:true}],['/terms',{method:'GET',session:true}],
  ['/board/tasks',{method:'GET',session:true}],['/board/notes',{method:'GET',session:true}],
  ['/downloads',{method:'GET',session:true}],
  ['/auth/signup',{method:'POST',challenge:true}],['/auth/login',{method:'POST',challenge:true}],
  ['/auth/forgot',{method:'POST',challenge:true}],['/auth/resend',{method:'POST',challenge:true}],
  ['/auth/verify',{method:'POST',challenge:true}],['/auth/reset',{method:'POST',challenge:true}],
  ['/me/email/verify',{method:'POST',challenge:true}],
  ['/auth/reauth',{method:'POST',session:true,challenge:true}],['/me/password',{method:'POST',session:true,challenge:true}],
  ['/me/email',{method:'POST',session:true,challenge:true}],['/terms/accept',{method:'POST',session:true}],
  ['/me/avatar',{method:'PUT',session:true,binary:true}],
  ['/auth/logout',{method:'POST',session:true}],['/auth/logout-all',{method:'POST',session:true}],
]);
export class AccountError extends Error {
  constructor(code, message, status = 0, retryAfter = null) { super(message); this.name='AccountError'; this.code=code; this.status=status; this.retryAfter=retryAfter; }
}
export function requiresSecureAccess(error) {return error?.status===403 && error?.code==='access_denied';}
export function consumeTokenFragment(location, history) {
  const fragment = String(location.hash || '').slice(1);
  const params = new URLSearchParams(fragment);
  const recognized = [...params.keys()].some(key => LINK_KINDS.has(key));
  if (!recognized) return null;
  // Scrub before validation, rendering, fetching or loading any third-party script.
  history.replaceState(null,'',`${location.pathname}${location.search || ''}`);
  const entries = [...params.entries()];
  if (entries.length !== 1 || !LINK_KINDS.has(entries[0][0]) || !TOKEN.test(entries[0][1])) return Object.freeze({kind:'invalid',token:null});
  return Object.freeze({kind:entries[0][0],token:entries[0][1]});
}
export function validatePassword(password) {
  if (typeof password !== 'string' || password.length > 256) return false;
  const points = [...password];
  return points.length >= 12 && points.length <= 128 && new TextEncoder().encode(password).length <= 512 && !points.some(point => {const value=point.codePointAt(0);return value>=0xd800 && value<=0xdfff;});
}
export function turnstileAction(route) {
  const spec = ROUTES.get(route);
  if (!spec?.challenge) throw new AccountError('invalid_route','This action does not use an authentication challenge.');
  return route.split('/').at(-1);
}
export function parseAccountConfig(value) {
  if (!value || value.account_service !== true || typeof value.turnstile_site_key !== 'string' || !value.turnstile_site_key.trim() || value.turnstile_site_key.length > 256) throw new AccountError('configuration_required','Account access is not available yet.',503);
  return Object.freeze({account_service:true,turnstile_site_key:value.turnstile_site_key,features:Object.freeze({avatars:value.features?.avatars === true,events:value.features?.events === true,board:value.features?.board === true,downloads:value.features?.downloads === true})});
}
export function validateAvatarResponse(value,userId) {
  if (!value || value.width!==128 || value.height!==128 || typeof value.url!=='string' || !/^\/api\/avatars\/[A-Za-z0-9_-]{1,128}$/.test(value.url) || value.url!==`/api/avatars/${encodeURIComponent(userId)}`) throw new AccountError('invalid_avatar','The account service did not confirm a safe profile picture.');
  return Object.freeze({url:value.url,width:128,height:128});
}
function safeUser(value) {
  if (!value || typeof value.id !== 'string' || typeof value.email !== 'string' || typeof value.display_name !== 'string' || !['owner','member'].includes(value.role) || value.verified !== true) throw new AccountError('invalid_response','The account service returned an incomplete session.');
  const avatar = value.avatar ? validateAvatarResponse(value.avatar,value.id) : null;
  return Object.freeze({id:value.id,email:value.email,display_name:value.display_name,handle:typeof value.handle === 'string' ? value.handle : '',role:value.role,verified:true,...(avatar ? {avatar} : {})});
}
export function createAccountClient({fetchImpl = globalThis.fetch.bind(globalThis)} = {}) {
  let csrf = null, user = null, termsRequired = true, revision = 0;
  const clear = () => {revision++;csrf=null;user=null;termsRequired=true;};
  async function request(route,{method,body,challengeToken} = {}) {
    const startedRevision=revision;
    const spec = ['/board/tasks','/board/notes'].includes(route) && method === 'POST' ? {method:'POST',session:true} : route === '/me' && method === 'PATCH' ? {method:'PATCH',session:true} : ROUTES.get(route);
    if (!spec || (method && method !== spec.method)) throw new AccountError('invalid_route','This account action is unavailable.');
    const verb = spec.method;
    const mutation = verb !== 'GET';
    const headers = {Accept:'application/json'};
    let payload;
    if (mutation) {
      if (!body || typeof body !== 'object' || Array.isArray(body)) throw new AccountError('invalid_body','Complete the form before continuing.');
      if (spec.session && !CSRF.test(csrf || '')) throw new AccountError('login_required','Refresh your sign-in before making this change.',401);
      if (spec.challenge && (typeof challengeToken !== 'string' || !challengeToken || challengeToken.length > 2048)) throw new AccountError('turnstile_required','Complete the security check to continue.');
      if(spec.binary) {
        if(!(body instanceof Blob)||body.size<1||body.size>2*1024*1024||!['image/png','image/jpeg','image/webp'].includes(body.type)) throw new AccountError('invalid_avatar','Choose a PNG, JPEG or WebP picture no larger than 2 MiB.');
        payload=body;headers['Content-Type']=body.type;
      } else {payload={...body};headers['Content-Type']='application/json';}
      if (spec.challenge) payload.turnstile=challengeToken;
      if (spec.session) headers['X-CSRF-Token']=csrf;
    }
    let response;
    try { response=await fetchImpl(`/api${route}`,{method:verb,headers,credentials:'same-origin',cache:'no-store',redirect:'manual',referrerPolicy:'no-referrer',signal:AbortSignal.timeout(30000),...(mutation ? {body:spec.binary?payload:JSON.stringify(payload)} : {})}); }
    catch { throw new AccountError('network_unavailable','The account service could not be reached. Check your connection and try again.'); }
    if(response.type==='opaqueredirect'||(response.status>=300&&response.status<400)) throw new AccountError('access_denied','Renew secure access to continue. Your app session may still be valid.',403);
    let result;
    try {result=await response.json();} catch {throw new AccountError('invalid_response','The account service returned an unreadable response.',response.status);}
    if(startedRevision!==revision)throw new AccountError('account_changed','The account changed while this request was running.',409);
    if (!response.ok) {
      const code=typeof result?.error === 'string' ? result.error : 'request_failed';
      if (code==='login_required' || (route==='/me' && response.status===401)) clear();
      const detail=typeof result?.detail === 'string' && result.detail.length <= 500 ? result.detail : 'This action could not be completed.';
      const retry=Number(response.headers.get('Retry-After'));
      throw new AccountError(code,detail,response.status,Number.isFinite(retry)&&retry>0 ? retry : null);
    }
    if (route==='/auth/login' || (route==='/me' && verb==='GET')) {
      if (!CSRF.test(result?.csrf || '') || typeof result.terms_required !== 'boolean') {clear();throw new AccountError('invalid_response','The account service returned an incomplete session.');}
      let nextUser;
      try {nextUser=safeUser(result.user);} catch(error) {clear();throw error;}
      csrf=result.csrf;user=nextUser;termsRequired=result.terms_required;
      // Never return the CSRF value to a renderer template or a logging callback.
      return {user,terms_required:termsRequired};
    }
    if (route==='/me' && verb==='PATCH') {user=safeUser(result?.user);return {user};}
    if (route==='/me/avatar') {const avatar=validateAvatarResponse(result,user?.id);user=Object.freeze({...user,avatar});return avatar;}
    if (route==='/terms/accept') termsRequired=false;
    if (['/auth/logout','/auth/logout-all','/auth/reset','/me/password','/me/email/verify'].includes(route)) clear();
    return result;
  }
  return Object.freeze({request,clear,getSession:() => user ? {user,terms_required:termsRequired} : null,async config(){return parseAccountConfig(await request('/config'));}});
}
