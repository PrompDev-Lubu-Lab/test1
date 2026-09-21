import test from 'node:test';
import assert from 'node:assert/strict';
import {EventEmitter} from 'node:events';
import {createRequire} from 'node:module';
const {launchVerifiedInstaller}=createRequire(import.meta.url)('../desktop/install.cjs');
test('installer launch uses the exact verified path and fixed non-shell arguments',async()=>{
  let unreferenced=false;
  await launchVerifiedInstaller('C:\\Cache\\approved.exe',{spawnImpl:(file,args,options)=>{
    assert.equal(file,'C:\\Cache\\approved.exe');assert.deepEqual(args,['--updated','--force-run']);assert.equal(options.shell,false);assert.equal(options.windowsHide,true);
    const child=new EventEmitter();child.unref=()=>{unreferenced=true;};queueMicrotask(()=>child.emit('spawn'));return child;
  }});assert.equal(unreferenced,true);
});
test('launch failures never retry, elevate or open a shell association',async()=>{
  for(const code of ['EACCES','UNKNOWN','ENOENT']) {
    let launches=0;
    await assert.rejects(launchVerifiedInstaller('C:\\Cache\\approved.exe',{spawnImpl:()=>{launches++;const child=new EventEmitter();queueMicrotask(()=>child.emit('error',Object.assign(new Error('redacted'),{code})));return child;}}));
    assert.equal(launches,1);
  }
  for(const path of ['relative.exe','\\\\remote\\installer.exe','C:\\Cache\\installer.cmd','C:\\bad\u0000.exe'])await assert.rejects(launchVerifiedInstaller(path,{spawnImpl:()=>{throw new Error('must not execute');}}));
});
