// Local synthetic workerd probe only. This is not a deployment entry point.
import {openEventStream} from '../events.mjs';
let allowed=true;
export default {
  async fetch(request,env) {
    const path=new URL(request.url).pathname;
    if(path==='/test/reset'){allowed=true;return new Response('reset');}
    if(path==='/test/revoke'){allowed=false;return new Response('revoked');}
    try {
      return await openEventStream(request,env,{authorize:async()=>allowed,accessExpiresAt:Date.now()+60000,sessionExpiresAt:Date.now()+60000},
        {fetchImpl:(url,options)=>env.SYNTHETIC_ORIGIN.fetch(url,options)});
    } catch(error) {return Response.json({error:error.code||'probe_failed'},{status:error.status||503});}
  }
};
