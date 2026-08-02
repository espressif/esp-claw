/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Web control UI — adapted from ai-rover (same dark theme, joystick, WiFi settings)
 */
#include "wave_rover_mcp_web.h"
#include "wave_rover_hal.h"
#include "wave_rover_config.h"
#include "wave_rover_mcp_state.h"
#include "wave_rover_power_mgr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char *TAG = "wr_web";

static const wave_rover_config_t *s_web_cfg     = NULL;
static volatile uint32_t          s_web_deadline = 0;  /* ms; 0 = no active web motion */
static TimerHandle_t              s_web_timer    = NULL;
static volatile float             s_cached_bat_v = 0.0f; /* updated by sensor task, never by httpd */
static wr_power_mgr_handle_t      s_web_power_mgr = NULL;

void wr_mcp_web_set_power_mgr(wr_power_mgr_handle_t pm) { s_web_power_mgr = pm; }

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ------------------------------------------------------------------ */
/* Auth — mirrors mcp_post_handler check; both use the same s_web_cfg  */
/* ------------------------------------------------------------------ */

/* Constant-time comparison to avoid timing side-channel on token */
static bool ct_streq(const char *a, const char *b)
{
    if (!a || !b) return false;
    size_t la = strlen(a), lb = strlen(b);
    volatile uint8_t diff = (uint8_t)(la ^ lb);
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

#define AUTH_HDR_BUF 96

static bool web_check_auth(httpd_req_t *req)
{
    if (!s_web_cfg || !s_web_cfg->auth_enabled) return true;
    if (s_web_cfg->auth_token[0] == '\0')       return true;
    char buf[AUTH_HDR_BUF] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization",
                                    buf, sizeof(buf)) != ESP_OK) return false;
    if (strncmp(buf, "Bearer ", 7) != 0) return false;
    return ct_streq(buf + 7, s_web_cfg->auth_token);
}

#define WEB_REQUIRE_AUTH(req) do { \
    if (!web_check_auth(req)) { \
        httpd_resp_set_status(req, "401 Unauthorized"); \
        httpd_resp_set_type(req, "application/json"); \
        return httpd_resp_sendstr(req, \
            "{\"ok\":false,\"error\":\"unauth\"}"); \
    } \
} while (0)

static void get_ip(char *buf, size_t len)
{
    /* Try STA first */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr) {
            snprintf(buf, len, IPSTR, IP2STR(&info.ip));
            return;
        }
    }
    /* Fall back to AP */
    netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr) {
            snprintf(buf, len, IPSTR, IP2STR(&info.ip));
            return;
        }
    }
    strlcpy(buf, "0.0.0.0", len);
}

static bool is_sta_connected(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return false;
    esp_netif_ip_info_t info;
    return (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0);
}

/* Watchdog: stop motors if browser disconnects / joystick goes stale */
static void web_watchdog_cb(TimerHandle_t t)
{
    (void)t;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t dl  = s_web_deadline;
    if (dl && (int32_t)(now - dl) >= 0) {
        s_web_deadline = 0;
        wr_motor_stop();
    }
}

/* ------------------------------------------------------------------ */
/* HTML page (adapted from ai-rover: dark theme, joystick, settings)  */
/* ------------------------------------------------------------------ */

