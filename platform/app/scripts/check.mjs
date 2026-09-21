import {readdir,readFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import {spawnSync} from 'node:child_process';
const root=fileURLToPath(new URL('../',import.meta.url));
let checked=0;
async function visit(path) {
  for(const entry of await readdir(path,{withFileTypes:true})) {
    if (['node_modules','dist','.git'].includes(entry.name)) continue;
    const file=join(path,entry.name);
    if(entry.isDirectory()) await visit(file);
    else if(/\.(?:js|mjs|cjs)$/.test(file)) { const result=spawnSync(process.execPath,['--check',file],{encoding:'utf8'}); if(result.status!==0) throw new Error(result.stderr || `Syntax failed: ${file}`); checked++; }
  }
}
await visit(root);
const manifest=JSON.parse(await readFile(join(root,'source-manifest.json'),'utf8'));
for(const file of manifest.files) {
  const hash=createHash('sha256').update(await readFile(join(root,file.destination))).digest('hex');
  if(hash!==file.sha256) throw new Error(`Source hash mismatch: ${file.destination}`);
}
console.log(`${checked} JavaScript files checked; ${manifest.files.length} imported source hashes verified.`);
