/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Web UI for rover_s3: drive, vision, chat, settings.
 * Ported from ai-rover with claw framework integration.
 */
#include "rover_s3_httpd.h"
#include "rover_s3_settings.h"
#include "rover_s3_wifi.h"

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_event.h"
#include "claw_event_router.h"
#include "claw_event_publisher.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "rover_s3_httpd";

/* ── Web response storage (for chat) ───────────────────────────────── */
#define WEB_RESP_MAX  2048
static char              s_web_response[WEB_RESP_MAX] = "idle";
static uint32_t          s_web_resp_id = 0;
static SemaphoreHandle_t s_resp_mutex;
static httpd_handle_t    s_httpd = NULL;

void rover_s3_httpd_set_response(const char *message)
{
    if (!message) return;
    if (!s_resp_mutex) return;
    xSemaphoreTake(s_resp_mutex, portMAX_DELAY);
    strlcpy(s_web_response, message, sizeof(s_web_response));
    s_web_resp_id++;
    xSemaphoreGive(s_resp_mutex);
}

/* ── web_send_message cap (receives agent replies for "web" channel) ── */
static esp_err_t web_send_message_execute(const char *input_json,
                                           const claw_cap_call_context_t *ctx,
                                           char *output, size_t output_size)
{
    (void)ctx;
    cJSON *args = cJSON_Parse(input_json ? input_json : "{}");
    const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(args, "message"));
    if (!msg) msg = cJSON_GetStringValue(cJSON_GetObjectItem(args, "text"));
    if (msg) {
        rover_s3_httpd_set_response(msg);
    }
    cJSON_Delete(args);
    if (output && output_size > 0) {
        snprintf(output, output_size, "{\"ok\":true}");
    }
    return ESP_OK;
}

static const claw_cap_descriptor_t s_web_desc = {
    .id          = "web_send_message",
    .name        = "web_send_message",
    .family      = "web",
    .description = "Internal: deliver agent reply to web UI client.",
    .kind        = CLAW_CAP_KIND_CALLABLE,
    .cap_flags   = 0,  /* not LLM-visible */
    .input_schema_json =
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"message\":{\"type\":\"string\"},"
        "\"chat_id\":{\"type\":\"string\"}"
        "}}",
    .execute = web_send_message_execute,
};

static const claw_cap_group_t s_web_group = {
    .group_id         = "cap_web",
    .plugin_name      = "cap_web",
    .descriptors      = &s_web_desc,
    .descriptor_count = 1,
};