static const char s_html[] =
    "<!doctype html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1,user-scalable=no'>"
    "<title>Wave Rover</title>"
    "<style>"
    "*{box-sizing:border-box}"
    "body{font-family:system-ui,-apple-system,sans-serif;background:#0b1220;color:#e5e7eb;"
    "margin:0;padding:12px;touch-action:manipulation}"
    "h1{font-size:18px;margin:0 0 10px}h2{font-size:15px;margin:14px 0 6px}"
    ".card{background:#111827;border:1px solid #1f2937;border-radius:10px;padding:12px;margin-bottom:10px}"
    ".row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}"
    "button{background:#1f2937;color:#e5e7eb;border:1px solid #374151;border-radius:8px;"
    "padding:10px 14px;font-size:14px;cursor:pointer;flex:1;min-width:60px}"
    "button:active{background:#374151}"
    ".danger{background:#7f1d1d;border-color:#991b1b}"
    ".tabs{margin-bottom:10px}"
    ".tab-btn.active{background:#2563eb;border-color:#1d4ed8}"
    ".chk{display:flex;align-items:center;gap:8px;margin-top:8px}"
    ".chk input{width:auto;margin:0}"
    ".pill{display:inline-block;padding:3px 10px;border-radius:12px;font-size:12px;font-weight:600}"
    "input,select{width:100%;background:#0f172a;color:#e5e7eb;border:1px solid #334155;"
    "border-radius:8px;padding:8px;font-size:14px;margin-top:4px}"
    "label{display:block;font-size:13px;color:#9ca3af;margin-top:8px}"
    ".muted{opacity:.7;font-size:12px}"
    "#joyWrap{width:180px;margin:0 auto}"
    "canvas{display:block;border-radius:50%;background:#0f172a}"
    ".spd-row{display:flex;align-items:center;gap:8px;margin-top:8px}"
    ".spd-row input{flex:1;margin:0;accent-color:#2563eb}"
    "</style></head><body>"
    "<h1>Wave Rover</h1>"
    /* Tab nav */
    "<div class='row tabs'>"
    "<button class='tab-btn active' id='tabBtnControl' onclick=\"showTab('control')\">Control</button>"
    "<button class='tab-btn' id='tabBtnSettings' onclick=\"showTab('settings')\">Settings</button>"
    "<button class='tab-btn' id='tabBtnPower' onclick=\"showTab('power')\">Power</button>"
    "</div>"
    "<div id='tabControl'>"
    /* Status */
    "<div class='card'>"
    "<div class='row' style='justify-content:space-between'>"
    "<span class='pill' id='ePill' style='background:#2d8b2d'>IDLE</span>"
    "<span class='muted' id='sBat'>--</span>"
    "</div></div>"
    /* Drive */
    "<div class='card'><h2>Drive</h2>"
    "<div id='joyWrap'><canvas id='joy' width='180' height='180'></canvas></div>"
    "<div class='spd-row'><span class='muted'>Speed</span>"
    "<input type='range' id='spd' min='10' max='100' value='80'>"
    "<span id='spdV' class='muted'>80%</span></div>"
    "<div class='row' style='margin-top:8px'>"
    "<button onmousedown=\"hs('rotate_left')\" onmouseup='hx()'"
    " ontouchstart=\"hs('rotate_left')\" ontouchend='hx()'>&#8634; Left</button>"
    "<button class='danger' onclick=\"cmd('stop')\">STOP</button>"
    "<button onmousedown=\"hs('rotate_right')\" onmouseup='hx()'"
    " ontouchstart=\"hs('rotate_right')\" ontouchend='hx()'>Right &#8635;</button>"
    "</div>"
    "<div class='row' style='margin-top:8px'>"
    "<button class='danger' onclick=\"cmd('estop')\">E-STOP</button>"
    "<button onclick=\"cmd('clear_estop')\">Clear ESTOP</button>"
    "</div></div>"
    "</div>"  /* end tabControl */
    "<div id='tabSettings' style='display:none'>"
    /* Limits */
    "<div class='card'><h2>Limits</h2>"
    "<label>Max speed (%)<input type='number' id='lMaxSpd' min='1' max='100'></label>"
    "<label>Max command duration (ms)<input type='number' id='lMaxDur' min='1' max='30000'></label>"
    "</div>"
    /* Syslog */
    "<div class='card'><h2>Syslog</h2>"
    "<div class='chk'><input type='checkbox' id='sysEn'>"
    "<label style='margin:0'>Forward logs over UDP</label></div>"
    "<label>Address (blank = subnet broadcast)<input type='text' id='sysHost' placeholder='e.g. 192.168.1.50'></label>"
    "<label>Port<input type='number' id='sysPort' min='1' max='65535'></label>"
    "<label>Facility (0-23)<input type='number' id='sysFac' min='0' max='23'></label>"
    "</div>"
    /* OTA update */
    "<div class='card'><h2>Firmware Update</h2>"
    "<div class='row'>"
    "<input type='file' id='fwFile' accept='.bin' style='flex:2'>"
    "<button id='fwBtn' onclick='otaFlash()'>Flash</button>"
    "</div>"
    "<div class='muted' id='fwSt'></div>"
    "</div>"
    /* WiFi settings */
    "<div class='card'><h2>Wi-Fi</h2>"
    "<label>Mode<select id='wMode' onchange='mChg()'>"
    "<option value='0'>AP (hotspot)</option>"
    "<option value='1'>STA (home network)</option>"
    "<option value='2'>AP + STA</option>"
    "</select></label>"
    "<div id='apF'>"
    "<label>AP name<input type='text' id='apSSID'></label>"
    "<label>AP password<input type='password' id='apPW' placeholder='blank=keep'></label>"
    "</div>"
    "<div id='staF'>"
    "<label>Network<select id='stSSID'></select></label>"
    "<label>Password<input type='password' id='stPW' placeholder='blank=keep'></label>"
    "</div>"
    "<div class='row' style='margin-top:10px'>"
    "<button onclick='wScan()'>Rescan</button>"
    "<button onclick='wSave()'>Save</button>"
    "<button class='danger' onclick='wReset()'>Reset</button>"
    "</div>"
    "<div class='muted' id='wInfo' style='margin-top:6px'>loading...</div>"
    "</div>"
    "</div>"  /* end tabSettings */
    "<div id='tabPower' style='display:none'>"
    "<div class='card'><h2>Power Mode</h2>"
    "<div class='row' style='justify-content:space-between;margin-bottom:8px'>"
    "<span>Mode:</span><span class='pill' id='pwMode' style='background:#374151'>--</span>"
    "</div>"
    "<div class='row' style='justify-content:space-between;margin-bottom:4px'>"
    "<span class='muted'>Battery</span><span id='pwBat'>--</span>"
    "</div>"
    "<div class='row' style='justify-content:space-between;margin-bottom:8px'>"
    "<span class='muted'>Last activity</span><span id='pwAct'>--</span>"
    "</div>"
    "<div class='row'>"
    "<button onclick=\"setMode('ACTIVE')\">ACTIVE</button>"
    "<button onclick=\"setMode('IDLE')\">IDLE</button>"
    "<button onclick=\"setMode('LOW_POWER')\">LOW POWER</button>"
    "</div>"
    "<div class='muted' id='pwErr' style='margin-top:6px'></div>"
    "</div></div>"  /* end tabPower */
    /* Script */
    "<script>"
    /* joystick */
    "const CV=document.getElementById('joy'),cx=CV.getContext('2d');"
    "const R=90,DR=30;"
    "let jx=0,jy=0,jD=false,jT=0,hA='',hT=0,selW='';"
    "const gs=()=>parseInt(document.getElementById('spd').value);"
    "document.getElementById('spd').oninput=function(){document.getElementById('spdV').textContent=this.value+'%'};"
    "function dJ(){"
    "cx.clearRect(0,0,180,180);"
    "cx.beginPath();cx.arc(R,R,R-2,0,Math.PI*2);"
    "cx.fillStyle='#1f2937';cx.fill();cx.strokeStyle='#374151';cx.lineWidth=2;cx.stroke();"
    "cx.beginPath();cx.moveTo(R,15);cx.lineTo(R,165);cx.moveTo(15,R);cx.lineTo(165,R);"
    "cx.strokeStyle='#374151';cx.lineWidth=1;cx.stroke();"
    "let dx=jx*(R-DR)/100,dy=-jy*(R-DR)/100;"
    "cx.beginPath();cx.arc(R+dx,R+dy,DR,0,Math.PI*2);"
    "cx.fillStyle=jD?'#2563eb':'#4b5563';cx.fill();"
    "cx.strokeStyle='#60a5fa';cx.lineWidth=2;cx.stroke();}"
    "function jP(e){"
    "const rc=CV.getBoundingClientRect();"
    "let t=e.touches?e.touches[0]:e;"
    "let px=t.clientX-rc.left-R,py=t.clientY-rc.top-R;"
    "let d=Math.sqrt(px*px+py*py),mx=R-DR;"
    "if(d>mx){px=px/d*mx;py=py/d*mx;}"
    "jx=Math.round(px/mx*100);jy=Math.round(-py/mx*100);dJ();}"
    "function jS(e){e.preventDefault();jD=true;jP(e);if(!jT)jT=setInterval(jX,100);}"
    "function jM(e){e.preventDefault();if(jD)jP(e);}"
    "function jE(e){e.preventDefault();jD=false;jx=0;jy=0;dJ();jX();"
    "if(jT){clearInterval(jT);jT=0;}}"
    "CV.addEventListener('mousedown',jS);CV.addEventListener('mousemove',jM);"
    "CV.addEventListener('mouseup',jE);CV.addEventListener('mouseleave',jE);"
    "CV.addEventListener('touchstart',jS,{passive:false});"
    "CV.addEventListener('touchmove',jM,{passive:false});"
    "CV.addEventListener('touchend',jE,{passive:false});"
    "function jX(){let s=gs()/100;let y=Math.round(jy*s),z=Math.round(jx*s);"
    "fetch('/cmd?act=move&y='+y+'&z='+z).catch(()=>{});}"
    "async function cmd(a){try{await fetch('/cmd?act='+encodeURIComponent(a));}catch(e){}rf();}"
    "function hs(a){hA=a;cmd(a);if(hT)clearInterval(hT);hT=setInterval(()=>cmd(hA),300);}"
    "function hx(){if(hT){clearInterval(hT);hT=0;}if(hA){cmd('stop');hA='';}}"
    "function showTab(t){"
    "['control','settings','power'].forEach(id=>{"
    "document.getElementById('tab'+id.charAt(0).toUpperCase()+id.slice(1)).style.display=(t===id)?'block':'none';"
    "document.getElementById('tabBtn'+id.charAt(0).toUpperCase()+id.slice(1)).classList.toggle('active',t===id);});}"
    /* status refresh */
    "const ST_COLORS={idle:'#2d8b2d',driving:'#2563eb',nav_busy:'#d97706',estop:'#dc2626'};"
    "async function rf(){try{"
    "const r=await fetch('/status');const j=await r.json();"
    "const st=j.state||'idle';"
    "const pill=document.getElementById('ePill');"
    "pill.textContent=st.toUpperCase().replace('_',' ');"
    "pill.style.background=ST_COLORS[st]||'#374151';"
    "document.getElementById('sBat').textContent=j.bat_v>0.1?j.bat_v.toFixed(1)+'V':'bat:--';"
    "}catch(e){document.getElementById('ePill').textContent='ERR';}}"
    /* settings */
    "function mChg(){"
    "const m=parseInt(document.getElementById('wMode').value);"
    "document.getElementById('apF').style.display=(m===0||m===2)?'block':'none';"
    "document.getElementById('staF').style.display=(m===1||m===2)?'block':'none';}"
    "function wi(t){document.getElementById('wInfo').textContent=t;}"
    "function aOpt(sel,v,l){const o=document.createElement('option');"
    "o.value=v;o.textContent=l||v;sel.appendChild(o);}"
    "function fillNets(nets,cur){"
    "const sel=document.getElementById('stSSID');"
    "const prev=selW||cur||sel.value||'';"
    "sel.innerHTML='';"
    "if(prev&&!nets.some(n=>n.ssid===prev))aOpt(sel,prev,prev+' (saved)');"
    "if(!nets.length)aOpt(sel,'','No networks found');"
    "nets.forEach(n=>aOpt(sel,n.ssid,n.ssid+' ('+n.rssi+' dBm)'));"
    "if(prev)sel.value=prev;selW=sel.value||prev;}"
    "async function loadSet(){try{"
    "const r=await fetch('/settings');const j=await r.json();if(!j.ok)throw 0;"
    "document.getElementById('wMode').value=j.wifi_mode||0;"
    "document.getElementById('apSSID').value=j.wifi_ap_ssid||'';"
    "selW=j.wifi_ssid||'';"
    "document.getElementById('apPW').value='';"
    "document.getElementById('stPW').value='';"
    "document.getElementById('lMaxSpd').value=Math.round((j.max_speed||0)*100);"
    "document.getElementById('lMaxDur').value=j.max_command_duration_ms||0;"
    "document.getElementById('sysEn').checked=!!j.syslog_enabled;"
    "document.getElementById('sysHost').value=j.syslog_host||'';"
    "document.getElementById('sysPort').value=j.syslog_port||0;"
    "document.getElementById('sysFac').value=j.syslog_facility||0;"
    "mChg();"
    "wi(j.wifi_connected?'Connected: '+j.wifi_ip:'IP: '+j.wifi_ip);"
    "}catch(e){wi('load failed');}}"
    "async function wScan(){wi('scanning...');try{"
    "const r=await fetch('/wifi_scan');const j=await r.json();"
    "if(!j.ok)throw j.error||'err';"
    "fillNets(j.networks||[],selW);"
    "wi('found '+(j.networks||[]).length+' networks');"
    "}catch(e){wi('scan failed: '+e);}}"
    "async function wSave(){"
    "const p={wifi_mode:parseInt(document.getElementById('wMode').value),"
    "wifi_ap_ssid:document.getElementById('apSSID').value,"
    "wifi_ap_password:document.getElementById('apPW').value,"
    "wifi_ssid:document.getElementById('stSSID').value,"
    "wifi_password:document.getElementById('stPW').value,"
    "max_speed:(parseInt(document.getElementById('lMaxSpd').value)||0)/100,"
    "max_command_duration_ms:parseInt(document.getElementById('lMaxDur').value)||0,"
    "syslog_enabled:document.getElementById('sysEn').checked,"
    "syslog_host:document.getElementById('sysHost').value,"
    "syslog_port:parseInt(document.getElementById('sysPort').value)||0,"
    "syslog_facility:parseInt(document.getElementById('sysFac').value)||0};"
    "wi('saving...');"
    "const r=await fetch('/settings',{method:'POST',"
    "headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});"
    "const t=await r.text();"
    "try{const j=JSON.parse(t);"
    "if(j.reboot){wi('saved — rebooting...');return;}wi('saved');loadSet();}"
    "catch(e){wi(t);}}"
    "async function wReset(){if(!confirm('Reset all settings?'))return;"
    "wi('resetting...');"
    "const r=await fetch('/settings/reset',{method:'POST'});"
    "const t=await r.text();"
    "try{const j=JSON.parse(t);if(j.reboot){wi('reset — rebooting...');return;}}"
    "catch(e){}wi(t);}"
    "document.getElementById('stSSID').onchange=function(){selW=this.value;};"
    "const PW_COLORS={active:'#2563eb',idle:'#2d8b2d',low_power:'#d97706'};"
    "async function rfPower(){try{"
    "const r=await fetch('/power');const j=await r.json();"
    "if(j.error){document.getElementById('pwMode').textContent='N/A';return;}"
    "const m=j.mode||'unknown';"
    "const pill=document.getElementById('pwMode');"
    "pill.textContent=m.toUpperCase().replace('_',' ');"
    "pill.style.background=PW_COLORS[m]||'#374151';"
    "document.getElementById('pwBat').textContent=j.battery_voltage>0.1?j.battery_voltage.toFixed(1)+'V':'--';"
    "document.getElementById('pwAct').textContent=j.last_activity_sec_ago+'s ago';"
    "}catch(e){}}"
    "async function setMode(m){"
    "document.getElementById('pwErr').textContent='setting '+m+'...';"
    "try{"
    "const r=await fetch('/power',{method:'POST',"
    "headers:{'Content-Type':'application/json'},body:JSON.stringify({mode:m})});"
    "const j=await r.json();"
    "document.getElementById('pwErr').textContent=j.ok?'':('Error: '+(j.error||'?'));"
    "rfPower();"
    "}catch(e){document.getElementById('pwErr').textContent='request failed';}}"
    "dJ();"
    "Promise.all([rf(),loadSet(),rfPower()]).finally(()=>{setInterval(rf,1500);setInterval(rfPower,2000);});"
    "async function otaFlash(){"
    "const f=document.getElementById('fwFile').files[0];"
    "if(!f){alert('Select a .bin firmware file');return;}"
    "const b=document.getElementById('fwBtn');"
    "b.disabled=true;"
    "document.getElementById('fwSt').textContent='Uploading '+f.size+' bytes...';"
    "try{"
    "const r=await fetch('/update',{method:'POST',"
    "headers:{'Content-Type':'application/octet-stream'},body:f});"
    "const j=await r.json();"
    "if(j.ok){document.getElementById('fwSt').textContent='Flashed — rebooting…';}"
    "else{document.getElementById('fwSt').textContent='Error: '+(j.error||'?');b.disabled=false;}"
    "}catch(e2){"
    "document.getElementById('fwSt').textContent='Upload failed: '+e2;"
    "b.disabled=false;}}"
    "</script></body></html>";

