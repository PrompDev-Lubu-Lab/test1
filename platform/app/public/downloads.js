export function validateDownload(value) {
  const version=/^(0|[1-9]\d{0,5})\.(0|[1-9]\d{0,5})\.(0|[1-9]\d{0,5})$/;
  if(!value || typeof value.version!=='string' || !version.test(value.version))throw new Error('Release information is unavailable.');
  const file=`clawdie-platform-${value.version}-win-x64.exe`;
  if(value.file!==file || value.url!==`/api/updates/windows/x64/${file}` || !Number.isSafeInteger(value.size) || value.size<1 || value.size>1073741824 || typeof value.sha512!=='string' || !/^[A-Za-z0-9+/]{86}==$/.test(value.sha512) || typeof value.released_at!=='string' || !Number.isFinite(Date.parse(value.released_at)) || (value.notes!==undefined && (typeof value.notes!=='string' || value.notes.length>6000)))throw new Error('Release information is unavailable.');
  return Object.freeze({version:value.version,file,url:value.url,size:value.size,sha512:value.sha512,released_at:value.released_at,notes:value.notes??''});
}