/* ── HTML page ─────────────────────────────────────────────────────── */
static const char *k_html =
    "<!doctype html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1,user-scalable=no'>"
    "<title>Rover S3</title>"
    "<style>"
    "*{box-sizing:border-box}"
    "body{font-family:system-ui,sans-serif;background:#0b1220;color:#e5e7eb;margin:0;padding:10px;touch-action:manipulation}"
    "h1{font-size:18px;margin:0 0 8px}"
    ".tabs{display:flex;gap:4px;margin-bottom:10px}"
    ".tab{flex:1;padding:8px;background:#1f2937;border:1px solid #374151;border-radius:8px;cursor:pointer;text-align:center;font-size:13px}"
    ".tab.active{background:#2563eb;border-color:#1d4ed8}"
    ".panel{display:none}.panel.active{display:block}"
    ".card{background:#111827;border:1px solid #1f2937;border-radius:10px;padding:12px;margin-bottom:10px}"
    ".row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}"
    "button{background:#1f2937;color:#e5e7eb;border:1px solid #374151;border-radius:8px;"
    "padding:10px 14px;font-size:14px;cursor:pointer;flex:1;min-width:60px}"
    "button:active{background:#374151}"
    ".danger{background:#7f1d1d;border-color:#991b1b}"
    ".pill{display:inline-block;padding:3px 10px;border-radius:12px;font-size:12px;font-weight:600}"
    "textarea,input[type=text],input[type=password]{width:100%;background:#0f172a;color:#e5e7eb;"
    "border:1px solid #334155;border-radius:8px;padding:8px;font-size:14px}"
    "textarea{min-height:70px;resize:vertical}"
    "pre{white-space:pre-wrap;word-break:break-word;background:#0f172a;border:1px solid #334155;"
    "border-radius:8px;padding:10px;font-size:12px;max-height:200px;overflow:auto}"
    ".muted{opacity:.7;font-size:12px}"
    "label{display:block;font-size:12px;color:#9ca3af;margin:8px 0 3px}"
    "#joyWrap{margin:0 auto}"
    "canvas{display:block;margin:0 auto;border-radius:50%;background:#0f172a}"
    ".spd-row{display:flex;align-items:center;gap:8px;margin-top:8px}"
    ".spd-row input[type=range]{flex:1;accent-color:#2563eb}"
    ".cfg-save{background:#065f46;border-color:#047857;margin-top:10px}"
    "</style></head><body>"
    "<h1>&#x1F916; Rover S3 <span class='muted' id='ipLabel'></span></h1>"
    "<div class='tabs'>"
    "<div class='tab active' onclick='showTab(0)'>Drive</div>"
    "<div class='tab' onclick='showTab(1)'>Vision</div>"
    "<div class='tab' onclick='showTab(2)'>Chat</div>"
    "<div class='tab' onclick='showTab(3)'>Settings</div>"
    "</div>"
    /* ── Drive ── */
    "<div class='panel active' id='p0'>"
    "<div class='card'>"
    "<div class='row' style='justify-content:space-between'>"
    "<span class='pill' id='stPill' style='background:#2d8b2d'>IDLE</span>"
    "<span class='muted' id='stMotion'>--</span>"
    "<span class='muted' id='stGrip'>--</span>"
    "</div></div>"
    "<div class='card'>"
    "<div id='joyWrap'><canvas id='joy' width='180' height='180'></canvas></div>"
    "<div class='spd-row'><span class='muted'>Speed</span>"
    "<input type='range' id='spdSlider' min='10' max='100' value='80'>"
    "<span id='spdVal' class='muted'>80%</span></div>"
    "<div class='row' style='margin-top:8px'>"
    "<button onmousedown=\"hold('rl')\" onmouseup='stopHold()' ontouchstart=\"hold('rl')\" ontouchend='stopHold()'>&#8634; Left</button>"
    "<button class='danger' onclick=\"cmd('stop')\">STOP</button>"
    "<button onmousedown=\"hold('rr')\" onmouseup='stopHold()' ontouchstart=\"hold('rr')\" ontouchend='stopHold()'>Right &#8635;</button>"
    "</div>"
    "<div class='row' style='margin-top:8px'>"
    "<button onclick=\"cmd('open')\">Grip Open</button>"
    "<button onclick=\"cmd('close')\">Grip Close</button>"
    "</div></div></div>"
    /* ── Vision ── */
    "<div class='panel' id='p1'>"
    "<div class='card'>"
    "<div class='row'>"
    "<button onclick=\"vscan('SCAN')\">Scan</button>"
    "<button onclick=\"vscan('OBJECTS')\">Objects</button>"
    "<button onclick=\"vscan('PING')\">Ping</button>"
    "<button onclick='vcap()'>Capture</button>"
    "</div>"
    "<img id='camImg' style='display:none;max-width:100%;margin-top:8px;border-radius:8px' />"
    "<pre id='vOut' style='margin-top:8px'>--</pre>"
    "</div></div>"
    /* ── Chat ── */
    "<div class='panel' id='p2'>"
    "<div class='card'>"
    "<textarea id='msg' placeholder='Message for rover AI...'></textarea>"
    "<div class='row' style='margin-top:8px'>"
    "<button onclick='ask()'>Send</button>"
    "<button onclick='poll()'>Poll</button>"
    "</div>"
    "<div class='muted' id='chatInfo' style='margin-top:6px'>idle</div>"
    "<pre id='chatOut'></pre>"
    "</div></div>"
    /* ── Settings: native form works in all browsers including CNA popup ── */
    "<div class='panel' id='p3'>"
    "<div class='card'>"
    "<form id='cfgForm' method='POST' action='/config' onsubmit='return saveConfig(event)'>"
    "<label>WiFi SSID</label><input type='text' name='wifi_ssid' id='cWifiSsid'>"
    "<label>WiFi Password</label><input type='password' name='wifi_password' id='cWifiPass'>"
    "<label>LLM API Key</label><input type='password' name='llm_api_key' id='cLlmKey'>"
    "<label>LLM Model</label><input type='text' name='llm_model' id='cLlmModel'>"
    "<label>LLM Base URL</label><input type='text' name='llm_base_url' id='cLlmUrl'>"
    "<label>Telegram Bot Token</label><input type='password' name='tg_bot_token' id='cTgToken'>"
    "<label>Timezone (POSIX TZ)</label><input type='text' name='time_timezone' id='cTz'>"
    "<div class='row' style='margin-top:10px'>"
    "<button type='submit' class='cfg-save'>Save &amp; Restart</button>"
    "<button type='button' onclick='loadConfig()'>Reload</button>"
    "</div>"
    "</form>"
    "<div class='muted' id='cfgInfo' style='margin-top:6px'></div>"
    "</div></div>"
    /* ── Script ── */
    "<script>"
    "const tabs=document.querySelectorAll('.tab'),panels=document.querySelectorAll('.panel');"
    "function showTab(i){tabs.forEach((t,j)=>{t.classList.toggle('active',i===j);panels[j].classList.toggle('active',i===j);});}"
    /* joystick */
    "const C=document.getElementById('joy'),ctx=C.getContext('2d'),R=90,DR=30;"
    "let jx=0,jy=0,jDown=false,jTim=0;"
    "const spd=()=>parseInt(document.getElementById('spdSlider').value);"
    "document.getElementById('spdSlider').oninput=function(){document.getElementById('spdVal').textContent=this.value+'%';};"
    "function drawJ(){ctx.clearRect(0,0,180,180);ctx.beginPath();ctx.arc(R,R,R-2,0,Math.PI*2);"
    "ctx.fillStyle='#1f2937';ctx.fill();ctx.strokeStyle='#374151';ctx.lineWidth=2;ctx.stroke();"
    "ctx.beginPath();ctx.moveTo(R,15);ctx.lineTo(R,R*2-15);ctx.moveTo(15,R);ctx.lineTo(R*2-15,R);"
    "ctx.strokeStyle='#374151';ctx.lineWidth=1;ctx.stroke();"
    "let dx=jx*(R-DR)/100,dy=-jy*(R-DR)/100;"
    "ctx.beginPath();ctx.arc(R+dx,R+dy,DR,0,Math.PI*2);"
    "ctx.fillStyle=jDown?'#2563eb':'#4b5563';ctx.fill();ctx.strokeStyle='#60a5fa';ctx.lineWidth=2;ctx.stroke();}"
    "function jPos(e){const r=C.getBoundingClientRect();let t=e.touches?e.touches[0]:e;"
    "let px=t.clientX-r.left-R,py=t.clientY-r.top-R,d=Math.sqrt(px*px+py*py),mx=R-DR;"
    "if(d>mx){px=px/d*mx;py=py/d*mx;}jx=Math.round(px/mx*100);jy=Math.round(-py/mx*100);drawJ();}"
    "function jStart(e){e.preventDefault();jDown=true;jPos(e);if(!jTim)jTim=setInterval(jSend,100);}"
    "function jMove(e){e.preventDefault();if(jDown)jPos(e);}"
    "function jEnd(e){e.preventDefault();jDown=false;jx=0;jy=0;drawJ();jSend();"
    "if(jTim){clearInterval(jTim);jTim=0;}}"
    "C.addEventListener('mousedown',jStart);C.addEventListener('mousemove',jMove);"
    "C.addEventListener('mouseup',jEnd);C.addEventListener('mouseleave',jEnd);"
    "C.addEventListener('touchstart',jStart,{passive:false});C.addEventListener('touchmove',jMove,{passive:false});"
    "C.addEventListener('touchend',jEnd,{passive:false});"
    "function jSend(){let s=spd()/100;fetch('/cmd?act=move&x=0&y='+Math.round(jy*s)+'&z='+Math.round(jx*s)).catch(()=>{});}"
    /* commands */
    "async function cmd(a){try{await fetch('/cmd?act='+encodeURIComponent(a));}catch(e){}}"
    "let holdAct='',holdTim=0;"
    "function hold(a){holdAct=a;cmd(a==='rl'?'rotate_left':'rotate_right');if(holdTim)clearInterval(holdTim);"
    "holdTim=setInterval(()=>cmd(holdAct==='rl'?'rotate_left':'rotate_right'),300);}"
    "function stopHold(){if(holdTim){clearInterval(holdTim);holdTim=0;}if(holdAct){cmd('stop');holdAct='';}}"
    /* status */
    "async function refresh(){try{const j=await(await fetch('/status')).json();"
    "const p=document.getElementById('stPill');p.textContent=j.state||'?';"
    "const c={IDLE:'#2d8b2d',WEB_CTRL:'#2563eb',AI_THINK:'#d97706',OFFLINE:'#dc2626'};"
    "p.style.background=c[j.state]||'#374151';"
    "document.getElementById('stMotion').textContent=j.motion?'x:'+j.x+' y:'+j.y+' z:'+j.z:'Stopped';"
    "document.getElementById('stGrip').textContent='Grip:'+j.gripper;"
    "document.getElementById('ipLabel').textContent=j.ip||'';"
    "}catch(e){}}"
    /* vision */
    "async function vscan(c){const o=document.getElementById('vOut');o.textContent='scanning...';"
    "try{o.textContent=JSON.stringify(await(await fetch('/vision?cmd='+c)).json(),null,2);}catch(e){o.textContent=''+e;}}"
    "async function vcap(){const o=document.getElementById('vOut'),img=document.getElementById('camImg');"
    "o.textContent='capturing...';"
    "try{const r=await fetch('/vision?cmd=CAPTURE&quality=75');"
    "if(!r.ok){o.textContent='failed '+r.status;return;}"
    "const b=await r.blob();const u=URL.createObjectURL(b);"
    "img.onload=()=>URL.revokeObjectURL(u);img.src=u;img.style.display='block';"
    "o.textContent=''+b.size+' bytes';}catch(e){o.textContent=''+e;}}"
    /* chat */
    "let lastChatId=0;"
    "async function ask(){const m=document.getElementById('msg').value.trim();if(!m)return;"
    "document.getElementById('chatInfo').textContent='sending...';"
    "try{const j=await(await fetch('/chat',{method:'POST',headers:{'Content-Type':'text/plain'},body:m})).json();"
    "if(j.id){lastChatId=j.id;document.getElementById('chatInfo').textContent='pending id='+j.id;setTimeout(poll,800);}}"
    "catch(e){document.getElementById('chatInfo').textContent='error: '+e;}}"
    "async function poll(){if(!lastChatId){document.getElementById('chatInfo').textContent='no id';return;}"
    "try{const r=await fetch('/chat_result?id='+lastChatId);"
    "const t=await r.text();"
    "if(r.status===202){document.getElementById('chatInfo').textContent='pending...';setTimeout(poll,900);return;}"
    "document.getElementById('chatInfo').textContent='done';document.getElementById('chatOut').textContent=t;}"
    "catch(e){document.getElementById('chatInfo').textContent='poll error';}}"
    /* settings */
    "async function loadConfig(){try{const j=await(await fetch('/config')).json();"
    "document.getElementById('cWifiSsid').value=j.wifi_ssid||'';"
    "document.getElementById('cWifiPass').value=j.wifi_password||'';"
    "document.getElementById('cLlmKey').value=j.llm_api_key||'';"
    "document.getElementById('cLlmModel').value=j.llm_model||'';"
    "document.getElementById('cLlmUrl').value=j.llm_base_url||'';"
    "document.getElementById('cTgToken').value=j.tg_bot_token||'';"
    "document.getElementById('cTz').value=j.time_timezone||'';"
    "document.getElementById('cfgInfo').textContent='loaded';}catch(e){document.getElementById('cfgInfo').textContent=''+e;}}"
    /* saveConfig: tries fetch first, falls back to native form submit */
    "async function saveConfig(evt){"
    "if(evt)evt.preventDefault();"
    "const f=document.getElementById('cfgForm');"
    "const data=Object.fromEntries(new FormData(f));"
    "document.getElementById('cfgInfo').textContent='saving...';"
    "try{"
    "const r=await fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});"
    "const j=await r.json();"
    "document.getElementById('cfgInfo').textContent=j.ok?'saved, restarting...':'error: '+JSON.stringify(j);"
    "}catch(e){"
    /* fetch failed (captive portal or other) — fall back to native form POST */
    "document.getElementById('cfgInfo').textContent='submitting form...';"
    "f.submit();"
    "}"
    "return false;}"
    "drawJ();setInterval(refresh,1500);refresh();"
    "if(document.readyState==='loading'){document.addEventListener('DOMContentLoaded',loadConfig);}else{loadConfig();}"
    "</script></body></html>";