/* ------------------------------------------------------------------ */
/* Background sensor task — polls I2C outside the httpd task          */
/* ------------------------------------------------------------------ */

static void sensor_poll_task(void *arg)
{
    for (;;) {
        wr_power_status_t ps = {0};
        wr_power_get_status(&ps);
        s_cached_bat_v = ps.load_voltage_v;
        uint16_t interval_sec = s_web_power_mgr
            ? wr_power_mgr_get_telemetry_interval_sec(s_web_power_mgr) : 1;
        if (interval_sec < 1) interval_sec = 1;
        vTaskDelay(pdMS_TO_TICKS((uint32_t)interval_sec * 1000));
    }
}

/* ------------------------------------------------------------------ */
/* HTTP handlers                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t handle_root(httpd_req_t *req)
{
    WEB_REQUIRE_AUTH(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, s_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_status(httpd_req_t *req)
{
    WEB_REQUIRE_AUTH(req);
    wr_motor_state_t ms = {0};
    wr_motor_get_state(&ms);

    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"bat_v\":%.2f,\"estop\":%s,\"left\":%.2f,\"right\":%.2f,\"state\":\"%s\"}",
             (double)s_cached_bat_v,
             wr_motor_emergency_stop_active() ? "true" : "false",
             (double)ms.left,
             (double)ms.right,
             wr_rover_state_name(wr_rover_state_get()));

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_cmd(httpd_req_t *req)
{
    WEB_REQUIRE_AUTH(req);
    if (s_web_power_mgr) wr_power_mgr_notify_activity(s_web_power_mgr, "web");
    char query[160] = {0};
    char action[32] = "stop";
    int  y = 0, z = 0;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "act", action, sizeof(action));
        char val[12];
        if (httpd_query_key_value(query, "y", val, sizeof(val)) == ESP_OK)
            y = clamp_int(atoi(val), -100, 100);
        if (httpd_query_key_value(query, "z", val, sizeof(val)) == ESP_OK)
            z = clamp_int(atoi(val), -100, 100);
    }

    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    float max_spd = (s_web_cfg && s_web_cfg->max_speed > 0.0f) ? s_web_cfg->max_speed : 1.0f;

    if (strcmp(action, "move") == 0) {
        /* Tank-drive mixing: y=forward/back, z=rotation */
        float left  = clamp_int(y + z, -100, 100) / 100.0f;
        float right = clamp_int(y - z, -100, 100) / 100.0f;
        if (left  >  max_spd) left  =  max_spd;
        if (left  < -max_spd) left  = -max_spd;
        if (right >  max_spd) right =  max_spd;
        if (right < -max_spd) right = -max_spd;
        wr_motor_set(left, right);
        s_web_deadline = now_ms + 1500;
    } else if (strcmp(action, "rotate_left") == 0) {
        float spd = max_spd < 0.6f ? max_spd : 0.6f;
        wr_motor_set(-spd, spd);
        s_web_deadline = now_ms + 1500;
    } else if (strcmp(action, "rotate_right") == 0) {
        float spd = max_spd < 0.6f ? max_spd : 0.6f;
        wr_motor_set(spd, -spd);
        s_web_deadline = now_ms + 1500;
    } else if (strcmp(action, "estop") == 0) {
        s_web_deadline = 0;
        wr_motor_emergency_stop_set();
    } else if (strcmp(action, "clear_estop") == 0) {
        wr_motor_emergency_stop_clear();
    } else {
        /* stop / unknown */
        s_web_deadline = 0;
        wr_motor_stop();
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ------------------------------------------------------------------ */
/* OTA firmware update — POST /update with raw binary body            */
/* ------------------------------------------------------------------ */

#define OTA_CHUNK   4096
#define OTA_MAX     (2 * 1024 * 1024)

static esp_err_t handle_update(httpd_req_t *req)
{
    WEB_REQUIRE_AUTH(req);

    /* Disable WiFi PS for the duration of the upload so the TCP connection
     * stays reliable regardless of the current power mode. The power manager
     * will restore the correct PS level on the next mode transition. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    uint32_t ota_lock_id = 0;
    if (s_web_power_mgr) wr_power_mgr_acquire_lock(s_web_power_mgr, "ota", 0, &ota_lock_id);

    int total = (int)req->content_len;
    if (total <= 0 || total > OTA_MAX) {
        if (s_web_power_mgr && ota_lock_id) wr_power_mgr_release_lock(s_web_power_mgr, ota_lock_id);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"size\"}");
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        if (s_web_power_mgr && ota_lock_id) wr_power_mgr_release_lock(s_web_power_mgr, ota_lock_id);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no_ota_partition\"}");
    }

    esp_ota_handle_t ota = 0;
    /* Pre-erase the full image size upfront so subsequent writes are fast
     * and do not repeatedly interrupt the WiFi stack (sequential writes
     * trigger a sector erase on every 4 KB chunk, which causes TCP drops). */
    esp_err_t err = esp_ota_begin(part, (size_t)total, &ota);
    if (err != ESP_OK) {
        if (s_web_power_mgr && ota_lock_id) wr_power_mgr_release_lock(s_web_power_mgr, ota_lock_id);
        ESP_LOGE(TAG, "ota begin: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"begin\"}");
    }

    char *buf = malloc(OTA_CHUNK);
    if (!buf) {
        if (s_web_power_mgr && ota_lock_id) wr_power_mgr_release_lock(s_web_power_mgr, ota_lock_id);
        esp_ota_abort(ota);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"oom\"}");
    }

    int remaining = total;
    bool write_ok = true;
    while (remaining > 0 && write_ok) {
        int want = remaining < OTA_CHUNK ? remaining : OTA_CHUNK;
        int got  = httpd_req_recv(req, buf, want);
        if (got <= 0) {
            ESP_LOGE(TAG, "ota recv error: %d", got);
            write_ok = false;
            break;
        }
        err = esp_ota_write(ota, buf, (size_t)got);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota write: %s", esp_err_to_name(err));
            write_ok = false;
            break;
        }
        remaining -= got;
    }
    free(buf);

    if (!write_ok) {
        if (s_web_power_mgr && ota_lock_id) wr_power_mgr_release_lock(s_web_power_mgr, ota_lock_id);
        esp_ota_abort(ota);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"write\"}");
    }

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        if (s_web_power_mgr && ota_lock_id) wr_power_mgr_release_lock(s_web_power_mgr, ota_lock_id);
        ESP_LOGE(TAG, "ota end: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"verify\"}");
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        if (s_web_power_mgr && ota_lock_id) wr_power_mgr_release_lock(s_web_power_mgr, ota_lock_id);
        ESP_LOGE(TAG, "ota set_boot: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"set_boot\"}");
    }

    /* Success path: device reboots, no lock release needed */
    ESP_LOGI(TAG, "OTA complete (%d bytes) → %s; rebooting", total, part->label);
    httpd_resp_set_type(req, "application/json");
    esp_err_t se = httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return se;
}

