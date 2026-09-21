import { transformAvatar } from '../avatar.mjs';
export default {
  async fetch(request,env) {
    if(request.method!=='POST') return new Response('Synthetic avatar compatibility probe. POST only.',{status:405});
    try {
      const result=await transformAvatar(request,env.IMAGES);
      return new Response(result.bytes,{headers:{'Content-Type':result.mime,'X-Image-Width':String(result.width),'X-Image-Height':String(result.height),'X-Content-SHA256':result.hash,'Cache-Control':'no-store'}});
    } catch(error) {return Response.json({error:error.code??'probe_failed'},{status:error.status??503});}
  }
};