/* ── HTTP handlers ─────────────────────────────────────────────────── */

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, k_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_status(httpd_req_t *req)
{
    extern rover_s3_settings_t g_settings;
    const char *ip = rover_s3_wifi_get_ip();
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "{\"state\":\"IDLE\",\"motion\":0,\"x\":0,\"y\":0,\"z\":0,"
                     "\"gripper\":\"unknown\",\"ip\":\"%s\"}",
                     ip ? ip : "");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t h_cmd(httpd_req_t *req)
{
    char query[160] = {0};
    char action[48] = "stop";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "act", action, sizeof(action));

    char input[128];
    char output[256] = {0};

    if (strcmp(action, "stop") == 0) {
        snprintf(input, sizeof(input), "{}");
        claw_cap_call("rover_stop", input, NULL, output, sizeof(output));
    } else if (strcmp(action, "open") == 0) {
        snprintf(input, sizeof(input), "{}");
        claw_cap_call("rover_open_gripper", input, NULL, output, sizeof(output));
    } else if (strcmp(action, "close") == 0) {
        snprintf(input, sizeof(input), "{}");
        claw_cap_call("rover_close_gripper", input, NULL, output, sizeof(output));
    } else if (strcmp(action, "rotate_left") == 0) {
        char sval[8] = "30"; httpd_query_key_value(query, "s", sval, sizeof(sval));
        snprintf(input, sizeof(input),
                 "{\"direction\":\"left\",\"angle_deg\":15,\"speed_percent\":%d}", atoi(sval));
        claw_cap_call("rover_turn", input, NULL, output, sizeof(output));
    } else if (strcmp(action, "rotate_right") == 0) {
        char sval[8] = "30"; httpd_query_key_value(query, "s", sval, sizeof(sval));
        snprintf(input, sizeof(input),
                 "{\"direction\":\"right\",\"angle_deg\":15,\"speed_percent\":%d}", atoi(sval));
        claw_cap_call("rover_turn", input, NULL, output, sizeof(output));
    } else if (strcmp(action, "move") == 0) {
        char xv[8] = "0", yv[8] = "0", zv[8] = "0";
        httpd_query_key_value(query, "x", xv, sizeof(xv));
        httpd_query_key_value(query, "y", yv, sizeof(yv));
        httpd_query_key_value(query, "z", zv, sizeof(zv));
        int x = atoi(xv), y = atoi(yv), z = atoi(zv);
        if (x == 0 && y == 0 && z == 0) {
            claw_cap_call("rover_stop", "{}", NULL, output, sizeof(output));
        } else {
            snprintf(input, sizeof(input),
                     "{\"x\":%d,\"y\":%d,\"z\":%d,\"duration_ms\":200}", x, y, z);
            claw_cap_call("rover_move", input, NULL, output, sizeof(output));
        }
    }

    httpd_resp_set_type(req, "application/json");
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"act\":\"%.48s\"}", action);
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_vision(httpd_req_t *req)
{
    char query[96] = {0};
    char cmd[16] = "SCAN";
    char quality_str[8] = "75";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "cmd", cmd, sizeof(cmd));
    httpd_query_key_value(query, "quality", quality_str, sizeof(quality_str));

    if (strcmp(cmd, "CAPTURE") == 0) {
        char input[64];
        snprintf(input, sizeof(input), "{\"quality\":%d}", atoi(quality_str));
        char *output = NULL;
        esp_err_t err = claw_cap_call_from_core("unitv_capture", input, NULL, &output, NULL);
        if (err != ESP_OK || !output) {
            httpd_resp_set_status(req, "504 Gateway Timeout");
            httpd_resp_set_type(req, "application/json");
            if (output) free(output);
            return httpd_resp_send(req, "{\"ok\":false,\"error\":\"capture failed\"}", HTTPD_RESP_USE_STRLEN);
        }
        /* output is JSON with base64 image — return as JSON */
        httpd_resp_set_type(req, "application/json");
        esp_err_t send_err = httpd_resp_send(req, output, HTTPD_RESP_USE_STRLEN);
        free(output);
        return send_err;
    }

    /* SCAN / OBJECTS / WHO / PING */
    char input[64] = "{}";
    char output[1024] = {0};
    claw_cap_call("unitv_scan", input, NULL, output, sizeof(output));

    httpd_resp_set_type(req, "application/json");
    if (!output[0]) {
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"timeout\"}", HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_send(req, output, HTTPD_RESP_USE_STRLEN);
}

