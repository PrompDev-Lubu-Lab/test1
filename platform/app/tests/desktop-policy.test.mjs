import test from 'node:test';
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
const require = createRequire(import.meta.url);
const {validateSettings,allowedNavigation,allowedExternal,allowedUpdateUrl,sessionCookies,validateUpdateInfo} = require('../desktop/policy.cjs');
const input = {schema:1,channel:'review',appOrigin:'https://app.example.test',accessOrigin:'https://example.cloudflareaccess.com',externalOrigins:['https://help.example.test'],updatesEnabled:false,publisherNames:[],certificateThumbprints:[]};
const settings = validateSettings(input);
test('desktop settings close updates without signing and reject unsafe origins', () => {
  for (const change of [{appOrigin:'http://app.example.test'},{appOrigin:'https://app.example.test/'},{appOrigin:'https://app.example.test:8443'},{accessOrigin:'https://imposter.test'},{updatesEnabled:true},{channel:'production'}]) assert.throws(()=>validateSettings({...input,...change}));
  assert.equal(settings.updatesEnabled,false);
});
test('human navigation accepts the app and exact login routes only', () => {
  for (const url of [settings.appOrigin,settings.accessOrigin+'/cdn-cgi/access/login','https://github.com/login/oauth/authorize','https://github.com/sessions/two-factor']) assert.equal(allowedNavigation(url,settings),true,url);
  for (const url of ['file:///private','javascript:alert(1)','https://github.com/organization/repo',settings.accessOrigin+'/other','https://app.example.test.evil.test','https://user:pass@app.example.test']) assert.equal(allowedNavigation(url,settings),false,url);
  assert.equal(allowedExternal('https://help.example.test/manual',settings),true);
  assert.equal(allowedExternal('https://other.example.test',settings),false);
});
test('update transport allows a single fixed origin and strict feed/artifact filenames', () => {
  assert.equal(allowedUpdateUrl(settings.feedUrl+'latest.yml?noCache=abc123',settings),true);
  assert.equal(allowedUpdateUrl(settings.feedUrl+'clawdie-platform-0.1.1-win-x64.exe',settings),true);
  for (const suffix of ['../latest.yml','%2e%2e/latest.yml','latest.yml?token=secret','latest.yml#fragment','clawdie-platform-01.1.1-win-x64.exe','anything.exe','latest.yml?noCache=a&noCache=b']) assert.equal(allowedUpdateUrl(settings.feedUrl+suffix,settings),false,suffix);
  assert.equal(allowedUpdateUrl('https://other.example.test'+new URL(settings.feedUrl).pathname+'latest.yml',settings),false);
});
const cookies = () => [{name:'CF_Authorization',value:'synthetic.jwt.value',secure:true,httpOnly:true,path:'/',domain:'app.example.test',hostOnly:true},{name:'__Host-platform-session',value:'syntheticSession123',secure:true,httpOnly:true,path:'/',domain:'app.example.test',hostOnly:true}];
test('cookie bridge picks only current required cookies and rejects duplicate/expired/foreign app sessions', () => {
  const selected = sessionCookies([...cookies(),{name:'analytics',value:'private'}],settings.appOrigin);
  assert.equal(selected.header,'CF_Authorization=synthetic.jwt.value; __Host-platform-session=syntheticSession123');
  for (const values of [cookies().slice(1),[...cookies(),cookies()[0]],cookies().map(c=>({...c,expirationDate:1})),cookies().map(c=>({...c,secure:false})),cookies().map(c=>({...c,hostOnly:false})),cookies().map(c=>({...c,value:'bad; injected=1'}))]) assert.throws(()=>sessionCookies(values,settings.appOrigin));
});
test('update metadata rejects downgrade, foreign artifacts, mismatched checksum and web installers', () => {
  const info = {version:'0.1.1',files:[{url:'clawdie-platform-0.1.1-win-x64.exe',size:100,sha512:Buffer.alloc(64,1).toString('base64')}]};
  assert.equal(validateUpdateInfo(info,settings,'0.1.0').version,'0.1.1');
  for (const candidate of [{...info,version:'0.1.0'},{...info,version:'0.1.1-beta'},{...info,packages:{}},{...info,files:[{...info.files[0],url:'https://other.example.test/setup.exe'}]},{...info,path:'other.exe'},{...info,sha512:'wrong'}]) assert.throws(()=>validateUpdateInfo(candidate,settings,'0.1.0'));
});
