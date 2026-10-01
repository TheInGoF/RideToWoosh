// webui.h — Embedded web interface for RideToWooshHID
// ---------------------------------------------------------------------
// Single-page UI (index.html) served from PROGMEM on port 80.
// Live data + mapping over WebSocket on port 80, path /ws.
//
// Design: eigenständiges, markenneutrales "Look-alike" — dunkles, sportliches
// Theme mit Orange/Blau-Akzenten (KEINE Zwift-/MyWhoosh-Logos/Schriften/Assets).
// Kachel-Navigation (kein Burger-Menü), zweisprachig EN/DE über separate
// i18n-Dateien (localStorage = nicht-permanent auf dem ESP).
//
// Bedienung: Tasten-Feld antippen -> Picker (funktioniert auch ohne physische
// Tastatur am Handy) ODER eine echte Taste drücken. Änderungen gehen sofort live
// an die Firmware (setmap), persistent erst per "Speichern" -> "Unsaved"-Anzeige.
//
// WS-Protokoll (JSON):
//   ESP -> Browser: {"t":"state","ride":bool,"hid":bool,"ip":"...","rideDev":"..","hidDev":"..","bat":0-100|-1}
//                   {"t":"btn","mask":<uint32>,"names":[...]}
//                   {"t":"map","map":{"LEFT_BTN":"a", ...}}
//   Browser -> ESP: {"t":"setmap","btn":"..","key":".."}  {"t":"getmap"}  {"t":"save"}
#pragma once
#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"HTMLDELIM(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#0a0f17">
<title>RideToWoosh</title>
<style>
  :root{
    --o:#fc6719; --o2:#ff812e; --b:#19b6ff; --b2:#5fccff;
    --bg:#070b11; --bg2:#0b1119; --panel:#121d2c; --panel2:#0e1622;
    --line:#1f2b3b; --line2:#2a3a4e; --ink:#eef3fa;
    --muted:#94a3b8; --faint:#6c7c92; --off:#34425a;
    --ok:#2fd07a; --warn:#ffb52e;
    --shadow:0 10px 30px rgba(0,0,0,.45);
    --raise:inset 0 1px 0 rgba(255,255,255,.05), 0 4px 14px rgba(0,0,0,.28);
  }
  *{box-sizing:border-box}
  html,body{margin:0;color:var(--ink);-webkit-text-size-adjust:100%;min-height:100vh;
    font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif;
    background:
      radial-gradient(1100px 540px at 16% -8%, rgba(252,103,25,.16), transparent 60%),
      radial-gradient(1000px 560px at 92% 2%, rgba(25,182,255,.14), transparent 58%),
      linear-gradient(180deg,var(--bg2),var(--bg));
    background-attachment:fixed}
  .wrap{max-width:900px;margin:0 auto;padding:18px 16px 96px}
  b{color:var(--b2);font-weight:800}
  :focus-visible{outline:2px solid var(--b2);outline-offset:2px;border-radius:8px}
  .skip{position:absolute;left:-9999px;top:0;background:var(--o);color:#160a00;
    padding:10px 14px;border-radius:0 0 10px 0;font-weight:800;z-index:50}
  .skip:focus{left:0}
  .sr{position:absolute;width:1px;height:1px;padding:0;margin:-1px;overflow:hidden;
    clip:rect(0,0,0,0);white-space:nowrap;border:0}

  /* header */
  header{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap}
  .brand{display:flex;align-items:center;gap:12px}
  .logo{width:42px;height:42px;border-radius:12px;flex:0 0 auto;
    background:linear-gradient(135deg,var(--o),var(--b));display:grid;place-items:center;
    box-shadow:var(--shadow)}
  .logo svg{width:22px;height:22px;display:block}
  .brand h1{font-size:18px;font-weight:800;margin:0;line-height:1.1;letter-spacing:.01em}
  .brand .tag{font-size:11px;color:var(--muted);letter-spacing:.14em;text-transform:uppercase;margin-top:3px}
  .seg{display:inline-flex;border:1px solid var(--line2);border-radius:11px;overflow:hidden}
  .seg button{background:var(--panel2);color:var(--muted);border:0;padding:0 14px;min-height:40px;
    font:700 12px/1 inherit;letter-spacing:.06em;cursor:pointer}
  .seg button[aria-pressed="true"]{background:linear-gradient(135deg,var(--o),var(--o2));color:#160a00}

  /* persistent status bar */
  .statusbar{display:flex;gap:10px;width:100%;margin:16px 0 4px;padding:8px;
    border:1px solid var(--line);border-radius:14px;background:var(--panel2);
    cursor:pointer;text-align:left;box-shadow:var(--raise)}
  .pill{display:inline-flex;align-items:center;gap:8px;flex:1;min-height:44px;padding:0 12px;
    border-radius:10px;background:rgba(52,66,90,.18);border:1px solid var(--line);
    font:600 12px inherit;color:var(--muted);min-width:0}
  .pill.on{background:rgba(47,208,122,.12);border-color:rgba(47,208,122,.35);color:var(--ink)}
  .pill .lab{display:flex;flex-direction:column;gap:1px;min-width:0}
  .pill .st{font-size:10px;color:var(--faint)}
  .pill.on .st{color:var(--ok)}
  .dot{width:10px;height:10px;border-radius:50%;background:var(--off);flex:0 0 auto;
    transition:background .25s,box-shadow .25s}
  .dot.on{background:var(--ok);box-shadow:0 0 10px var(--ok)}
  .dot.searching{background:var(--warn);box-shadow:0 0 10px var(--warn);animation:pulse 1s ease-in-out infinite}
  @keyframes pulse{50%{opacity:.4}}

  .banner{margin:10px 0 0;padding:10px 14px;border-radius:10px;font:600 12px inherit;
    background:rgba(255,181,46,.12);border:1px solid rgba(255,181,46,.4);color:#ffd083;display:none}
  .banner.show{display:block}

  /* views */
  .view{display:none}
  .view.show{display:block;animation:fade .18s ease}
  @keyframes fade{from{opacity:0;transform:translateY(6px)}to{opacity:1;transform:none}}
  .back{display:inline-flex;align-items:center;gap:8px;margin:18px 0 6px;padding:0 16px;min-height:44px;
    border:1px solid var(--line2);border-radius:11px;background:var(--panel2);color:var(--ink);
    font:700 13px inherit;cursor:pointer}
  h2.vtitle{font-size:15px;font-weight:800;letter-spacing:.01em;margin:16px 0 12px}

  /* live hero */
  .live{margin:16px 0 18px;padding:24px 22px;border:1px solid var(--line);border-radius:18px;
    background:linear-gradient(180deg,var(--panel),var(--panel2));box-shadow:var(--shadow);
    position:relative;overflow:hidden}
  .live::after{content:"";position:absolute;inset:0;opacity:.5;pointer-events:none;
    background:repeating-linear-gradient(115deg,transparent 0 22px,rgba(255,255,255,.012) 22px 24px)}
  .live::before{content:"";position:absolute;inset:0;opacity:0;transition:opacity .12s;
    background:radial-gradient(420px 200px at 50% 0%,rgba(252,103,25,.32),transparent 70%)}
  .live.hit::before{opacity:1}
  .live .lbl{font-size:11px;letter-spacing:.18em;text-transform:uppercase;color:var(--muted);position:relative}
  .live .now{font-size:30px;font-weight:800;margin-top:10px;min-height:38px;color:var(--muted);
    transition:color .08s;position:relative;line-height:1.12}
  .live.hit .now{color:var(--o2);text-shadow:0 0 22px rgba(252,103,25,.5)}
  .live .keyout{margin-top:12px;min-height:18px;position:relative;display:flex;gap:8px;align-items:center;
    font:600 13px inherit;color:var(--muted)}

  /* setup checklist */
  .setup{margin:6px 0 18px;padding:18px;border:1px solid var(--line);border-radius:16px;
    background:var(--panel2);box-shadow:var(--raise);display:none}
  .setup.show{display:block}
  .setup h2{font-size:12px;letter-spacing:.16em;text-transform:uppercase;color:var(--muted);margin:0 0 12px}
  .step{display:flex;gap:12px;align-items:flex-start;padding:8px 0;font-size:14px;color:var(--ink)}
  .step+.step{border-top:1px solid var(--line)}
  .step .mk{width:24px;height:24px;border-radius:50%;flex:0 0 auto;display:grid;place-items:center;
    border:2px solid var(--line2);color:var(--faint);font-weight:800;font-size:13px}
  .step.done .mk{background:var(--ok);border-color:var(--ok);color:#04140b}
  .step.done{color:var(--muted)}

  /* nav tiles */
  .tiles{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:14px}
  .tile{display:flex;flex-direction:column;align-items:flex-start;gap:6px;text-align:left;
    padding:18px;border:1px solid var(--line);border-radius:18px;cursor:pointer;color:var(--ink);
    background:linear-gradient(180deg,var(--panel),var(--panel2));box-shadow:var(--shadow);
    transition:transform .1s,border-color .12s;font:inherit;min-height:44px}
  .tile:hover{transform:translateY(-2px);border-color:var(--line2)}
  .tic{width:44px;height:44px;border-radius:13px;display:grid;place-items:center;color:var(--b2);
    margin-bottom:4px;background:linear-gradient(135deg,rgba(252,103,25,.22),rgba(25,182,255,.22));
    border:1px solid var(--line2)}
  .tic svg{width:22px;height:22px}
  .tile .tt{font-size:16px;font-weight:800}
  .tile .td{font-size:12px;color:var(--muted);line-height:1.4}

  /* presets */
  .presets{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin:2px 0 8px}
  .presets .pl{font-size:11px;letter-spacing:.12em;text-transform:uppercase;color:var(--muted);width:100%}
  .chip{min-height:40px;padding:0 16px;border-radius:999px;border:1px solid var(--line2);
    background:var(--panel2);color:var(--ink);font:700 13px inherit;cursor:pointer}
  .chip:hover{border-color:var(--b)}

  /* mapping groups */
  .group{margin-top:20px}
  .group h3{font-size:11px;letter-spacing:.16em;text-transform:uppercase;color:var(--muted);
    margin:0 0 12px;display:flex;align-items:center;gap:10px;font-weight:700}
  .group h3::after{content:"";flex:1;height:1px;background:var(--line)}
  .group h3 .ic{width:16px;height:16px;border-radius:5px;flex:0 0 auto;
    background:linear-gradient(135deg,var(--o),var(--b));opacity:.85}
  .rows{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:10px}
  .row{display:grid;grid-template-columns:1fr 116px;gap:12px;align-items:center;padding:12px 14px;
    border:1px solid var(--line);border-radius:13px;background:var(--panel2);box-shadow:var(--raise);
    transition:border-color .12s,background .12s}
  .row.active{border-color:var(--o);background:linear-gradient(180deg,#231911,#1a130d)}
  .row.conflict{border-color:var(--warn)}
  .row .bn{display:flex;flex-direction:column;gap:3px;min-width:0}
  .row .bn .id{font-size:14px;font-weight:700;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .row .bn .cf{font-size:10px;color:var(--warn);display:none}
  .row.conflict .bn .cf{display:block}
  /* keycap */
  .cap{width:100%;min-height:44px;cursor:pointer;color:var(--ink);font:800 15px inherit;
    text-transform:uppercase;letter-spacing:.04em;text-align:center;
    background:linear-gradient(180deg,#1b2735,#0d1521);border:1px solid #324358;border-top-color:#42566e;
    border-radius:10px;box-shadow:0 2px 0 #05090f,0 3px 6px rgba(0,0,0,.5),inset 0 1px 0 rgba(255,255,255,.06);
    transition:transform .06s,border-color .12s}
  .cap:hover{border-color:var(--b)}
  .cap:active{transform:translateY(1px);box-shadow:0 1px 0 #05090f,inset 0 1px 0 rgba(255,255,255,.05)}
  .cap.empty{color:var(--faint);font-weight:600;text-transform:none}

  /* actions (sticky on mapping) */
  .actions{position:sticky;bottom:0;display:flex;gap:12px;margin-top:24px;padding:14px 0 4px;flex-wrap:wrap;
    background:linear-gradient(0deg,var(--bg) 60%,transparent)}
  button.act{font:700 13px inherit;letter-spacing:.03em;padding:0 22px;min-height:48px;border-radius:12px;
    border:1px solid var(--line2);background:var(--panel2);color:var(--ink);cursor:pointer;
    transition:transform .08s,filter .12s}
  button.act:hover{filter:brightness(1.12)}
  button.act:active{transform:translateY(1px)}
  button.primary{border:0;color:#160a00;font-weight:800;
    background:linear-gradient(135deg,var(--o),var(--o2));box-shadow:0 6px 18px rgba(252,103,25,.35)}
  button.primary.dirty{box-shadow:0 0 0 3px rgba(252,103,25,.25),0 6px 18px rgba(252,103,25,.4)}
  button.primary .badge{display:none;margin-left:8px;width:8px;height:8px;border-radius:50%;background:#160a00;
    vertical-align:middle}
  button.primary.dirty .badge{display:inline-block}
  .hint{font-size:12px;color:var(--muted);margin-top:18px;line-height:1.7;border-top:1px solid var(--line);padding-top:16px}

  /* devices */
  .devs{display:grid;grid-template-columns:1fr 1fr;gap:14px}
  @media (max-width:560px){.devs{grid-template-columns:1fr}}
  .dev{padding:18px;border:1px solid var(--line);border-radius:16px;background:var(--panel2);box-shadow:var(--raise)}
  .dev .top{display:flex;align-items:center;gap:10px}
  .dev .cl{font-size:10px;letter-spacing:.14em;text-transform:uppercase;color:var(--muted)}
  .dev .cv{font-size:15px;font-weight:700;margin-top:10px;color:var(--muted);word-break:break-all}
  .dev .cv.on{color:var(--ink)}
  .dev .note{font-size:11px;color:var(--faint);margin-top:8px;line-height:1.5}

  /* history */
  .log{border:1px solid var(--line);border-radius:14px;background:var(--panel2);box-shadow:var(--raise);
    max-height:60vh;overflow-y:auto;font-size:13px}
  .logempty{padding:16px;color:var(--faint)}
  .lrow{display:grid;grid-template-columns:auto 1fr auto auto;gap:12px;align-items:center;
    padding:9px 14px;border-bottom:1px solid var(--line)}
  .lrow:nth-child(even){background:rgba(255,255,255,.015)}
  .lrow:first-child{box-shadow:inset 3px 0 0 var(--o)}
  .lrow .lt{color:var(--faint);font-variant-numeric:tabular-nums}
  .lrow .la{color:var(--faint)}
  .lrow .lb{color:var(--ink);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .lrow .lk{color:var(--b2);font-weight:800;min-width:18px;text-align:center;text-transform:uppercase}

  /* settings */
  .card{padding:18px;border:1px solid var(--line);border-radius:16px;background:var(--panel2);
    margin-bottom:14px;box-shadow:var(--raise)}
  .card .cl{font-size:10px;letter-spacing:.14em;text-transform:uppercase;color:var(--muted);margin-bottom:12px}
  .card p{margin:0;font-size:13px;color:var(--muted);line-height:1.6}
  .card .note{font-size:11px;color:var(--faint);margin-top:10px}

  /* controller map (button/paddle styles live inside the SVG's own <style>) */
  .ctrlwrap{border:1px solid var(--line);border-radius:16px;background:#0e1622;box-shadow:var(--raise);padding:16px}
  .ctrlwrap svg{width:100%;height:auto;display:block}
  .ctrlwrap .kc{transition:fill .05s,stroke .05s}
  .ctrlhint{font-size:12px;color:var(--muted);margin-top:12px;text-align:center;line-height:1.6}
  .livekey{text-align:center;color:var(--muted);font:600 13px inherit;min-height:18px;margin:12px 0 2px}

  /* key picker modal */
  .modal{position:fixed;inset:0;background:rgba(4,8,13,.72);display:none;align-items:flex-end;
    justify-content:center;z-index:40;padding:0}
  .modal.show{display:flex;animation:fade .15s ease}
  .sheet{width:100%;max-width:560px;background:var(--panel);border:1px solid var(--line2);
    border-radius:18px 18px 0 0;box-shadow:var(--shadow);padding:18px 16px 24px;max-height:86vh;overflow-y:auto}
  @media (min-width:560px){.modal{align-items:center}.sheet{border-radius:18px}}
  .pkhead{display:flex;align-items:center;justify-content:space-between;gap:10px}
  .pkhead h2{font-size:16px;font-weight:800;margin:0}
  .pkx{min-width:44px;min-height:44px;border:1px solid var(--line2);border-radius:11px;background:var(--panel2);
    color:var(--ink);font:700 16px inherit;cursor:pointer}
  .pkhint{font-size:12px;color:var(--muted);margin:8px 0 14px}
  .pklbl{font-size:10px;letter-spacing:.14em;text-transform:uppercase;color:var(--faint);margin:14px 0 8px}
  .pkgrid{display:grid;grid-template-columns:repeat(auto-fill,minmax(56px,1fr));gap:8px}
  .pkgrid.wide{grid-template-columns:repeat(auto-fill,minmax(92px,1fr))}
  .pkkey{min-height:48px;border-radius:10px;border:1px solid #324358;border-top-color:#42566e;
    background:linear-gradient(180deg,#1b2735,#0d1521);color:var(--ink);font:800 15px inherit;cursor:pointer;
    box-shadow:0 2px 0 #05090f,inset 0 1px 0 rgba(255,255,255,.06)}
  .pkkey:hover{border-color:var(--b)}
  .pkkey:active{transform:translateY(1px)}
  .pkclear{margin-top:16px;width:100%;min-height:48px;border-radius:11px;border:1px solid var(--line2);
    background:transparent;color:var(--warn);font:700 13px inherit;cursor:pointer}

  .toast{position:fixed;left:50%;bottom:26px;transform:translateX(-50%) translateY(24px);
    background:linear-gradient(135deg,var(--ok),#27b569);color:#04140b;font:800 13px inherit;
    padding:12px 22px;border-radius:12px;opacity:0;box-shadow:var(--shadow);transition:.25s;
    pointer-events:none;z-index:60}
  .toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
  @media (prefers-reduced-motion:reduce){
    *{transition:none!important;animation:none!important}
    .tile:hover,.cap:active,.pkkey:active{transform:none!important}
  }
</style>
</head>
<body>
<a href="#main" class="skip" data-i18n="skip">Skip to main content</a>
<div class="wrap">
  <header>
    <div class="brand">
      <div class="logo" aria-hidden="true">
        <svg viewBox="0 0 24 24" fill="none"><path d="M7 8l10 8l-5 4l0 -16l5 4l-10 8" stroke="#0a0f17" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"/></svg>
      </div>
      <div>
        <h1>RideToWoosh</h1>
        <div class="tag" data-i18n="tag">Controller Bridge</div>
      </div>
    </div>
    <div class="seg" id="langSeg" role="group" aria-label="Language" data-i18n-aria="langLabel">
      <button data-lang="en" aria-pressed="true">EN</button>
      <button data-lang="de" aria-pressed="false">DE</button>
    </div>
  </header>

  <button class="statusbar" data-go="devices" aria-label="Connection status, open devices">
    <span class="pill" id="pIn"><span class="dot" id="dRide"></span>
      <span class="lab"><span data-i18n="conn.in">Input</span><span class="st" id="stRide">—</span></span></span>
    <span class="pill" id="pOut"><span class="dot" id="dHid"></span>
      <span class="lab"><span data-i18n="conn.out">Output</span><span class="st" id="stHid">—</span></span></span>
  </button>
  <div class="banner" id="banner" role="status" aria-live="polite" data-i18n="reconnecting">Reconnecting…</div>

  <main id="main">
  <!-- HOME -->
  <section class="view show" data-view="home" aria-label="Home">
    <div class="setup" id="setup">
      <h2 data-i18n="setup.title">Get started</h2>
      <div class="step" id="step-kbd"><span class="mk">1</span><span data-i18n="setup.kbd"></span></div>
      <div class="step" id="step-ride"><span class="mk">2</span><span data-i18n="setup.ride"></span></div>
      <div class="step" id="step-map"><span class="mk">3</span><span data-i18n="setup.remap"></span></div>
    </div>
    <div class="ctrlwrap"><svg id="ctrlSvg" viewBox="0 0 480 320" role="img" aria-label="Controller live map"></svg></div>
    <div class="livekey" id="liveKey" aria-live="polite"></div>
    <div class="tiles" id="tiles"></div>
  </section>

  <!-- MAPPING -->
  <section class="view" data-view="mapping" aria-labelledby="t-map">
    <button class="back" data-go="home">‹ <span data-i18n="back">Back</span></button>
    <h2 class="vtitle" id="t-map" tabindex="-1" data-i18n="nav.mapping.t">Key Mapping</h2>
    <div class="presets">
      <span class="pl" data-i18n="presets.label">Quick presets</span>
      <button class="chip" data-preset="MyWhoosh">MyWhoosh</button>
      <button class="chip" data-preset="MyWhooshClickV2">MyWhoosh · Click V2</button>
      <button class="chip" data-preset="Zwift">Zwift</button>
    </div>
    <div id="groups"></div>
    <div class="actions">
      <button class="act primary" id="btnSave"><span id="saveTxt" data-i18n="save">Save</span><span class="badge"></span></button>
      <button class="act" id="btnReload" data-i18n="reload">Revert</button>
    </div>
    <p class="hint" data-i18n="hint"></p>
  </section>

  <!-- HISTORY -->
  <section class="view" data-view="history" aria-labelledby="t-hist">
    <button class="back" data-go="home">‹ <span data-i18n="back">Back</span></button>
    <h2 class="vtitle" id="t-hist" tabindex="-1" data-i18n="nav.history.t">History</h2>
    <div class="log" id="log" role="log" tabindex="0" aria-label="Button press history">
      <div class="logempty" id="logEmpty" data-i18n="history_empty">Nothing pressed yet.</div>
    </div>
  </section>

  <!-- DEVICES -->
  <section class="view" data-view="devices" aria-labelledby="t-dev">
    <button class="back" data-go="home">‹ <span data-i18n="back">Back</span></button>
    <h2 class="vtitle" id="t-dev" tabindex="-1" data-i18n="nav.devices.t">Devices</h2>
    <div class="devs">
      <div class="dev">
        <div class="top"><span class="dot" id="dvIn"></span><span class="cl" data-i18n="conn.in">Input</span></div>
        <div class="cv" id="dvInVal">—</div>
      </div>
      <div class="dev">
        <div class="top"><span class="dot" id="dvOut"></span><span class="cl" data-i18n="conn.out">Output</span></div>
        <div class="cv" id="dvOutVal">—</div>
        <div class="note" data-i18n="conn.hostNote"></div>
      </div>
    </div>
  </section>

  <!-- SETTINGS -->
  <section class="view" data-view="settings" aria-labelledby="t-set">
    <button class="back" data-go="home">‹ <span data-i18n="back">Back</span></button>
    <h2 class="vtitle" id="t-set" tabindex="-1" data-i18n="nav.settings.t">Settings</h2>
    <div class="card">
      <div class="cl" data-i18n="settings_lang">Language</div>
      <div class="seg" id="langSeg2" role="group" aria-label="Language">
        <button data-lang="en" aria-pressed="true">English</button>
        <button data-lang="de" aria-pressed="false">Deutsch</button>
      </div>
      <div class="note" data-i18n="settings_langHint"></div>
    </div>
    <div class="card">
      <div class="cl" data-i18n="settings_about">About</div>
      <p data-i18n="settings_aboutText"></p>
    </div>
  </section>
  </main>
</div>

<!-- key picker -->
<div class="modal" id="picker" role="dialog" aria-modal="true" aria-labelledby="pkTitle" hidden>
  <div class="sheet">
    <div class="pkhead">
      <h2 id="pkTitle" data-i18n="picker.title">Assign a key</h2>
      <button class="pkx" id="pkClose" aria-label="Cancel" data-i18n-aria="picker.cancel">✕</button>
    </div>
    <div class="pkhint" data-i18n="picker.hint">Tap a key…</div>
    <div class="pklbl" data-i18n="picker.special">Special</div>
    <div class="pkgrid wide" id="pkSpecial"></div>
    <div class="pklbl" data-i18n="picker.letters">Letters</div>
    <div class="pkgrid" id="pkLetters"></div>
    <div class="pklbl" data-i18n="picker.numbers">Numbers</div>
    <div class="pkgrid" id="pkNumbers"></div>
    <button class="pkclear" id="pkClear" data-i18n="picker.clear">Clear (no key)</button>
  </div>
</div>

<div class="toast" id="toast" role="status" aria-live="polite" data-i18n="saved">Saved</div>

<script>
// Button table (MAKINOLO Zwift Ride protocol, proto2, msg-id 0x23): [id, hex, group]
const BUTTONS = [
  ["LEFT_BTN","0x01","dpad"],["UP_BTN","0x02","dpad"],
  ["RIGHT_BTN","0x04","dpad"],["DOWN_BTN","0x08","dpad"],
  ["A_BTN","0x10","action"],["B_BTN","0x20","action"],
  ["Y_BTN","0x40","action"],["Z_BTN","0x80","action"],
  ["SHFT_UP_L_BTN","0x100","left"],["SHFT_DN_L_BTN","0x200","left"],["AUX_L_BTN","0x400","left"],
  ["SHFT_UP_R_BTN","0x1000","right"],["SHFT_DN_R_BTN","0x2000","right"],["AUX_R_BTN","0x4000","right"],
  ["STEER_LL_BTN","analog","levers"],["STEER_LR_BTN","analog","levers"],
  ["STEER_RL_BTN","analog","levers"],["STEER_RR_BTN","analog","levers"],
];
const GROUPS = ["dpad","action","left","right","levers"];
// inline icons (stroke=currentColor) — consistent set, no mixed unicode glyphs
const ICONS = {
  mapping:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6"><rect x="2.5" y="6" width="19" height="12" rx="2.5"/><path d="M6 9.5h.01M9 9.5h.01M12 9.5h.01M15 9.5h.01M18 9.5h.01M7.5 14h9" stroke-linecap="round"/></svg>',
  history:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6"><path d="M3.5 12a8.5 8.5 0 1 0 2.6-6.1" stroke-linecap="round"/><path d="M5.5 3.5v3h3" stroke-linecap="round" stroke-linejoin="round"/><path d="M12 7.5V12l3 2" stroke-linecap="round" stroke-linejoin="round"/></svg>',
  devices:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M4 8h13l-3-3M20 16H7l3 3"/></svg>',
  settings:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6"><circle cx="12" cy="12" r="3.2"/><path d="M12 2.5v3M12 18.5v3M2.5 12h3M18.5 12h3M5 5l2.1 2.1M16.9 16.9 19 19M19 5l-2.1 2.1M7.1 16.9 5 19" stroke-linecap="round"/></svg>',
  controller:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6"><path d="M8 8h8a5 5 0 0 1 5 5 3 3 0 0 1-5.5 1.7l-.7-1H9.2l-.7 1A3 3 0 0 1 3 13a5 5 0 0 1 5-5z"/><path d="M7 11v2.2M5.9 12.1h2.2" stroke-linecap="round"/><circle cx="15.5" cy="11.4" r=".7" fill="currentColor" stroke="none"/><circle cx="17.4" cy="13" r=".7" fill="currentColor" stroke="none"/></svg>'
};
// Statisches Cockpit-SVG: zusammenhängende Lenker-Illustration (symmetrisch),
// jede Taste ist ein <g class="cbtn" data-btn="ID"> das per .on aufleuchtet.
// viewBox 0 0 480 320. Styles liegen im eigenen <style> des SVG.
const CTRL_SVG = `<style>
.kc{fill:#0e1622;stroke:#2a3a4e;stroke-width:1.5}
.kl{fill:#94a3b8;font:700 13px -apple-system,Helvetica,Arial,sans-serif;text-anchor:middle;dominant-baseline:central}
.cbtn.on .kc{fill:#fc6719;stroke:#ff812e}
.cbtn.on .kl{fill:#160a00}
.cbtn.lever .kc{fill:#3a2512;stroke:#7c4a1f}
.cbtn.lever .kl{fill:#e8a06a}
.cbtn.lever.on .kc{fill:#fc6719}
.cbtn.lever.on .kl{fill:#160a00}
.seclabel{fill:#6c7c92;font:700 9px -apple-system,sans-serif;letter-spacing:.1em;text-transform:uppercase;text-anchor:middle}
</style>
<defs><linearGradient id="pod" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#1b2737"/><stop offset="1" stop-color="#121b27"/></linearGradient></defs>
<rect x="60" y="40" width="360" height="16" rx="8" fill="#0c131d" stroke="#22303f" stroke-width="1.5"/>
<rect x="113" y="52" width="14" height="26" rx="4" fill="#0c131d" stroke="#22303f" stroke-width="1.5"/>
<rect x="353" y="52" width="14" height="26" rx="4" fill="#0c131d" stroke="#22303f" stroke-width="1.5"/>
<rect x="40" y="70" width="160" height="220" rx="26" fill="url(#pod)" stroke="#2a3a4e" stroke-width="2"/>
<rect x="280" y="70" width="160" height="220" rx="26" fill="url(#pod)" stroke="#2a3a4e" stroke-width="2"/>
<text class="seclabel" x="120" y="86">{{dpad}}</text>
<g class="cbtn" data-btn="UP_BTN"><rect class="kc" x="107" y="91" width="26" height="26" rx="6"/><text class="kl" x="120" y="104">▲</text></g>
<g class="cbtn" data-btn="LEFT_BTN"><rect class="kc" x="80" y="118" width="26" height="26" rx="6"/><text class="kl" x="93" y="131">◀</text></g>
<g class="cbtn" data-btn="RIGHT_BTN"><rect class="kc" x="134" y="118" width="26" height="26" rx="6"/><text class="kl" x="147" y="131">▶</text></g>
<g class="cbtn" data-btn="DOWN_BTN"><rect class="kc" x="107" y="145" width="26" height="26" rx="6"/><text class="kl" x="120" y="158">▼</text></g>
<text class="seclabel" x="120" y="186">{{shifters}}</text>
<g class="cbtn" data-btn="SHFT_UP_L_BTN"><rect class="kc" x="62" y="192" width="50" height="26" rx="6"/><text class="kl" x="87" y="205" style="font-size:10px">{{shiftUp}}</text></g>
<g class="cbtn" data-btn="SHFT_DN_L_BTN"><rect class="kc" x="128" y="192" width="50" height="26" rx="6"/><text class="kl" x="153" y="205" style="font-size:10px">{{shiftDn}}</text></g>
<g class="cbtn lever" data-btn="STEER_LL_BTN"><rect class="kc" x="56" y="228" width="61" height="32" rx="9"/><text class="kl" x="86" y="244">◀</text></g>
<g class="cbtn lever" data-btn="STEER_LR_BTN"><rect class="kc" x="123" y="228" width="61" height="32" rx="9"/><text class="kl" x="153" y="244">▶</text></g>
<g class="cbtn" data-btn="AUX_L_BTN"><circle class="kc" cx="120" cy="277" r="13"/><text class="kl" x="120" y="277" style="font-size:7px">{{aux}}</text></g>
<text class="seclabel" x="360" y="86">{{buttons}}</text>
<g class="cbtn" data-btn="Y_BTN"><circle class="kc" cx="360" cy="104" r="13"/><text class="kl" x="360" y="104">Y</text></g>
<g class="cbtn" data-btn="Z_BTN"><circle class="kc" cx="333" cy="131" r="13"/><text class="kl" x="333" y="131">Z</text></g>
<g class="cbtn" data-btn="A_BTN"><circle class="kc" cx="387" cy="131" r="13"/><text class="kl" x="387" y="131">A</text></g>
<g class="cbtn" data-btn="B_BTN"><circle class="kc" cx="360" cy="158" r="13"/><text class="kl" x="360" y="158">B</text></g>
<text class="seclabel" x="360" y="186">{{shifters}}</text>
<g class="cbtn" data-btn="SHFT_UP_R_BTN"><rect class="kc" x="302" y="192" width="50" height="26" rx="6"/><text class="kl" x="327" y="205" style="font-size:10px">{{shiftUp}}</text></g>
<g class="cbtn" data-btn="SHFT_DN_R_BTN"><rect class="kc" x="368" y="192" width="50" height="26" rx="6"/><text class="kl" x="393" y="205" style="font-size:10px">{{shiftDn}}</text></g>
<g class="cbtn lever" data-btn="STEER_RL_BTN"><rect class="kc" x="296" y="228" width="61" height="32" rx="9"/><text class="kl" x="326" y="244">◀</text></g>
<g class="cbtn lever" data-btn="STEER_RR_BTN"><rect class="kc" x="363" y="228" width="61" height="32" rx="9"/><text class="kl" x="393" y="244">▶</text></g>
<g class="cbtn" data-btn="AUX_R_BTN"><circle class="kc" cx="360" cy="277" r="13"/><text class="kl" x="360" y="277" style="font-size:7px">{{aux}}</text></g>`;
const TILES = [["mapping",ICONS.mapping],["history",ICONS.history],["devices",ICONS.devices],["settings",ICONS.settings]];
// client-side starting-point presets (user reviews + saves)
const PRESETS = {
  MyWhoosh: {SHFT_UP_L_BTN:"i",SHFT_DN_L_BTN:"k",SHFT_UP_R_BTN:"i",SHFT_DN_R_BTN:"k",
    LEFT_BTN:"LEFT",RIGHT_BTN:"RIGHT",UP_BTN:"UP",DOWN_BTN:"DOWN"},
  // Click V2: je Seite nur EIN Paddle, beide melden "UP" (links=Bit 8, rechts=Bit 12).
  // SRAM-Logik: links leichter, rechts schwerer.
  // Shift-Tasten getestet (MyWhoosh): "i" macht es LEICHTER, "k" SCHWERER - die
  // MyWhoosh-Doku nennt I = Shift Up, das stimmt hier nicht. Daher links=i (leichter),
  // rechts=k (schwerer, "+" wie bei SRAM).
  // Links: Pfeiltasten (Lenken/Navigation). Rechts: nur Tasten, die MyWhoosh
  // wirklich kennt (Emotes 1-7, U = minimale UI): Y=Minimal UI, Z=Wave,
  // A=Thumbs up, B=Fist bump.
  MyWhooshClickV2: {SHFT_UP_L_BTN:"i",SHFT_UP_R_BTN:"k",
    LEFT_BTN:"LEFT",RIGHT_BTN:"RIGHT",UP_BTN:"UP",DOWN_BTN:"DOWN",
    Y_BTN:"u",Z_BTN:"2",A_BTN:"7",B_BTN:"3"},
  Zwift: {SHFT_UP_L_BTN:"UP",SHFT_DN_L_BTN:"DOWN",SHFT_UP_R_BTN:"UP",SHFT_DN_R_BTN:"DOWN",
    LEFT_BTN:"LEFT",RIGHT_BTN:"RIGHT",UP_BTN:"UP",DOWN_BTN:"DOWN",A_BTN:"SPACE"}
};
// left/right mirror pairs are intentionally identical -> not a conflict
const MIRROR = {SHFT_UP_L_BTN:"SU",SHFT_UP_R_BTN:"SU",SHFT_DN_L_BTN:"SD",SHFT_DN_R_BTN:"SD",
  AUX_L_BTN:"AUX",AUX_R_BTN:"AUX",
  STEER_LL_BTN:"SL",STEER_RL_BTN:"SL",STEER_LR_BTN:"SR",STEER_RR_BTN:"SR"};

const $ = id => document.getElementById(id);
let ws, mapping = {}, T = {}, dirty = false, pickerTarget = null;
let LANG = localStorage.getItem("lang") || "en";
let lastState = {ride:false,hid:false,rideDev:"",hidDev:"",bat:-1};
let liveSet = new Set(), lastNames = [];

const escHtml = s => String(s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
const tp = path => path.split(".").reduce((o,k)=>o&&o[k], T);
const prettyName = id => (T.btn && T.btn[id]) || id;
const keyLabel = tok => !tok ? (tp("tap_to_assign")||"") :
  (tok==="UP"?"↑":tok==="DOWN"?"↓":tok==="LEFT"?"←":tok==="RIGHT"?"→":tok);

// ---------- i18n ----------
async function loadLang(l){
  try{ const r = await fetch("/i18n/"+l+".json",{cache:"no-store"}); if(!r.ok) throw 0; T = await r.json(); }
  catch(e){ if(l!=="en") return loadLang("en"); }
  LANG = l; localStorage.setItem("lang", l);
  applyI18n();
}
function applyI18n(){
  document.documentElement.lang = LANG;
  document.querySelectorAll("[data-i18n]").forEach(el=>{ const v=tp(el.dataset.i18n); if(v!=null) el.textContent=v; });
  document.querySelectorAll("[data-i18n-aria]").forEach(el=>{ const v=tp(el.dataset.i18nAria); if(v!=null) el.setAttribute("aria-label",v); });
  document.querySelectorAll("[data-lang]").forEach(b=>b.setAttribute("aria-pressed", b.dataset.lang===LANG?"true":"false"));
  buildTiles(); buildGrid(); buildPicker(); buildController(); renderState(); showLive(lastNames); updateSave();
}

// ---------- views ----------
function showView(v){
  document.querySelectorAll(".view").forEach(el=>el.classList.toggle("show", el.dataset.view===v));
  window.scrollTo(0,0);
  const t = document.querySelector('.view[data-view="'+v+'"] .vtitle'); if(t) t.focus();
}
function buildTiles(){
  const g=$("tiles"); g.innerHTML="";
  for(const [id,ic] of TILES){
    const b=document.createElement("button"); b.className="tile"; b.dataset.go=id;
    b.innerHTML='<span class="tic" aria-hidden="true">'+ic+'</span>'+
      '<span class="tt">'+escHtml(tp("nav."+id+".t")||id)+'</span>'+
      '<span class="td">'+escHtml(tp("nav."+id+".d")||"")+'</span>';
    g.appendChild(b);
  }
}

// ---------- controller live map ----------
function buildController(){
  const svg=$("ctrlSvg"); if(!svg) return;
  const L={dpad:tp("ctrl.dpad")||"D-Pad", buttons:tp("ctrl.buttons")||"Buttons",
    shifters:tp("ctrl.shifters")||"Shift", shiftUp:tp("ctrl.shiftUp")||"Shift ▲",
    shiftDn:tp("ctrl.shiftDn")||"Shift ▼", aux:tp("ctrl.aux")||"Aux"};
  svg.innerHTML = CTRL_SVG.replace(/\{\{(\w+)\}\}/g, (_,k)=>escHtml(L[k]||k));
  highlightController(new Set(lastNames));
}
function highlightController(set){
  const svg=$("ctrlSvg"); if(!svg) return;
  svg.querySelectorAll(".cbtn").forEach(g=>g.classList.toggle("on", set.has(g.dataset.btn)));
}

// ---------- mapping grid ----------
function buildGrid(){
  const g=$("groups"); g.innerHTML="";
  for(const grp of GROUPS){
    const sec=document.createElement("div"); sec.className="group";
    sec.innerHTML='<h3><span class="ic" aria-hidden="true"></span>'+escHtml(tp("groups."+grp)||grp)+'</h3><div class="rows"></div>';
    const rows=sec.querySelector(".rows");
    for(const [id,hex,group] of BUTTONS){
      if(group!==grp) continue;
      const row=document.createElement("div"); row.className="row"; row.dataset.btn=id;
      row.innerHTML='<div class="bn"><span class="id">'+escHtml(prettyName(id))+
          '</span><span class="cf" data-i18n="conflict">'+escHtml(tp("conflict")||"")+'</span></div>'+
        '<button class="cap" data-btn="'+id+'" type="button"></button>';
      rows.appendChild(row);
    }
    g.appendChild(sec);
  }
  g.querySelectorAll(".cap").forEach(c=>c.addEventListener("click",()=>openPicker(c.dataset.btn)));
  applyMap(mapping);
}
function applyMap(m){
  if(m) mapping = m;
  document.querySelectorAll("#groups .cap").forEach(cap=>{
    const v = mapping[cap.dataset.btn] || "";
    cap.textContent = keyLabel(v);
    cap.classList.toggle("empty", !v);
    cap.setAttribute("aria-label", prettyName(cap.dataset.btn) + ": " + (v ? keyLabel(v) : (tp("tap_to_assign")||"")));
  });
  markConflicts();
}
function assignKey(id, tok){            // tok "" = clear
  mapping[id] = tok;
  send({t:"setmap",btn:id,key:tok});
  setDirty(true);
  applyMap(mapping);
}
function applyPreset(name){
  const p = PRESETS[name]; if(!p) return;
  for(const [id] of BUTTONS){ const v = p[id]||""; mapping[id]=v; send({t:"setmap",btn:id,key:v}); }
  setDirty(true); applyMap(mapping);
}
function markConflicts(){
  const byVal={};
  for(const [id] of BUTTONS){ const v=mapping[id]; if(!v) continue; const c=MIRROR[id]||id;
    (byVal[v]=byVal[v]||new Set()).add(c); }
  document.querySelectorAll("#groups .row").forEach(r=>{
    const v=mapping[r.dataset.btn]; const dup=v && byVal[v] && byVal[v].size>1;
    r.classList.toggle("conflict", !!dup);
    const cap=r.querySelector(".cap"); if(cap) cap.setAttribute("aria-invalid", dup?"true":"false");
  });
}

// ---------- key picker ----------
function buildPicker(){
  const special=[["LEFT","←"],["UP","↑"],["DOWN","↓"],["RIGHT","→"],["SPACE","Space"],["ENTER","Enter"],["ESC","Esc"],["TAB","Tab"]];
  const sp=$("pkSpecial"); sp.innerHTML="";
  for(const [tok,lab] of special){ sp.appendChild(pkBtn(tok,lab)); }
  const le=$("pkLetters"); le.innerHTML="";
  for(let c=97;c<=122;c++){ const ch=String.fromCharCode(c); le.appendChild(pkBtn(ch,ch.toUpperCase())); }
  const nu=$("pkNumbers"); nu.innerHTML="";
  for(let n=0;n<=9;n++){ nu.appendChild(pkBtn(String(n),String(n))); }
}
function pkBtn(tok,lab){
  const b=document.createElement("button"); b.className="pkkey"; b.type="button";
  b.textContent=lab; b.addEventListener("click",()=>{ if(pickerTarget) assignKey(pickerTarget,tok); closePicker(); });
  return b;
}
function openPicker(id){
  pickerTarget=id;
  const m=$("picker"); m.hidden=false; m.classList.add("show");
  $("pkTitle").textContent=(tp("picker.title")||"Assign")+" — "+prettyName(id);
  $("pkClose").focus();
}
function closePicker(){ const m=$("picker"); m.classList.remove("show"); m.hidden=true; pickerTarget=null; }

// ---------- live + history ----------
function showLive(names){
  const set=new Set(names);
  document.querySelectorAll("#groups .row").forEach(r=>r.classList.toggle("active", set.has(r.dataset.btn)));
  const lk=$("liveKey"); if(!lk) return;
  const keys=names.map(n=>mapping[n]).filter(Boolean);
  if(keys.length) lk.innerHTML = escHtml(tp("live.kbd")||"")+" → <b>"+keys.map(k=>escHtml(keyLabel(k))).join("</b> <b>")+"</b>";
  else lk.textContent = names.length ? (tp("live.noKey")||"") : "";
}
function flashButtons(names){
  const set=new Set(names);
  for(const n of names) if(!liveSet.has(n)) logPress(n);
  liveSet=set; lastNames=names; showLive(names); highlightController(set);
}
function span(c,t){ const s=document.createElement("span"); s.className=c; s.textContent=t; return s; }
function logPress(id){
  const le=$("logEmpty"); if(le) le.style.display="none";
  const row=document.createElement("div"); row.className="lrow";
  row.append(span("lt",new Date().toLocaleTimeString()), span("lb",prettyName(id)),
    span("la","→"), span("lk",keyLabel(mapping[id])||"·"));
  const log=$("log"); log.insertBefore(row, log.firstChild);
  const rows=log.querySelectorAll(".lrow"); for(let i=50;i<rows.length;i++) rows[i].remove();
}

// ---------- connection state ----------
function setDot(id,on,searching){ const d=$(id); d.classList.toggle("on",!!on); d.classList.toggle("searching",!!searching&&!on); }
function renderState(){
  const s=lastState, conn=tp("conn.connected")||"", disc=tp("conn.disc")||"";
  setDot("dRide",s.ride); setDot("dHid",s.hid);
  setDot("dvIn",s.ride);  setDot("dvOut",s.hid);
  $("pIn").classList.toggle("on",s.ride); $("pOut").classList.toggle("on",s.hid);
  $("stRide").textContent=s.ride?conn:disc; $("stHid").textContent=s.hid?conn:disc;
  $("pIn").setAttribute("aria-label",(tp("conn.in")||"")+": "+(s.ride?conn:disc));
  $("pOut").setAttribute("aria-label",(tp("conn.out")||"")+": "+(s.hid?conn:disc));
  const iv=$("dvInVal"), ov=$("dvOutVal");
  iv.textContent=s.ride?((s.rideDev||tp("conn.inFb")||"")+(s.bat>=0?" · "+s.bat+" %":"")):disc; iv.classList.toggle("on",s.ride);
  ov.textContent=s.hid?(s.hidDev||tp("conn.outFb")||""):disc; ov.classList.toggle("on",s.hid);
  // setup checklist (home) — show until both connected
  $("step-kbd").classList.toggle("done",s.hid);
  $("step-ride").classList.toggle("done",s.ride);
  $("setup").classList.toggle("show", !(s.ride&&s.hid));
}

// ---------- dirty / save ----------
function setDirty(v){ dirty=v; updateSave(); }
function updateSave(){
  const btn=$("btnSave"); if(!btn) return;
  btn.classList.toggle("dirty",dirty);
  $("saveTxt").textContent = dirty ? (tp("saveChanges")||"Save changes") : (tp("save")||"Save");
}
window.addEventListener("beforeunload",e=>{ if(dirty){ e.preventDefault(); e.returnValue=""; } });

// ---------- websocket ----------
function send(o){ if(ws&&ws.readyState===1) ws.send(JSON.stringify(o)); }
function connect(){
  ws=new WebSocket("ws://"+location.hostname+"/ws");
  ws.onopen=()=>{ $("banner").classList.remove("show"); send({t:"getmap"}); };
  ws.onmessage=e=>{ let m; try{ m=JSON.parse(e.data); }catch(_){ return; }
    if(m.t==="state"){ lastState={ride:!!m.ride,hid:!!m.hid,rideDev:m.rideDev||"",hidDev:m.hidDev||"",bat:(typeof m.bat==="number"?m.bat:-1)}; renderState(); }
    else if(m.t==="btn"){ flashButtons(m.names||[]); }
    else if(m.t==="map"){ applyMap(m.map||{}); setDirty(false); } };
  ws.onclose=()=>{ lastState={ride:false,hid:false,rideDev:"",hidDev:"",bat:-1}; renderState();
    $("banner").classList.add("show"); setTimeout(connect,1200); };
}

// ---------- events ----------
document.addEventListener("click",e=>{
  const go=e.target.closest("[data-go]"); if(go){ showView(go.dataset.go); return; }
  const lg=e.target.closest("[data-lang]"); if(lg){ loadLang(lg.dataset.lang); return; }
  const pr=e.target.closest("[data-preset]"); if(pr){ applyPreset(pr.dataset.preset); return; }
  if(e.target===$("picker")) closePicker();
});
document.addEventListener("keydown",e=>{
  if($("picker").classList.contains("show")){
    if(e.key==="Escape"){ closePicker(); return; }
    // let Enter/Space activate a focused picker button natively (a11y)
    if(e.target.closest(".sheet") && (e.key==="Enter"||e.key===" ")) return;
    const k=normKey(e); if(k!==null){ e.preventDefault();
      if(pickerTarget) assignKey(pickerTarget, k==="__clear__"?"":k); closePicker(); }
  }
});
function normKey(ev){
  const k=ev.key;
  if(k==="Backspace"||k==="Delete") return "__clear__";
  if(k==="ArrowUp") return "UP"; if(k==="ArrowDown") return "DOWN";
  if(k==="ArrowLeft") return "LEFT"; if(k==="ArrowRight") return "RIGHT";
  if(k===" ") return "SPACE"; if(k==="Enter") return "ENTER";
  if(k==="Escape") return "ESC"; if(k==="Tab") return "TAB";
  if(k.length===1) return k.toLowerCase();
  return null;
}
$("pkClose").addEventListener("click",closePicker);
$("pkClear").addEventListener("click",()=>{ if(pickerTarget) assignKey(pickerTarget,""); closePicker(); });
$("btnSave").addEventListener("click",()=>{ send({t:"save"}); setDirty(false);
  const t=$("toast"); t.classList.add("show"); setTimeout(()=>t.classList.remove("show"),1400); });
$("btnReload").addEventListener("click",()=>{ if(dirty && !confirm(tp("discard")||"Discard?")) return; send({t:"getmap"}); });

// ---------- boot ----------
loadLang(LANG);
connect();
</script>
</body>
</html>
)HTMLDELIM";
