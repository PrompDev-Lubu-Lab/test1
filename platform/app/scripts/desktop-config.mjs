import {createRequire} from 'node:module';
import {APP_NAME,APP_SLUG,APP_VERSION} from '../public/config.js';
const require=createRequire(import.meta.url);
const {validateSettings}=require('../desktop/policy.cjs');
export function desktopBuildConfiguration(input,{review=false}={}) {
  const settings=validateSettings(input);
  if(review ? settings.channel!=='review' || settings.updatesEnabled : settings.channel!=='production' || !settings.updatesEnabled)throw new Error('The build channel and configured update policy must agree.');
  return {
    settings,
    config:{
      appId:review?'ai.clawdie.platform.review':'ai.clawdie.platform',productName:review?`${APP_NAME} Development`:APP_NAME,
      executableName:review?`${APP_SLUG}-development`:APP_SLUG,asar:true,electronVersion:'44.2.0',npmRebuild:false,
      extraMetadata:{main:'desktop/main.cjs',version:APP_VERSION},
      directories:{output:review?'release/review':'release/production'},
      files:['desktop/*.cjs','desktop/offline.html','desktop/settings.generated.json','public/config.js','licenses/**','source-manifest.json','package.json'],
      electronFuses:{runAsNode:false,enableCookieEncryption:true,enableNodeOptionsEnvironmentVariable:false,enableNodeCliInspectArguments:false,enableEmbeddedAsarIntegrityValidation:true,onlyLoadAppFromAsar:true,grantFileProtocolExtraPrivileges:false},
      forceCodeSigning:!review,
      artifactName:review?`${APP_SLUG}-\${version}-win-x64-UNSIGNED-DEVELOPMENT.exe`:`${APP_SLUG}-\${version}-win-x64.exe`,
      win:{target:[{target:'nsis',arch:['x64']}],verifyUpdateCodeSignature:true,...(review?{signAndEditExecutable:true}:{signtoolOptions:{publisherName:settings.publisherNames,signingHashAlgorithms:['sha256']}})},
      nsis:{oneClick:true,perMachine:false,allowElevation:false,packElevateHelper:false,runAfterFinish:true},
      publish:review?null:[{provider:'generic',url:settings.feedUrl,channel:'latest'}]
    }
  };
}