static esp_err_t handle_settings_get(httpd_req_t *req)
{
    WEB_REQUIRE_AUTH(req);
    if (!s_web_cfg) return httpd_resp_sendstr(req, "{\"ok\":false}");

    char ip[20];
    get_ip(ip, sizeof(ip));

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"oom\"}");
    }
    cJSON_AddBoolToObject(root,   "ok",             true);
    cJSON_AddNumberToObject(root, "wifi_mode",      s_web_cfg->wifi_mode);
    cJSON_AddStringToObject(root, "wifi_ap_ssid",   s_web_cfg->wifi_ap_ssid);
    cJSON_AddStringToObject(root, "wifi_ssid",      s_web_cfg->wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_ip",        ip);
    cJSON_AddBoolToObject(root,   "wifi_connected", is_sta_connected());
    cJSON_AddNumberToObject(root, "max_speed",      s_web_cfg->max_speed);
    cJSON_AddNumberToObject(root, "max_command_duration_ms", s_web_cfg->max_command_duration_ms);
    cJSON_AddBoolToObject(root,   "syslog_enabled", s_web_cfg->syslog_enabled);
    cJSON_AddStringToObject(root, "syslog_host",    s_web_cfg->syslog_host);
    cJSON_AddNumberToObject(root, "syslog_port",    s_web_cfg->syslog_port);
    cJSON_AddNumberToObject(root, "syslog_facility",s_web_cfg->syslog_facility);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"oom\"}");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t se = httpd_resp_sendstr(req, json);
    free(json);
    return se;
}

