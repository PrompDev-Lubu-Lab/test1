import test from 'node:test';
import assert from 'node:assert/strict';
import {EventEmitter} from 'node:events';
import {join} from 'node:path';
import {homedir} from 'node:os';
import {createRequire} from 'node:module';
const require=createRequire(import.meta.url);
const {createDesktopUpdater}=require('../desktop/updater.cjs');
const {validateSettings}=require('../desktop/policy.cjs');
const settings=validateSettings({schema:1,channel:'production',appOrigin:'https://workspace.example.com',accessOrigin:'https://example.cloudflareaccess.com',externalOrigins:[],updatesEnabled:true,publisherNames:['Example Publisher'],certificateThumbprints:['A'.repeat(40)]});
async function fixture({verify,answers=[1,1],config={}}={}) {
  const record=[];
  const jar=new EventEmitter();jar.get=async()=>['CF_Authorization','__Host-platform-session'].map(name=>({name,value:'synthetic',domain:'workspace.example.com',hostOnly:true,path:'/',secure:true,httpOnly:true}));
  const appSession={cookies:jar,fetch:async()=>{record.push('session');return Response.json({user:{id:'owner',role:'owner',verified:true},terms_required:false,authentication_expires_at:Math.floor(Date.now()/1000)+60});}};
  const native=new EventEmitter();const cacheDir=join(process.env.LOCALAPPDATA||join(homedir(),'AppData','Local'),settings.cacheName),file=join(cacheDir,'pending','clawdie-platform-0.1.1-win-x64.exe');
  native.configOnDisk={value:Promise.resolve({provider:'generic',url:settings.feedUrl,publisherName:settings.publisherNames,updaterCacheDirName:settings.cacheName,...config})};
  native.getOrCreateDownloadHelper=async()=>({cacheDir,file});
  native.netSession={webRequest:Object.fromEntries(['onBeforeRequest','onBeforeSendHeaders','onHeadersReceived','onCompleted','onErrorOccurred'].map(name=>[name,()=>{}]))};
  const sha512=Buffer.alloc(64).toString('base64');
  native.checkForUpdates=async()=>({isUpdateAvailable:true,updateInfo:{version:'0.1.1',files:[{url:'clawdie-platform-0.1.1-win-x64.exe',sha512,size:42}]},cancellationToken:{cancel(){record.push('cancel');}}});
  native.downloadUpdate=async()=>{record.push('cached-download');return [file];};
  native.quitAndInstall=()=>{throw new Error('Forbidden native fallback path.');};
  const app={isPackaged:true,getVersion:()=> '0.1.0',quit:()=>record.push('quit')};
  let confirmations=0;
  const updater=await createDesktopUpdater({app,appSession,settings,platform:'win32',makeUpdater:()=>native,window:{},dialog:{showMessageBox:async(_window,options)=>{if(options.type==='question')return {response:answers[confirmations++]??0};record.push('warning');return {response:0};}},verifyInstaller:async policy=>{record.push('verify');assert.equal(policy.expectedSha512,sha512);assert.equal(policy.expectedSize,42);assert.equal(policy.path,file);return verify?verify({jar,record}):null;},launchInstaller:async path=>{assert.equal(path,file);record.push('launch');}});
  return {updater,native,record,jar};
}
test('cached installers still require two bound verifications and a current session before a fixed launch',async()=>{
  const f=await fixture();await f.updater.check();
  assert.equal(f.native.autoDownload,false);assert.equal(f.native.autoInstallOnAppQuit,false);assert.equal(f.native.disableWebInstaller,true);assert.equal(f.native.disableDifferentialDownload,true);assert.equal(f.native.allowDowngrade,false);
  assert.equal(f.record.filter(v=>v==='verify').length,2);assert.ok(f.record.indexOf('session')<f.record.indexOf('verify'));assert.deepEqual(f.record.filter(v=>['launch','quit','warning'].includes(v)),['launch','quit']);f.updater.dispose();
});
test('missing publisher config is rejected before any update request',async()=>{
  await assert.rejects(fixture({config:{publisherName:undefined}}),/signing policy/);
});
test('invalid signatures, changed sessions and declined installation cannot launch',async()=>{
  for(const options of [{verify:()=> 'bad signature'},{answers:[1,0]},{verify:({jar,record})=>{if(record.filter(v=>v==='verify').length===2)jar.emit('changed',{}, {name:'__Host-platform-session'});return null;}}]) {
    const f=await fixture(options);await f.updater.check();assert.equal(f.record.includes('launch'),false);assert.equal(f.record.includes('quit'),false);f.updater.dispose();
  }
});
test('declining a download avoids cache access and signature work',async()=>{
  const f=await fixture({answers:[0]});await f.updater.check();assert.equal(f.record.includes('cached-download'),false);assert.equal(f.record.includes('verify'),false);f.updater.dispose();
});
