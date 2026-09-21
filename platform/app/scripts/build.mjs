import {cp,mkdir,writeFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import {join} from 'node:path';
const root=fileURLToPath(new URL('../',import.meta.url));
await mkdir(join(root,'dist'),{recursive:true});
await cp(join(root,'public'),join(root,'dist'),{recursive:true});
// A static build can never enable the local synthetic bypass.
await writeFile(join(root,'dist/runtime-config.json'),JSON.stringify({syntheticPreview:false,access:'closed'})+'\n');
await cp(join(root,'licenses'),join(root,'dist/licenses'),{recursive:true});
await cp(join(root,'source-manifest.json'),join(root,'dist/source-manifest.json'));
console.log('Static presentation prepared in dist; production access remains closed.');