static bool read_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0 || req->content_len == 0 ||
        req->content_len >= buf_size) {
        return false;
    }
    int total = (int)req->content_len;
    int done  = 0;
    while (done < total) {
        int r = httpd_req_recv(req, buf + done, total - done);
        if (r <= 0) return false;
        done += r;
    }
    buf[total] = '\0';
    return true;
}

static esp_err_t handle_settings_post(httpd_req_t *req)
{
    WEB_REQUIRE_AUTH(req);
    if (s_web_power_mgr) wr_power_mgr_notify_activity(s_web_power_mgr, "web");
    char *body = malloc(1024);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"oom\"}");
    }
    if (!read_body(req, body, 1024)) {
        free(body);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"body\"}");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"json\"}");
    }

    /* Copy current config, apply updates */
    wave_rover_config_t cfg;
    memcpy(&cfg, s_web_cfg, sizeof(cfg));

    cJSON *v;
    v = cJSON_GetObjectItem(root, "wifi_mode");
    if (cJSON_IsNumber(v) && v->valueint >= 0 && v->valueint <= 2)
        cfg.wifi_mode = (uint8_t)v->valueint;

    v = cJSON_GetObjectItem(root, "wifi_ap_ssid");
    if (cJSON_IsString(v) && v->valuestring[0])
        strlcpy(cfg.wifi_ap_ssid, v->valuestring, sizeof(cfg.wifi_ap_ssid));

    /* Passwords: only update if non-empty */
    v = cJSON_GetObjectItem(root, "wifi_ap_password");
    if (cJSON_IsString(v) && v->valuestring[0])
        strlcpy(cfg.wifi_ap_password, v->valuestring, sizeof(cfg.wifi_ap_password));

    v = cJSON_GetObjectItem(root, "wifi_ssid");
    if (cJSON_IsString(v))
        strlcpy(cfg.wifi_ssid, v->valuestring, sizeof(cfg.wifi_ssid));

    v = cJSON_GetObjectItem(root, "wifi_password");
    if (cJSON_IsString(v) && v->valuestring[0])
        strlcpy(cfg.wifi_password, v->valuestring, sizeof(cfg.wifi_password));

    v = cJSON_GetObjectItem(root, "max_speed");
    if (cJSON_IsNumber(v) && v->valuedouble >= 0.0 && v->valuedouble <= 1.0)
        cfg.max_speed = (float)v->valuedouble;

    v = cJSON_GetObjectItem(root, "max_command_duration_ms");
    if (cJSON_IsNumber(v) && v->valueint >= 1 && v->valueint <= 30000)
        cfg.max_command_duration_ms = (uint32_t)v->valueint;

    v = cJSON_GetObjectItem(root, "syslog_enabled");
    if (cJSON_IsBool(v))
        cfg.syslog_enabled = cJSON_IsTrue(v);

    v = cJSON_GetObjectItem(root, "syslog_host");
    if (cJSON_IsString(v))
        strlcpy(cfg.syslog_host, v->valuestring, sizeof(cfg.syslog_host));

    v = cJSON_GetObjectItem(root, "syslog_port");
    if (cJSON_IsNumber(v) && v->valueint >= 1 && v->valueint <= 65535)
        cfg.syslog_port = (uint16_t)v->valueint;

    v = cJSON_GetObjectItem(root, "syslog_facility");
    if (cJSON_IsNumber(v) && v->valueint >= 0 && v->valueint <= 23)
        cfg.syslog_facility = (uint8_t)v->valueint;

    cJSON_Delete(root);

    esp_err_t err = wave_rover_config_save(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings save failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"save\"}");
    }

    /* Always reboot to apply WiFi changes */
    httpd_resp_set_type(req, "application/json");
    esp_err_t se = httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return se;
}

