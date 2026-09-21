import {AccountError} from './account-client.js';
let scriptPromise;
function loadTurnstile() {
  if (globalThis.turnstile?.render && globalThis.turnstile?.execute) return Promise.resolve(globalThis.turnstile);
  if (!scriptPromise) scriptPromise=new Promise((resolve,reject) => {
    const script=document.createElement('script');
    let timer;
    const fail=() => {clearTimeout(timer);script.remove();scriptPromise=null;reject(new AccountError('turnstile_unavailable','The security check could not load. Try again when it is available.'));};
    script.src='https://challenges.cloudflare.com/turnstile/v0/api.js?render=explicit';
    script.async=true;script.defer=true;script.referrerPolicy='no-referrer';
    script.addEventListener('error',fail,{once:true});
    script.addEventListener('load',() => {clearTimeout(timer);if(globalThis.turnstile?.render) resolve(globalThis.turnstile);else fail();},{once:true});
    timer=setTimeout(fail,15000);document.head.append(script);
  });
  return scriptPromise;
}
export function createTurnstile(sitekey) {
  let active=null;
  function cancel() {active?.cancel();}
  return Object.freeze({cancel,async challenge(action,host) {
    cancel();
    if (typeof sitekey !== 'string' || !sitekey || !/^[A-Za-z0-9_-]{1,32}$/.test(action) || !host?.isConnected) throw new AccountError('turnstile_unavailable','The security check is not configured.');
    const provider=await loadTurnstile();
    if (!host.isConnected) throw new AccountError('challenge_cancelled','The security check was cancelled.');
    host.replaceChildren();
    return new Promise((resolve,reject) => {
      let widget,timer,settled=false;
      const finish=(error,token) => {
        if(settled)return;settled=true;clearTimeout(timer);
        if(widget!==undefined){try{provider.remove(widget);}catch{/* The provider may already have removed an expired widget. */}}
        host.replaceChildren();active=null;
        if(error)reject(error);else resolve(token);
      };
      active={cancel:() => finish(new AccountError('challenge_cancelled','The security check was cancelled.'))};
      timer=setTimeout(() => finish(new AccountError('turnstile_timeout','The security check timed out. Submit again for a fresh check.')),120000);
      try {widget=provider.render(host,{sitekey,action,theme:'auto',size:'flexible',execution:'execute',appearance:'interaction-only',
        callback:token => typeof token==='string'&&token ? finish(null,token) : finish(new AccountError('turnstile_failed','The security check did not complete.')),
        'error-callback':() => {finish(new AccountError('turnstile_failed','The security check did not complete. Submit again to retry.'));return true;},
        'expired-callback':() => finish(new AccountError('turnstile_expired','The security check expired. Submit again for a new check.')),
        'timeout-callback':() => finish(new AccountError('turnstile_timeout','The security check timed out. Submit again to retry.'))});
      if(settled){try{provider.remove(widget);}catch{}return;}
      provider.execute(widget);
      } catch {finish(new AccountError('turnstile_failed','The security check could not start. Submit again to retry.'));}
    });
  }});
}
