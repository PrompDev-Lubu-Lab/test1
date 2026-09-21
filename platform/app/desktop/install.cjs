'use strict';
const {spawn}=require('node:child_process');
// Called only by main after consent, current-session, checksum and signature checks.
// Do not use NsisUpdater.doInstall: its error path invokes elevation/openPath fallbacks.
function launchVerifiedInstaller(path,{spawnImpl=spawn}={}) {
  if(typeof path!=='string' || !/^[A-Za-z]:[\\/]/.test(path) || !/\.exe$/i.test(path) || /[\x00-\x1f\x7f]/.test(path))return Promise.reject(new Error('Invalid installer path.'));
  return new Promise((resolve,reject)=>{
    let child;
    try {child=spawnImpl(path,['--updated','--force-run'],{detached:true,stdio:'ignore',shell:false,windowsHide:true});}
    catch {reject(new Error('The verified installer could not be started.'));return;}
    child.once('error',()=>reject(new Error('The verified installer could not be started.')));
    child.once('spawn',()=>{child.unref();resolve();});
  });
}
module.exports={launchVerifiedInstaller};