static esp_err_t handle_settings_reset(httpd_req_t *req)
{
    WEB_REQUIRE_AUTH(req);
    wave_rover_config_t defaults;
    wave_rover_config_defaults(&defaults);

    esp_err_t err = wave_rover_config_save(&defaults);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"reset\"}");
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t se = httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return se;
}

static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    WEB_REQUIRE_AUTH(req);
    /* In pure-AP mode switch to APSTA so scanning works */
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    wifi_scan_config_t sc = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"scan\"}");
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > 32) count = 32;

    wifi_ap_record_t *recs = count ? calloc(count, sizeof(wifi_ap_record_t)) : NULL;
    if (count && !recs) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"oom\"}");
    }
    if (recs) {
        uint16_t got = count;
        if (esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) got = 0;
        count = got;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "networks");
    cJSON_AddBoolToObject(root, "ok", true);

    for (uint16_t i = 0; i < count; i++) {
        wifi_ap_record_t *best = &recs[i];
        if (!best->ssid[0]) continue;
        /* Deduplicate: keep strongest signal per SSID */
        for (uint16_t j = i + 1; j < count; j++) {
            if (!recs[j].ssid[0]) continue;
            if (strcmp((char *)best->ssid, (char *)recs[j].ssid) == 0) {
                if (recs[j].rssi > best->rssi) *best = recs[j];
                recs[j].ssid[0] = '\0';
            }
        }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid",    (char *)best->ssid);
        cJSON_AddNumberToObject(item, "rssi",    best->rssi);
        cJSON_AddNumberToObject(item, "channel", best->primary);
        cJSON_AddItemToArray(arr, item);
    }
    free(recs);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"oom\"}");
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t se = httpd_resp_sendstr(req, json);
    free(json);
    return se;
}