static uint32_t s_chat_id = 0;

static esp_err_t h_chat(httpd_req_t *req)
{
    char prompt[384] = {0};

    if (req->method == HTTP_POST && req->content_len > 0) {
        int to_read = (int)req->content_len;
        if (to_read >= (int)sizeof(prompt)) to_read = (int)sizeof(prompt) - 1;
        int got = 0;
        while (got < to_read) {
            int r = httpd_req_recv(req, prompt + got, to_read - got);
            if (r <= 0) break;
            got += r;
        }
        prompt[got] = '\0';
    }

    if (!prompt[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"missing msg\"}", HTTPD_RESP_USE_STRLEN);
    }

    /* Publish event to claw_event_router; default routing will send to agent */
    claw_event_t ev = {0};
    s_chat_id++;
    snprintf(ev.event_id, sizeof(ev.event_id), "web-%lu", (unsigned long)s_chat_id);
    strlcpy(ev.source_cap,     "cap_web",   sizeof(ev.source_cap));
    strlcpy(ev.event_type,     "message",   sizeof(ev.event_type));
    strlcpy(ev.source_channel, "web",       sizeof(ev.source_channel));
    strlcpy(ev.target_channel, "web",       sizeof(ev.target_channel));
    strlcpy(ev.chat_id,        "web",       sizeof(ev.chat_id));
    ev.text = prompt;
    claw_event_router_publish(&ev);

    /* Reset response buffer to indicate pending */
    rover_s3_httpd_set_response("...");

    char resp[64];
    int n = snprintf(resp, sizeof(resp), "{\"ok\":true,\"id\":%lu,\"status\":\"pending\"}",
                     (unsigned long)s_chat_id);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, n);
}

