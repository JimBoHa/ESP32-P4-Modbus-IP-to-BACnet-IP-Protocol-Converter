#!/usr/bin/env node
/** Local browser smoke against a loopback mock of the firmware HTTP contract.
 * Requires Playwright: npm install --no-save playwright && npx playwright install chromium
 * Optional PLAYWRIGHT_MODULE=/absolute/path/to/playwright/index.mjs uses an existing install.
 * Optional CHROMIUM_EXECUTABLE=/absolute/browser/path overrides the bundled browser.
 * Run: node tests/test_web_ui.mjs [artifact-directory]
 * These tests do not establish physical controller communication.
 */
import assert from 'node:assert/strict';
import {createServer} from 'node:http';
import {readFile, mkdir, writeFile} from 'node:fs/promises';
import {resolve, dirname} from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';
const {chromium} = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const output = resolve(process.argv[2] || '/tmp/esp32-p4-web-ui');
await mkdir(output, {recursive:true});
const html = await readFile(resolve(root, 'main/web/index.html'));
const header = 'instance,name,object_type,function,address,data_type,byte_order,scale,offset,units,bit,states,length,poll_ms,description';
const goodCsv = header + '\n1001,Load voltage,AI,3,0,u16,AB,0.1,0,5,,,,1000,Line voltage\n';
const attack = '<img src=x onerror="window.__xss=1">';
const profiles = [
  {id:'mpac1500_full',name:'Kohler MPAC1500 ATS — full',description:'Older Section 13 ATS map; controller verification enabled.',point_count:164},
  {id:'mpac1500_electrical',name:'Kohler MPAC1500 ATS — electrical',description:'24 electrical/status points; same polling and verification.',point_count:24},
  {id:'custom',name:'Custom CSV point map',description:'Use your documented Modbus register map.',point_count:1}
];
const initialConfig = {
  profile:'mpac1500_full',modbus_host:'192.0.2.81',modbus_port:502,modbus_unit:41,
  device_instance:75181,device_name:'Kohler-MPAC1500-Gateway',bacnet_port:47808,
  expected_firmware:515,expected_mac_fragment:0,revision:7,custom_point_count:1,config_error:''
};
const rawPoints = [
  {name:'Source 1 voltage',object_type:0,instance:1001,modbus_offset:3,quality:'good',quality_reason:'',value:488.125},
  {name:'Outage duration',object_type:0,instance:1132,modbus_offset:179,quality:'unverified',quality_reason:'Raw unit interpretation unverified',value:69},
  {name:attack,object_type:40,instance:2001,modbus_offset:251,quality:'communication-failure',quality_reason:attack,value:attack}
];
let config = structuredClone(initialConfig), map = goodCsv, offline = false, rejectSave = false;
const requests = [], posts = [];
const server = createServer(async (req,res) => {
  requests.push({method:req.method,url:req.url});
  res.setHeader('Cache-Control','no-store');
  const send = (status,data) => {res.writeHead(status,{'Content-Type':'application/json'});res.end(JSON.stringify(data));};
  if (req.url === '/' || req.url === '/index.html') {res.writeHead(200,{'Content-Type':'text/html'});res.end(html);return;}
  if (req.url === '/favicon.ico') {res.writeHead(204);res.end();return;}
  if (req.method === 'POST') {
    let raw = ''; for await (const part of req) raw += part;
    let data; try {data=JSON.parse(raw);} catch {send(400,{error:'Malformed JSON'});return;}
    posts.push({url:req.url,data,headers:req.headers});
    if (req.headers['x-gateway-request'] !== '1' || req.headers['content-type'] !== 'application/json') {send(403,{error:'Mutation headers required'});return;}
    if (req.url === '/api/validate') {
      if (data.csv !== goodCsv) {send(400,{ok:false,error:'CSV row 2: invalid object_type ' + attack});return;}
      send(200,{ok:true,point_count:1,points:[{name:attack,object_type:0,instance:1001}]});return;
    }
    if (req.url === '/api/config') {
      if (rejectSave || data.revision !== config.revision) {send(409,{error:'Configuration changed; reload the page before saving.'});return;}
      const {csv,...fields} = data;
      config = {...config,...fields,revision:config.revision+1};
      if (csv !== undefined) map=csv;
      send(200,{ok:true,restart:true,revision:config.revision});return;
    }
  }
  if (req.url === '/api/config') {send(200,config);return;}
  if (req.url === '/api/profiles') {send(200,profiles);return;}
  if (req.url === '/api/status') {
    if (offline) {send(503,{error:'Mock connection unavailable'});return;}
    send(200,{
      profile:config.profile,firmware:'2.0.0-test',board:'Waveshare ESP32-P4-POE-ETH',uptime_seconds:321,
      ethernet_up:true,profile_verified:true,profile_status:'Controller identity verified',
      modbus_host:config.modbus_host,modbus_port:config.modbus_port,modbus_unit:config.modbus_unit,
      modbus_requests:901,modbus_successful_responses:900,modbus_failures:1,
      last_modbus_response_age_seconds:0.2,bacnet_device_instance:config.device_instance,
      device_name:config.device_name,bacnet_port:config.bacnet_port,bacnet_received_packets:456,
      good_points:1,fault_points:2,point_count:3
    });return;
  }
  if (req.url === '/api/points') {send(200,rawPoints);return;}
  if (req.url === '/api/map.csv' || req.url === '/api/template.csv') {res.writeHead(200,{'Content-Type':'text/csv'});res.end(map);return;}
  send(404,{error:'Unknown mock endpoint'});
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const origin = `http://127.0.0.1:${server.address().port}`;
const browser = await chromium.launch({headless:true, ...(process.env.CHROMIUM_EXECUTABLE ? {executablePath:process.env.CHROMIUM_EXECUTABLE} : {})});
const page = await browser.newPage({viewport:{width:1365,height:1000},deviceScaleFactor:1});
const errors = [], foreignRequests = [];
page.on('pageerror',e=>errors.push(e.message));
page.on('request',req=>{if (!req.url().startsWith(origin)) foreignRequests.push(req.url());});
const tests = [];
async function check(name, fn) {await fn();tests.push(name);console.log('PASS '+name);}
async function waitFor(fn, message, timeout=10000) {
  const deadline=Date.now()+timeout;
  while(Date.now()<deadline) {if(await fn()) return;await new Promise(r=>setTimeout(r,25));}
  throw new Error(message);
}
const text = id=>page.locator('#'+id).textContent();
const enabled = id=>page.locator('#'+id).isEnabled();
const file = (name,contents)=>page.locator('#csvFile').setInputFiles({name,mimeType:'text/csv',buffer:Buffer.from(contents)});
const savedPosts = ()=>posts.filter(p=>p.url==='/api/config');
try {
  await page.goto(origin);
  await waitFor(async()=>await text('connectionLabel')==='Connected','Initial status did not load');
  await check('loads runtime, full profile, three live points, and safe API text',async()=>{
    assert.equal(await text('sourceHost'),'192.0.2.81');
    assert.equal(await text('deviceInstance'),'75181');
    assert.equal(await page.locator('#pointsBody tr').count(),3);
    assert.equal(await page.locator('#pointsBody img').count(),0);
    assert.equal(await page.evaluate(()=>window.__xss),undefined);
    assert.ok((await text('pointsBody')).includes(attack));
    assert.equal(await page.locator('#profile').inputValue(),'mpac1500_full');
    await page.screenshot({path:resolve(output,'desktop-overview.png'),fullPage:true});
  });
  await check('search and quality/type filters select actual points',async()=>{
    await page.locator('#pointsTab').click();
    await page.locator('#qualityFilter').selectOption('fault');
    assert.equal(await page.locator('#pointsBody tr').count(),2);
    await page.locator('#pointSearch').fill('1132');
    assert.equal(await page.locator('#pointsBody tr').count(),1);
    assert.ok((await text('pointsBody')).includes('69'));
    await page.locator('#pointSearch').fill('');
    await page.locator('#typeFilter').selectOption('CSV');
    assert.equal(await page.locator('#pointsBody tr').count(),1);
    await page.locator('#qualityFilter').selectOption('all');
    await page.locator('#typeFilter').selectOption('all');
  });
  await check('connection loss retains values with paused warning and recovers',async()=>{
    offline=true;await page.locator('#refreshButton').click();
    await waitFor(async()=>(await text('connectionLabel'))==='Connection lost','No offline warning');
    assert.equal(await page.locator('#pointsBody tr').count(),3);
    assert.equal(await text('pointsLiveBadge'),'Updates paused');
    offline=false;await page.locator('#refreshButton').click();
    await waitFor(async()=>(await text('connectionLabel'))==='Connected','Did not recover');
  });
  await check('profile selection and refresh preserve unsaved settings',async()=>{
    await page.locator('#configTab').click();
    await page.locator('#profile').selectOption('mpac1500_electrical');
    assert.equal(await text('profileCount'),'24 points');
    await page.locator('#device_name').fill('Site-ATS');
    await page.locator('#refreshButton').click();
    await waitFor(()=>enabled('refreshButton'),'Refresh did not finish');
    assert.equal(await page.locator('#device_name').inputValue(),'Site-ATS');
    assert.equal(await page.locator('#profile').inputValue(),'mpac1500_electrical');
    await page.locator('#resetButton').click();
    assert.equal(await page.locator('#device_name').inputValue(),initialConfig.device_name);
    await page.screenshot({path:resolve(output,'desktop-configuration.png'),fullPage:true});
  });
  await check('invalid CSV error is literal and blocks saving, including oversized replacement',async()=>{
    await page.locator('#profile').selectOption('custom');
    await file('invalid.csv','invalid header');
    await waitFor(async()=>(await text('mapMessage')).includes('CSV row 2'),'Validation error not shown');
    assert.equal(await enabled('saveButton'),false);
    assert.equal(await page.locator('#mapMessage img').count(),0);
    assert.equal(savedPosts().length,0);
    await file('too-big.csv','x'.repeat(32769));
    await page.locator('#device_name').fill('New-name');
    assert.equal(await enabled('saveButton'),false);
    assert.ok((await text('mapMessage')).includes('32 KiB'));
    await page.locator('#clearFileButton').click();
    assert.equal(await enabled('saveButton'),true);
  });
  await check('valid CSV previews safely; Save sends exact config, revision, CSV, and headers',async()=>{
    await file('valid.csv',goodCsv);
    await waitFor(async()=>(await text('mapBadge'))==='Validated','CSV did not validate');
    assert.equal(await page.locator('#previewBody tr').count(),1);
    assert.equal(await page.locator('#previewBody img').count(),0);
    assert.ok((await text('previewBody')).includes(attack));
    await page.locator('#modbus_unit').fill('255');
    await page.locator('#saveButton').click();
    await waitFor(()=>savedPosts().length===1,'Save request missing');
    const saved=savedPosts()[0];
    assert.deepEqual(saved.data,{
      profile:'custom',modbus_host:initialConfig.modbus_host,modbus_port:502,modbus_unit:255,
      device_instance:75181,device_name:'New-name',bacnet_port:47808,expected_firmware:515,
      expected_mac_fragment:0,revision:7,csv:goodCsv
    });
    assert.equal(saved.headers['x-gateway-request'],'1');
    assert.equal(saved.headers['content-type'],'application/json');
    assert.equal(await enabled('saveButton'),false);
    await waitFor(async()=>(await text('saveMessage')).includes('Gateway is responding'),'Restart reconnect missing');
    assert.equal(await text('revisionValue'),'8');
    assert.equal(await page.locator('#csvFile').inputValue(),'');
    assert.equal(await enabled('saveButton'),true);
  });
  await check('loaded custom map is retained when only unit 0 changes',async()=>{
    await page.locator('#modbus_unit').fill('0');
    await page.locator('#saveButton').click();
    await waitFor(()=>savedPosts().length===2,'Second save missing');
    assert.equal(savedPosts()[1].data.modbus_unit,0);
    assert.equal(savedPosts()[1].data.revision,8);
    assert.equal(Object.hasOwn(savedPosts()[1].data,'csv'),false);
    assert.equal(map,goodCsv);
    await waitFor(async()=>(await text('revisionValue'))==='9','Second restart missing');
  });
  await check('409 conflict retains form edits and reports server error',async()=>{
    rejectSave=true;
    await page.locator('#device_name').fill('Edited-before-conflict');
    await page.locator('#saveButton').click();
    await waitFor(async()=>(await text('saveMessage')).includes('Configuration changed'),'Conflict message missing');
    assert.equal(await page.locator('#device_name').inputValue(),'Edited-before-conflict');
    assert.equal(await enabled('saveButton'),true);
    assert.equal(config.revision,9);
    rejectSave=false;
    await page.locator('#resetButton').click();
  });
  await check('invalid IPv4/name validation clears after corrected input',async()=>{
    const before=savedPosts().length;
    await page.locator('#modbus_host').fill('300.1.2.3');
    await page.locator('#saveButton').click();
    assert.equal(savedPosts().length,before);
    assert.ok(await page.locator('#modbus_host').evaluate(el=>el.validationMessage.length>0));
    await page.locator('#modbus_host').fill('192.0.2.81');
    assert.equal(await page.locator('#modbus_host').evaluate(el=>el.validationMessage),'');
    await page.locator('#device_name').fill('Gateway-reserved');
    await page.locator('#saveButton').click();
    assert.equal(savedPosts().length,before);
    assert.ok(await page.locator('#device_name').evaluate(el=>el.validationMessage.length>0));
    await page.locator('#resetButton').click();
    assert.equal(await page.locator('#device_name').evaluate(el=>el.validationMessage),'');
  });
  await check('mobile layout has no page overflow; tabs support keyboard navigation',async()=>{
    await page.setViewportSize({width:390,height:844});
    await page.locator('#profile').selectOption('custom');
    await file('valid.csv',goodCsv);
    await waitFor(async()=>(await text('mapBadge'))==='Validated','Mobile validation missing');
    assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth <= window.innerWidth),true);
    await page.screenshot({path:resolve(output,'mobile-configuration.png'),fullPage:true});
    await page.locator('#configTab').focus();
    await page.keyboard.press('ArrowRight');
    assert.equal(await page.locator('#pointsTab').getAttribute('aria-selected'),'true');
    assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth <= window.innerWidth),true);
    await page.screenshot({path:resolve(output,'mobile-points.png'),fullPage:true});
    await page.locator('#configTab').click();await page.locator('#resetButton').click();
  });
  await check('stored config error remains repairable and visible',async()=>{
    config.config_error='Saved CSV invalid; acquisition disabled.';
    await page.reload();
    await waitFor(async()=>(await text('connectionLabel'))==='Connected','Reload failed');
    assert.ok((await text('globalMessage')).includes(config.config_error));
    offline=true;await page.locator('#refreshButton').click();
    await waitFor(async()=>(await text('connectionLabel'))==='Connection lost','No config-error offline warning');
    offline=false;await page.locator('#refreshButton').click();
    await waitFor(async()=>(await text('connectionLabel'))==='Connected','No config-error recovery');
    assert.ok((await text('globalMessage')).includes(config.config_error));
    await page.locator('#configTab').click();
    assert.equal(await page.locator('#configFields').isDisabled(),false);
    await page.locator('#profile').selectOption('mpac1500_full');
    assert.equal(await enabled('saveButton'),true);
  });
  assert.deepEqual(errors,[],'Browser JavaScript errors');
  assert.deepEqual(foreignRequests,[],'Page made external requests');
  await writeFile(resolve(output,'report.json'),JSON.stringify({ok:true,tests,requests:requests.length,mutations:posts.map(p=>({url:p.url,revision:p.data.revision})),javascript_errors:errors,external_requests:foreignRequests},null,2)+'\n');
  console.log(`PASS ${tests.length} scenarios; artifacts: ${output}`);
} catch(error) {
  await page.screenshot({path:resolve(output,'failure.png'),fullPage:true});
  throw error;
} finally {
  await browser.close();await new Promise(resolve=>server.close(resolve));
}
