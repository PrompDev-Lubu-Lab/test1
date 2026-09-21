'use strict';
const {join,resolve} = require('node:path');
const {homedir} = require('node:os');
const {createUpdateTransport} = require('./transport.cjs');
const {validateUpdateInfo} = require('./policy.cjs');
const {verifyWindowsInstaller} = require('./signature.cjs');
const {launchVerifiedInstaller} = require('./install.cjs');
async function createDesktopUpdater({app,appSession,settings,dialog,window,changed=()=>{},platform=process.platform,makeUpdater,verifyInstaller=verifyWindowsInstaller,launchInstaller=launchVerifiedInstaller}) {
  if(!app.isPackaged || platform!=='win32' || !settings.updatesEnabled || settings.channel!=='production') return {enabled:false,check:async()=>{},dispose(){}};
  // Test injection is a main-process construction option; there is no renderer IPC.
  const updater=makeUpdater?makeUpdater():new (require('electron-updater').NsisUpdater)();
  updater.autoDownload=false;updater.autoInstallOnAppQuit=false;updater.allowDowngrade=false;updater.allowPrerelease=false;
  updater.disableWebInstaller=true;updater.disableDifferentialDownload=true;updater.autoRunAppAfterInstall=true;
  updater.logger={info(){},warn(){},error(){},debug(){}};
  // Pin to the audited 6.8.9 config contract; missing publishers must not skip verification.
  const config=await updater.configOnDisk.value;
  const publishers=Array.isArray(config.publisherName)?config.publisherName:[config.publisherName];
  if(config.provider!=='generic' || config.url!==settings.feedUrl || config.updaterCacheDirName!==settings.cacheName || publishers.length!==settings.publisherNames.length || publishers.some(name=>!settings.publisherNames.includes(name))) throw new Error('The packaged update policy does not match the signing policy.');
  const helper=await updater.getOrCreateDownloadHelper();
  const expectedCache=resolve(process.env.LOCALAPPDATA || join(homedir(),'AppData','Local'),settings.cacheName);
  if(resolve(helper.cacheDir)!==expectedCache)throw new Error('The update cache is not the configured local directory.');
  let expectedRelease=null;
  const verify=path=>expectedRelease?verifyInstaller({path,allowedRoot:expectedCache,publisherNames:settings.publisherNames,certificateThumbprints:settings.certificateThumbprints,expectedSha512:expectedRelease.sha512,expectedSize:expectedRelease.size}):Promise.resolve('No verified release metadata.');
  updater.verifyUpdateCodeSignature=async(names,path)=>{
    if(names.length!==settings.publisherNames.length || names.some(name=>!settings.publisherNames.includes(name)))return 'Publisher policy mismatch.';
    return verify(path);
  };
  let cancellation=null,busy=false,disposed=false;
  const transport=createUpdateTransport({appSession,updateSession:updater.netSession,settings,onInvalidated:()=>cancellation?.cancel()});
  updater.on('error',()=>{}); // Awaited check/download errors are handled by their own operation.
  async function check() {
    if(busy || disposed)return;
    busy=true;changed(true);let timer=null,checking=false;
    try {
      await transport.begin();
      const result=await updater.checkForUpdates();
      if(!result?.isUpdateAvailable) {await dialog.showMessageBox(window,{type:'info',message:'No newer desktop release is available.'});return;}
      const release=validateUpdateInfo(result.updateInfo,settings,app.getVersion());
      expectedRelease=release;
      cancellation=result.cancellationToken;
      const operationCancellation=cancellation;
      const answer=await dialog.showMessageBox(window,{type:'question',message:`Download desktop ${release.version}?`,detail:'The installer must match the approved publisher and signing certificate. Installation requires a second confirmation.',buttons:['Cancel','Download'],defaultId:0,cancelId:0,noLink:true});
      if(answer.response!==1)return;
      await transport.validate();transport.permitArtifact(release.file);
      timer=setInterval(()=>{if(checking)return;checking=true;void transport.validate().catch(()=>operationCancellation.cancel()).finally(()=>{checking=false;});},10000);
      const paths=await updater.downloadUpdate(cancellation);
      clearInterval(timer);timer=null;
      if(paths.length!==1 || resolve(paths[0])!==resolve(helper.file ?? ''))throw new Error('Unexpected installer path.');
      await transport.validate();
      if(await verify(paths[0])!==null)throw new Error('Installer verification failed.');
      const install=await dialog.showMessageBox(window,{type:'question',message:`Install desktop ${release.version} and restart?`,detail:'Save any unfinished work first. Closing this dialog leaves the current version installed.',buttons:['Later','Install and restart'],defaultId:0,cancelId:0,noLink:true});
      if(install.response!==1)return;
      const receipt=await transport.validate();
      // Cache hits skip the native download verifier, so recheck immediately before install.
      if(await verify(paths[0])!==null)throw new Error('Installer verification failed.');
      if(disposed || !transport.isCurrent(receipt) || resolve(helper.file ?? '')!==resolve(paths[0]))throw new Error('Installer or account state changed.');
      await launchInstaller(paths[0]);
      app.quit();
    } catch (error) {
      const authentication=error?.code==='UPDATE_AUTHENTICATION_REQUIRED';
      if(!disposed)await dialog.showMessageBox(window,{type:'warning',message:authentication?'Your sign-in could not be confirmed.':'The update could not be verified or downloaded.',detail:authentication?'Check your connection and sign in to the app, then use Check for updates. The current version is unchanged.':'The current version is unchanged. Check your connection and try again. A release with an unapproved signature cannot be installed.'});
    } finally {if(timer)clearInterval(timer);transport.invalidate();cancellation=null;expectedRelease=null;busy=false;if(!disposed)changed(false);}
  }
  return {enabled:true,check,dispose(){disposed=true;transport.dispose();}};
}
module.exports={createDesktopUpdater};