static esp_err_t h_chat_result(httpd_req_t *req)
{
    char query[32] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));

    xSemaphoreTake(s_resp_mutex, portMAX_DELAY);
    const char *text = s_web_response;
    bool pending = (strcmp(text, "...") == 0);
    char buf[WEB_RESP_MAX + 4];
    strlcpy(buf, text, sizeof(buf));
    xSemaphoreGive(s_resp_mutex);

    if (pending) {
        httpd_resp_set_status(req, "202 Accepted");
        return httpd_resp_send(req, "pending", HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_config_get(httpd_req_t *req)
{
    extern rover_s3_settings_t g_settings;
    rover_s3_settings_t *s = &g_settings;

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "wifi_ssid",     s->wifi_ssid);
    cJSON_AddStringToObject(j, "wifi_password", s->wifi_password);
    cJSON_AddStringToObject(j, "llm_api_key",   s->llm_api_key);
    cJSON_AddStringToObject(j, "llm_model",     s->llm_model);
    cJSON_AddStringToObject(j, "llm_base_url",  s->llm_base_url);
    cJSON_AddStringToObject(j, "llm_backend_type", s->llm_backend_type);
    cJSON_AddStringToObject(j, "llm_auth_type", s->llm_auth_type);
    cJSON_AddStringToObject(j, "tg_bot_token",  s->tg_bot_token);
    cJSON_AddStringToObject(j, "time_timezone", s->time_timezone);
    cJSON_AddStringToObject(j, "tts_voice",     s->tts_voice);
    cJSON_AddStringToObject(j, "voice_enabled", s->voice_enabled);

    char *out = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!out) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return err;
}

