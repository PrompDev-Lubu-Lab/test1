import {cp,mkdir,writeFile,readFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import {join} from 'node:path';
let webSocketOrigin=null;
if(process.env.APP_ORIGIN) {
  const origin=new URL(process.env.APP_ORIGIN);
  if(origin.protocol!=='https:' || origin.origin!==process.env.APP_ORIGIN || origin.username || origin.password)throw new Error('APP_ORIGIN must be an exact HTTPS origin.');
  webSocketOrigin=`wss://${origin.host}`;
}
const root=fileURLToPath(new URL('../',import.meta.url));
await mkdir(join(root,'dist'),{recursive:true});
await cp(join(root,'public'),join(root,'dist'),{recursive:true});
if(webSocketOrigin) {
  const path=join(root,'dist/_headers');
  const headers=await readFile(path,'utf8');
  await writeFile(path,headers.replace("connect-src 'self'",`connect-src 'self' ${webSocketOrigin}`));
}
// A static build can never enable the local synthetic bypass.
await writeFile(join(root,'dist/runtime-config.json'),JSON.stringify({syntheticPreview:false,access:'closed'})+'\n');
await cp(join(root,'licenses'),join(root,'dist/licenses'),{recursive:true});
await cp(join(root,'source-manifest.json'),join(root,'dist/source-manifest.json'));
console.log('Static presentation prepared in dist; production access remains closed.');
