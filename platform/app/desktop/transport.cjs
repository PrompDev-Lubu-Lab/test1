'use strict';
const {allowedUpdateUrl,sessionCookies,COOKIE_NAMES} = require('./policy.cjs');
const CLOSED = 'Sign in again before checking for updates.';
async function boundedJson(response) {
  if (!response.ok || response.redirected || response.headers.get('content-type')?.split(';')[0] !== 'application/json') throw new Error(CLOSED);
  const reader = response.body?.getReader();
  if (!reader) throw new Error(CLOSED);
  let size=0;const chunks=[];
  try { while (true) { const next=await reader.read();if(next.done)break;size+=next.value.byteLength;if(size>16384)throw new Error(CLOSED);chunks.push(Buffer.from(next.value)); } }
  finally { try { await reader.cancel(); } catch { /* Already closed. */ } }
  return JSON.parse(Buffer.concat(chunks).toString('utf8'));
}
function createUpdateTransport({appSession,updateSession,settings,onInvalidated=()=>{}}) {
  let revision=0, active=null, artifact=null;
  const requests=new Map();
  function invalidate(expectedRevision) {if(expectedRevision!==undefined && expectedRevision!==revision)return;revision++;active=null;artifact=null;requests.clear();onInvalidated();}
  const cookieChanged = (_event,cookie) => {if(COOKIE_NAMES.includes(cookie.name))invalidate();};
  appSession.cookies.on('changed',cookieChanged);
  function permitted(details) {
    if (!active || active.expiresAt<=Date.now() || !['GET','HEAD'].includes(details.method) || !allowedUpdateUrl(details.url,settings)) return false;
    const name=new URL(details.url).pathname.split('/').at(-1);
    return name==='latest.yml' || name===artifact;
  }
  updateSession.webRequest.onBeforeRequest((details,callback)=>{const allowed=permitted(details);if(allowed)requests.set(details.id,revision);callback({cancel:!allowed});});
  updateSession.webRequest.onBeforeSendHeaders((details,callback)=>{
    const seen=revision;
    if(!permitted(details) || requests.get(details.id)!==seen) {callback({cancel:true});return;}
    void appSession.cookies.get({url:details.url}).then(cookies=>{
      const current=sessionCookies(cookies,settings.appOrigin);
      if(seen!==revision || !active || current.fingerprint!==active.fingerprint || !permitted(details)) throw new Error(CLOSED);
      const headers={};
      for(const [key,value] of Object.entries(details.requestHeaders ?? {})) if(!/^(cookie|authorization|proxy-authorization|cf-access-.*|referer)$/i.test(key))headers[key]=value;
      headers.Cookie=current.header;
      headers.Origin=settings.appOrigin;
      callback({requestHeaders:headers});
    }).catch(()=>{invalidate(seen);callback({cancel:true});});
  });
  // Reject every redirect before the updater can replay any credential headers.
  updateSession.webRequest.onHeadersReceived((details,callback)=>{
    const own=requests.get(details.id);
    if(own!==revision) {callback({cancel:true});return;}
    if(!permitted(details) || ![200,206].includes(details.statusCode)) {invalidate(own);callback({cancel:true});return;}
    callback({});
  });
  updateSession.webRequest.onCompleted(details=>requests.delete(details.id));
  updateSession.webRequest.onErrorOccurred(details=>requests.delete(details.id));
  async function authenticate({initial=false}={}) {
    const seen=revision;
    try {
    const before=sessionCookies(await appSession.cookies.get({url:settings.feedUrl}),settings.appOrigin);
    const result=await boundedJson(await appSession.fetch(settings.appOrigin+'/api/me',{method:'GET',credentials:'include',redirect:'error',cache:'no-store',signal:AbortSignal.timeout(10000)}));
    const after=sessionCookies(await appSession.cookies.get({url:settings.feedUrl}),settings.appOrigin);
    if(seen!==revision || before.fingerprint!==after.fingerprint || !result.user?.id || !result.user.verified || result.terms_required!==false || !['owner','member'].includes(result.user.role) || !Number.isSafeInteger(result.authentication_expires_at) || result.authentication_expires_at*1000<=Date.now()) throw new Error(CLOSED);
    if(!initial && (!active || active.fingerprint!==after.fingerprint || active.userId!==result.user.id)) throw new Error(CLOSED);
    active={fingerprint:after.fingerprint,userId:result.user.id,expiresAt:Math.min(after.expiresAt,result.authentication_expires_at*1000)};
    return {userId:result.user.id,revision,validUntil:Math.min(Date.now()+20000,active.expiresAt)};
    } catch {invalidate(seen);throw new Error(CLOSED);}
  }
  return {
    begin:async()=>{invalidate();return authenticate({initial:true});},
    validate:()=>authenticate(),
    permitArtifact(name){if(!active || !allowedUpdateUrl(settings.feedUrl+name,settings))throw new Error(CLOSED);artifact=name;},
    isCurrent(receipt){return Boolean(active && receipt.revision===revision && receipt.userId===active.userId && receipt.validUntil>Date.now());},
    invalidate,
    dispose(){invalidate();appSession.cookies.removeListener('changed',cookieChanged);updateSession.webRequest.onBeforeRequest(null);updateSession.webRequest.onBeforeSendHeaders(null);updateSession.webRequest.onHeadersReceived(null);updateSession.webRequest.onCompleted(null);updateSession.webRequest.onErrorOccurred(null);}
  };
}
module.exports={createUpdateTransport};