static void deferred_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

/* Simple URL decode in-place: %XX → char, + → space */
static void url_decode_inplace(char *s)
{
    char *p = s, *q = s;
    while (*p) {
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = {p[1], p[2], '\0'};
            *q++ = (char)strtol(hex, NULL, 16);
            p += 3;
        } else {
            *q++ = (*p == '+') ? ' ' : *p;
            p++;
        }
    }
    *q = '\0';
}

static esp_err_t h_config_post(httpd_req_t *req)
{
    extern rover_s3_settings_t g_settings;

    /* Read body — accept both JSON and form-encoded */
    size_t max_body = 2048;
    char *body = NULL;

    if (req->content_len > 0 && req->content_len <= 4096) {
        body = malloc(req->content_len + 1);
        if (body) {
            int got = 0;
            while (got < (int)req->content_len) {
                int r = httpd_req_recv(req, body + got,
                                       req->content_len - (size_t)got);
                if (r <= 0) break;
                got += r;
            }
            body[got] = '\0';
            max_body = (size_t)got;
        }
    } else {
        /* No Content-Length — read until connection closes */
        body = malloc(2049);
        if (body) {
            int got = 0;
            while (got < 2048) {
                int r = httpd_req_recv(req, body + got, 2048 - (size_t)got);
                if (r <= 0) break;
                got += r;
            }
            body[got] = '\0';
            max_body = (size_t)got;
        }
    }

    if (!body) return httpd_resp_send_500(req);

    /* Detect content type: try JSON first, else form-encoded */
    char ct[64] = {0};
    httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct));
    bool is_json = (strstr(ct, "application/json") != NULL);
    bool body_is_json = (body[0] == '{');

    rover_s3_settings_t s = g_settings;

    if (is_json || body_is_json) {
        cJSON *j = cJSON_Parse(body);
        if (j) {
#define SET_J(key, field) do { \
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(j, key)); \
    if (v) strlcpy(s.field, v, sizeof(s.field)); \
} while(0)
            SET_J("wifi_ssid",       wifi_ssid);
            SET_J("wifi_password",   wifi_password);
            SET_J("llm_api_key",     llm_api_key);
            SET_J("llm_model",       llm_model);
            SET_J("llm_base_url",    llm_base_url);
            SET_J("llm_backend_type",llm_backend_type);
            SET_J("llm_auth_type",   llm_auth_type);
            SET_J("tg_bot_token",    tg_bot_token);
            SET_J("time_timezone",   time_timezone);
            SET_J("tts_voice",       tts_voice);
            SET_J("voice_enabled",   voice_enabled);
