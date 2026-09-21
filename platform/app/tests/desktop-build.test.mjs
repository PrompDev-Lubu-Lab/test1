import test from 'node:test';
import assert from 'node:assert/strict';
import {desktopBuildConfiguration} from '../scripts/desktop-config.mjs';
import {APP_NAME} from '../public/config.js';
const review={schema:1,channel:'review',appOrigin:'https://app.example.test',accessOrigin:'https://example.cloudflareaccess.com',externalOrigins:[],updatesEnabled:false,publisherNames:[],certificateThumbprints:[]};
test('review build has an unmistakable label and no feed; packaged policy is inside ASAR',()=>{
  const {config}=desktopBuildConfiguration(review,{review:true});
  assert.equal(config.productName,APP_NAME+' Development');assert.equal(config.publish,null);
  assert.match(config.artifactName,/UNSIGNED-DEVELOPMENT/);assert.ok(config.files.includes('desktop/settings.generated.json'));
  assert.equal(config.electronFuses.enableCookieEncryption,true);assert.equal(config.electronFuses.onlyLoadAppFromAsar,true);
  assert.equal(config.electronFuses.enableEmbeddedAsarIntegrityValidation,true);
  assert.equal(config.electronFuses.runAsNode,false);assert.equal(config.nsis.packElevateHelper,false);assert.equal(config.nsis.perMachine,false);assert.equal(config.nsis.allowElevation,false);
  assert.throws(()=>desktopBuildConfiguration(review));
});
test('production build fails closed without pins and uses only a protected generic feed',()=>{
  const production={...review,channel:'production',appOrigin:'https://workspace.example.com',updatesEnabled:true,publisherNames:['Example Publisher'],certificateThumbprints:['A'.repeat(40),'B'.repeat(40)]};
  assert.throws(()=>desktopBuildConfiguration({...production,certificateThumbprints:[]}));
  const {config}=desktopBuildConfiguration(production);assert.equal(config.forceCodeSigning,true);assert.equal(config.win.verifyUpdateCodeSignature,true);
  assert.deepEqual(config.win.signtoolOptions.publisherName,['Example Publisher']);assert.equal(config.publish[0].url,production.appOrigin+'/api/updates/windows/x64/');
});
