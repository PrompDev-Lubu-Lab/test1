import { SecurityError } from './security.mjs';

const MAX_UPLOAD = 2 * 1024 * 1024, MAX_OUTPUT = 128 * 1024;
const bad = (code, status = 400) => { throw new SecurityError(code, status); };
export const avatarReady = env => env.AVATAR_READY === 'verified' && typeof env.IMAGES?.info === 'function' && typeof env.IMAGES?.input === 'function' && !!env.AVATARS;
const stream = bytes => new Response(bytes).body;
export async function boundedBytes(source, maximum) {
  const length = source.headers.get('Content-Length');
  if (length !== null && (!/^\d+$/.test(length) || Number(length) > maximum)) bad('image_too_large', 413);
  if (!source.body) bad('invalid_image');
  const reader = source.body.getReader(), chunks = []; let size = 0;
  try {
    while (true) {
      const {value,done} = await reader.read(); if (done) break;
      size += value.byteLength; if (size > maximum) {await reader.cancel(); bad('image_too_large',413);}
      chunks.push(value);
    }
  } finally {reader.releaseLock();}
  if (!size) bad('invalid_image');
  const result = new Uint8Array(size); let offset = 0;
  for (const chunk of chunks) {result.set(chunk,offset);offset+=chunk.length;}
  return result;
}
function formatAndAnimation(bytes) {
  const view = new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength), ascii=(from,to)=>String.fromCharCode(...bytes.subarray(from,to));
  if (bytes.length >= 8 && bytes[0]===137 && ascii(1,4)==='PNG' && bytes[4]===13 && bytes[5]===10 && bytes[6]===26 && bytes[7]===10) {
    let offset=8, ended=false;
    while(offset+12<=bytes.length) {
      const length=view.getUint32(offset), kind=ascii(offset+4,offset+8);
      if(length>bytes.length-offset-12) bad('invalid_image');
      if(['acTL','fcTL','fdAT'].includes(kind)) bad('animated_image');
      offset+=length+12; if(kind==='IEND'){ended=true;break;}
    }
    if(!ended) bad('invalid_image');
    return 'image/png';
  }
  if(bytes.length>=12 && ascii(0,4)==='RIFF' && ascii(8,12)==='WEBP') {
    const end=view.getUint32(4,true)+8; if(end>bytes.length || end<12) bad('invalid_image');
    let offset=12;
    while(offset+8<=end) {
      const kind=ascii(offset,offset+4), length=view.getUint32(offset+4,true);
      if(length>end-offset-8) bad('invalid_image');
      if(kind==='ANIM'||kind==='ANMF'||(kind==='VP8X' && length>0 && (bytes[offset+8]&2))) bad('animated_image');
      offset+=8+length+(length&1);
    }
    if(offset!==end) bad('invalid_image');
    return 'image/webp';
  }
  if(bytes.length>=3 && bytes[0]===255 && bytes[1]===216 && bytes[2]===255) return 'image/jpeg';
  return bad('unsupported_image',415);
}
/** The decoder is the Images service; header checks only reject unsupported/animated inputs early. */
export async function transformAvatar(request, images) {
  if (!images?.info || !images?.input) bad('avatar_binding_required',503);
  const type=request.headers.get('Content-Type')?.split(';')[0].toLowerCase();
  if(!['image/png','image/jpeg','image/webp'].includes(type)) bad('unsupported_image',415);
  const input=await boundedBytes(request,MAX_UPLOAD), format=formatAndAnimation(input);
  if(format!==type) bad('image_type_mismatch',415);
  let info;
  try {info=await images.info(stream(input));} catch {bad('invalid_image');}
  if(info.format!==format || !Number.isInteger(info.width) || !Number.isInteger(info.height) || info.width<1 || info.height<1 || info.width>8192 || info.height>8192 || info.width*info.height>16000000) bad('image_dimensions');
  let output;
  try {output=(await images.input(stream(input)).transform({width:128,height:128,fit:'cover',metadata:'none'}).output({format:'image/webp',quality:85,anim:false})).response();}
  catch {bad('image_transform_unavailable',503);}
  if(!output.ok || output.headers.get('Content-Type')?.split(';')[0]!=='image/webp') bad('image_transform_unavailable',503);
  const bytes=await boundedBytes(output,MAX_OUTPUT);
  let checked; try {checked=await images.info(stream(bytes));} catch {bad('image_transform_unavailable',503);}
  if(checked.format!=='image/webp' || checked.width!==128 || checked.height!==128 || formatAndAnimation(bytes)!=='image/webp') bad('image_transform_unavailable',503);
  const hash=Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',bytes)),value=>value.toString(16).padStart(2,'0')).join('');
  return {bytes,hash,width:128,height:128,mime:'image/webp'};
}

export async function uploadAvatar(request, env, store, user, guard) {
  if(!avatarReady(env)) bad('avatar_binding_required',503);
  const transformed=await transformAvatar(request,env.IMAGES);
  const previous=await store.avatar(user.id), key=`avatars/${user.id}/${crypto.randomUUID()}.webp`;
  await env.AVATARS.put(key,transformed.bytes,{httpMetadata:{contentType:'image/webp'},customMetadata:{sha256:transformed.hash}});
  let saved;
  try {saved=await store.saveAvatar(user.id,key,transformed.hash,previous?.object_key??null,Math.floor(Date.now()/1000),guard);}
  catch {
    // An uncertain DB result must not delete an object that may already be referenced.
    const current=await store.avatar(user.id).catch(()=>null);
    if(current?.object_key===key) saved=true; else bad('avatar_save_unavailable',503);
  }
  if(!saved) {await env.AVATARS.delete(key);bad('avatar_conflict',409);}
  if(previous?.object_key) await env.AVATARS.delete(previous.object_key).catch(()=>{});
  return {url:`/api/avatars/${user.id}`,width:128,height:128};
}