static esp_err_t handle_power_get(httpd_req_t *req)
{
    WEB_REQUIRE_AUTH(req);
    char buf[256];
    if (!s_web_power_mgr ||
        wr_power_mgr_get_status_json(s_web_power_mgr, buf, sizeof(buf)) != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "{\"error\":\"power manager unavailable\"}",
                               HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_power_post(httpd_req_t *req)
{
    WEB_REQUIRE_AUTH(req);
    char body[128] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"recv\"}");
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"json\"}");
    }
    const cJSON *mode_j = cJSON_GetObjectItem(root, "mode");
    wr_power_mode_t mode = WR_POWER_MODE_ACTIVE;
    bool ok = false;
    if (cJSON_IsString(mode_j)) {
        if (strcasecmp(mode_j->valuestring, "ACTIVE") == 0)    { mode = WR_POWER_MODE_ACTIVE;    ok = true; }
        else if (strcasecmp(mode_j->valuestring, "IDLE") == 0) { mode = WR_POWER_MODE_IDLE;      ok = true; }
        else if (strcasecmp(mode_j->valuestring, "LOW_POWER") == 0) { mode = WR_POWER_MODE_LOW_POWER; ok = true; }
    }
    cJSON_Delete(root);

    if (!ok || !s_web_power_mgr) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid mode\"}",
                               HTTPD_RESP_USE_STRLEN);
    }
    esp_err_t err = wr_power_mgr_set_mode(s_web_power_mgr, mode, "web_ui");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req,
        err == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"rejected\"}",
        HTTPD_RESP_USE_STRLEN);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t wr_mcp_web_register(httpd_handle_t server,
                               const wave_rover_config_t *cfg)
{
    s_web_cfg = cfg;

    const httpd_uri_t routes[] = {
        { "/",               HTTP_GET,  handle_root,          NULL },
        { "/status",         HTTP_GET,  handle_status,        NULL },
        { "/cmd",            HTTP_GET,  handle_cmd,           NULL },
        { "/update",         HTTP_POST, handle_update,        NULL },
        { "/settings",       HTTP_GET,  handle_settings_get,  NULL },
        { "/settings",       HTTP_POST, handle_settings_post, NULL },
        { "/settings/reset", HTTP_POST, handle_settings_reset,NULL },
        { "/wifi_scan",      HTTP_GET,  handle_wifi_scan,     NULL },
        { "/power",          HTTP_GET,  handle_power_get,     NULL },
        { "/power",          HTTP_POST, handle_power_post,    NULL },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to register %s: %s",
                     routes[i].uri, esp_err_to_name(err));
        }
    }

    s_web_timer = xTimerCreate("wr_web_wd", pdMS_TO_TICKS(500),
                               pdTRUE, NULL, web_watchdog_cb);
    if (s_web_timer) xTimerStart(s_web_timer, 0);

    xTaskCreate(sensor_poll_task, "wr_sensor", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "web UI registered on port %u at /", cfg->mcp_port);
    return ESP_OK;
}

void wr_mcp_web_stop(void)
{
    if (s_web_timer) {
        xTimerStop(s_web_timer, 0);
        xTimerDelete(s_web_timer, 0);
        s_web_timer = NULL;
    }
    s_web_deadline  = 0;
    s_web_cfg       = NULL;
    s_web_power_mgr = NULL;
}
