/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
(() => {
  'use strict';
  if (typeof document === 'undefined') return;
  const $ = id => document.getElementById(id);

  const PAGES = [
    { name: 'VOICE',    knobs: ['FOLD', 'RATIO', 'FINE', 'WAVE'] },
    { name: 'MOTION',   knobs: ['RISE', 'FALL', 'LOOP', 'DEPTH'] },
    { name: 'GATE',     knobs: ['LEVEL', 'SOFT', 'RAND P', 'RAND T'] },
    { name: 'SEQUENCE', knobs: ['TEMPO', 'SWING', 'LENGTH', 'TRANSP'] },
  ];

  const wc = { track: 0, page: 0, values: new Uint8Array(4), visible: false, api: null };

  function waitForApi() {
    return new Promise(resolve => {
      const check = () => globalThis.FM1Easel ? resolve(globalThis.FM1Easel) : setTimeout(check, 120);
      check();
    });
  }

  function injectStyles() {
    if ($('sim-easel-styles')) return;
    const style = document.createElement('style');
    style.id = 'sim-easel-styles';
    style.textContent = `
      .easel-panel{position:absolute;right:1.5%;top:2.5%;width:42%;max-width:340px;
        background:#1a1c18f2;backdrop-filter:blur(8px);
        border:2px solid #8bc34a;border-radius:8px;padding:12px;z-index:30;
        color:#dcedc8;font:12px/1.4 system-ui,sans-serif;box-shadow:0 10px 40px #000c}
      .easel-panel[hidden]{display:none}
      .easel-header{display:flex;align-items:center;gap:8px;
        border-bottom:1px solid #33691e;padding-bottom:6px;margin-bottom:10px}
      .easel-header strong{color:#8bc34a;letter-spacing:.1em;font-size:12px;text-shadow:0 0 5px #8bc34a50}
      .easel-header span{color:#aed581;font-size:10px}
      .easel-header #easel-close{background:none;border:none;color:#aed581;
        font-size:18px;cursor:pointer;padding:0 4px;line-height:1;margin-left:auto}
      .easel-header #easel-close:hover{color:#ff8a65}
      .easel-page-nav{display:flex;gap:4px;margin-bottom:10px}
      .easel-page-nav button{flex:1;padding:6px;background:#2e3b25;color:#aed581;
        border:1px solid #33691e;border-radius:4px;cursor:pointer;font-size:10px;font-weight:bold}
      .easel-page-nav button[aria-pressed=true]{background:#558b2f;color:#fff;border-color:#8bc34a}
      .easel-knobs{display:grid;grid-template-columns:1fr 1fr;gap:8px}
      .easel-knob{background:#263238;border:1px solid #455a64;border-radius:6px;
        padding:8px;text-align:center;user-select:none;touch-action:none;cursor:ns-resize;
        box-shadow:inset 0 0 10px #0006}
      .easel-knob label{display:block;font-size:9px;color:#90a4ae;margin-bottom:2px;letter-spacing:.05em}
      .easel-knob .easel-val{font:18px/1.1 Consolas,monospace;color:#8bc34a}
      .easel-knob .easel-valbar{height:2px;background:#1a1c18;margin-top:6px;border-radius:2px;overflow:hidden}
      .easel-knob .easel-valbar>i{display:block;height:100%;background:#8bc34a;transition:width .05s}
      .easel-knob.drag{background:#37474f;border-color:#8bc34a}
      .easel-footer{display:flex;gap:6px;margin-top:12px}
      .easel-footer button{flex:1;padding:8px;background:#2e3b25;color:#aed581;
        border:1px solid #33691e;border-radius:4px;cursor:pointer;font-size:10px}
      .easel-footer button:hover{background:#558b2f;color:#fff}
    `;
    document.head.appendChild(style);
  }

  function buildPanel() {
    if ($('sim-easel-panel')) return $('sim-easel-panel');
    const device = $('sim-device');
    if (!device) return null;
    injectStyles();
    const panel = document.createElement('div');
    panel.id = 'sim-easel-panel';
    panel.className = 'easel-panel';
    panel.hidden = true;
    panel.innerHTML = `
      <div class="easel-header">
        <strong>EASEL FM</strong>
        <span id="easel-track-label">T1+T2</span>
        <button id="easel-close" aria-label="Close">×</button>
      </div>
      <div class="easel-page-nav">
        ${PAGES.map((p, i) => `<button data-easel-page="${i}" aria-pressed="${i===0}">${p.name}</button>`).join('')}
      </div>
      <div class="easel-knobs">
        ${[0,1,2,3].map(i => `
          <div class="easel-knob" data-easel-knob="${i}">
            <label>${PAGES[0].knobs[i]}</label>
            <div class="easel-val">0</div>
            <div class="easel-valbar"><i style="width:0"></i></div>
          </div>`).join('')}
      </div>
      <div class="easel-footer">
        <button id="easel-exit">Exit Easel</button>
      </div>
    `;
    device.appendChild(panel);
    return panel;
  }

  function addEaselButton() {
    if ($('sim-easel-open')) return $('sim-easel-open');
    const shortcuts = document.querySelector('#tab-sim .edit-shortcuts');
    if (!shortcuts) return null;
    const btn = document.createElement('button');
    btn.id = 'sim-easel-open';
    btn.type = 'button';
    btn.textContent = 'Easel FM';
    btn.style.background = '#33691e';
    btn.style.borderColor = '#8bc34a';
    shortcuts.appendChild(btn);
    return btn;
  }

  function render() {
    const panel = $('sim-easel-panel');
    if (!panel) return;
    const baseTrack = wc.track & ~1;
    $('easel-track-label').textContent = `T${baseTrack + 1}+T${baseTrack + 2}`;
    
    panel.querySelectorAll('[data-easel-page]').forEach(b => {
      b.setAttribute('aria-pressed', String(Number(b.dataset.easelPage) === wc.page));
    });

    panel.querySelectorAll('.easel-knob').forEach(el => {
      const i = Number(el.dataset.easelKnob);
      const raw = wc.values[i];
      el.querySelector('label').textContent = PAGES[wc.page].knobs[i];
      el.querySelector('.easel-val').textContent = raw;
      el.querySelector('.easel-valbar > i').style.width = (raw / 127 * 100) + '%';
    });
  }

  async function refreshValues() {
    if (!wc.api) return;
    try {
      wc.values = await wc.api.read(wc.track);
      render();
    } catch (e) { console.warn('[Easel] read failed:', e); }
  }

  let deltaBuf = 0, deltaTimer = 0, lastKnob = 0;
  async function flushDelta() {
    const d = deltaBuf; deltaBuf = 0; deltaTimer = 0;
    if (!wc.api || !d) return;
    try { await wc.api.adjust(wc.track, lastKnob, d); await refreshValues(); }
    catch (e) { console.warn('[Easel] adjust failed:', e); }
  }
  function queueDelta(knob, delta) {
    lastKnob = knob;
    deltaBuf += delta;
    if (!deltaTimer) deltaTimer = setTimeout(flushDelta, 40);
  }

  function wireKnob(el, knob) {
    let lastY = 0, moved = false;
    el.addEventListener('pointerdown', e => {
      if (e.button !== 0) return;
      e.preventDefault();
      lastY = e.clientY; moved = false;
      el.classList.add('drag');
      el.setPointerCapture(e.pointerId);
    });
    el.addEventListener('pointermove', e => {
      if (!el.hasPointerCapture(e.pointerId)) return;
      const d = Math.trunc((lastY - e.clientY) / 4);
      if (d) { moved = true; lastY -= d * 4; queueDelta(knob, d); }
    });
    const up = e => {
      if (el.hasPointerCapture(e.pointerId)) el.releasePointerCapture(e.pointerId);
      el.classList.remove('drag');
      if (!moved) queueDelta(knob, 1);
    };
    el.addEventListener('pointerup', up);
    el.addEventListener('pointercancel', up);
    el.addEventListener('wheel', e => {
      if (!e.deltaY || e.ctrlKey || e.metaKey) return;
      e.preventDefault();
      queueDelta(knob, e.deltaY < 0 ? 1 : -1);
    }, { passive: false });
  }

  function wirePanel(panel) {
    if (panel.dataset.wired) return;
    panel.dataset.wired = '1';
    $('easel-close').onclick = $('easel-exit').onclick = closePanel;
    panel.querySelectorAll('[data-easel-page]').forEach(btn => {
      btn.onclick = async () => {
        wc.page = Number(btn.dataset.easelPage);
        if (wc.api) { try { await wc.api.setPage(wc.track, wc.page); } catch (_) {} }
        render();
        refreshValues();
      };
    });
    panel.querySelectorAll('.easel-knob').forEach(el => {
      wireKnob(el, Number(el.dataset.easelKnob));
    });
  }

  async function openPanel() {
    const api = await waitForApi();
    wc.api = api;
    
    // Get current track from FM1
    const t = await api.currentTrack();
    wc.track = t & ~1; // Force even base track
    
    const startBtn = document.getElementById('sim-start');
    if (startBtn && startBtn.getAttribute('aria-pressed') === 'false') startBtn.click();

    try { 
      await api.enter(wc.track); 
      // Explicitly select the base track so the UI follows
      await api.select(wc.track);
    }
    catch (e) {
      console.error("Easel enter error:", e);
      alert('Easel FM failed to initialize. Make sure tracks are available.');
      return;
    }

    const panel = buildPanel();
    if (!panel) return;
    wc.page = await api.page(wc.track);
    wc.visible = true;
    panel.hidden = false;
    wirePanel(panel);
    await refreshValues();
  }

  async function closePanel() {
    const panel = $('sim-easel-panel');
    if (panel) panel.hidden = true;
    wc.visible = false;
    if (wc.api) { try { await wc.api.leave(wc.track); } catch (_) {} }
  }

  async function main() {
    const btn = addEaselButton();
    if (!btn) { setTimeout(main, 300); return; }
    btn.disabled = true;
    await waitForApi();
    btn.disabled = false;
    btn.onclick = openPanel;
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', main);
  else main();
})();