#undef SET_J
            cJSON_Delete(j);
        }
    } else {
        /* application/x-www-form-urlencoded from native <form> POST */
#define SET_FORM(key, field) do { \
    char v[sizeof(s.field)] = {0}; \
    if (httpd_query_key_value(body, key, v, sizeof(v)) == ESP_OK) { \
        url_decode_inplace(v); \
        strlcpy(s.field, v, sizeof(s.field)); \
    } \
} while(0)
        SET_FORM("wifi_ssid",       wifi_ssid);
        SET_FORM("wifi_password",   wifi_password);
        SET_FORM("llm_api_key",     llm_api_key);
        SET_FORM("llm_model",       llm_model);
        SET_FORM("llm_base_url",    llm_base_url);
        SET_FORM("llm_backend_type",llm_backend_type);
        SET_FORM("llm_auth_type",   llm_auth_type);
        SET_FORM("tg_bot_token",    tg_bot_token);
        SET_FORM("time_timezone",   time_timezone);
        SET_FORM("tts_voice",       tts_voice);
        SET_FORM("voice_enabled",   voice_enabled);
#undef SET_FORM
    }

    free(body);

    ESP_LOGI(TAG, "config save: ssid='%s' model='%s'",
             s.wifi_ssid, s.llm_model);

    rover_s3_settings_save(&s);
    g_settings = s;

    /* JSON response for fetch(), or HTML page for native form submission */
    bool want_html = !is_json && !body_is_json;
    xTaskCreate(deferred_restart_task, "httpd_restart", 2048, NULL, 5, NULL);

    if (want_html) {
        static const char *saved_html =
            "<!doctype html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<meta http-equiv='refresh' content='4;url=/'>"
            "<title>Saved</title>"
            "<style>body{font-family:system-ui,sans-serif;background:#0b1220;color:#e5e7eb;"
            "display:flex;align-items:center;justify-content:center;height:100vh;margin:0}"
            ".box{text-align:center;padding:20px}</style></head>"
            "<body><div class='box'>"
            "<h2>&#x2705; Settings Saved</h2>"
            "<p>Restarting in 4 seconds...</p>"
            "<p><a href='/'>Back to settings</a></p>"
            "</div></body></html>";
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, saved_html, HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true,\"restarting\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_config(httpd_req_t *req)
{
    if (req->method == HTTP_GET) return h_config_get(req);
    return h_config_post(req);
}

/* ── Start / Stop ──────────────────────────────────────────────────── */

esp_err_t rover_s3_httpd_start(void)
{
    if (s_httpd) return ESP_OK;

    if (!s_resp_mutex) {
        s_resp_mutex = xSemaphoreCreateMutex();
    }

    /* Register web cap group for chat responses */
    esp_err_t err = claw_cap_register_group(&s_web_group);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "web cap register: %s", esp_err_to_name(err));
    }
    claw_event_router_register_outbound_binding("web", "web_send_message");

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_uri_handlers = 12;
    cfg.stack_size       = 8192;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;

    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), TAG, "httpd start");

    static const httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = h_root       },
        { .uri = "/status",      .method = HTTP_GET,  .handler = h_status     },
        { .uri = "/cmd",         .method = HTTP_GET,  .handler = h_cmd        },
        { .uri = "/vision",      .method = HTTP_GET,  .handler = h_vision     },
        { .uri = "/chat",        .method = HTTP_GET,  .handler = h_chat       },
        { .uri = "/chat",        .method = HTTP_POST, .handler = h_chat       },
        { .uri = "/chat_result", .method = HTTP_GET,  .handler = h_chat_result},
        { .uri = "/config",      .method = HTTP_GET,  .handler = h_config     },
        { .uri = "/config",      .method = HTTP_POST, .handler = h_config     },
    };
    /* Wildcard handler: redirect all unknown URLs to / for captive portal */
    static const httpd_uri_t catchall = {
        .uri      = "/*",
        .method   = HTTP_GET,
        .handler  = h_root,  /* serve the page; browser will navigate to settings */
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_httpd, &uris[i]);
    }
    httpd_register_uri_handler(s_httpd, &catchall);

    ESP_LOGI(TAG, "HTTP server started on port 80");
    return ESP_OK;
}

esp_err_t rover_s3_httpd_stop(void)
{
    if (!s_httpd) return ESP_OK;
    httpd_stop(s_httpd);
    s_httpd = NULL;
    return ESP_OK;
}
