// Battery card (the home page's main card): live data from main/features/bms.c on
// WebSocket topic 'bms' (sent after every poll), history charts from
// GET /api/bms/history, and the Settings section (poll interval, pinned
// address, pairing passkey, pause for the FSolar app) under Settings → Battery.
import { lineChart } from '/chart.js';

const CSS = `
.bms-top { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 12px; }
.bms-tile { border: 1px solid var(--line); border-radius: 10px; padding: 10px 12px; min-width: 0; }
.bms-tile dt { font-size: 12px; color: var(--muted); text-transform: uppercase; letter-spacing: .04em; }
.bms-tile dd { text-align: left; margin: 2px 0 0; font-size: 26px; font-weight: 650; font-variant-numeric: tabular-nums; white-space: nowrap; }
.bms-tile dd small { font-size: 14px; font-weight: 500; color: var(--muted); margin-left: 2px; }
.bms-tile .sub { font-size: 12px; margin-top: 2px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.bms-tile .meter { margin-top: 6px; }
.chg { color: var(--ok); } .dis { color: var(--warn); }
.bms-st { color: var(--muted); font-size: 13px; margin: 12px 0 4px; }
.bms-cells { display: grid; grid-template-columns: repeat(8, minmax(0, 1fr)); gap: 6px; margin-top: 14px; }
.bms-cell { border: 1px solid var(--line); border-radius: 8px; padding: 5px 7px; font-variant-numeric: tabular-nums; }
.bms-cell b { display: block; font-size: 14px; font-weight: 600; }
.bms-cell span { color: var(--muted); font-size: 11px; }
.bms-cell.min { border-color: var(--warn); } .bms-cell.max { border-color: var(--accent); }
.bms-sum { color: var(--muted); font-size: 13px; margin-top: 8px; font-variant-numeric: tabular-nums; }
.bms-charts { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 24px; margin-top: 20px; }
.bms-charts h3 { font-size: 13px; font-weight: 600; color: var(--muted); margin: 0 0 14px; }
.bms-alert { display: none; margin-top: 12px; }
@media (max-width: 720px) {
  .bms-top { grid-template-columns: repeat(2, minmax(0, 1fr)); }
  .bms-cells { grid-template-columns: repeat(4, minmax(0, 1fr)); }
  .bms-charts { grid-template-columns: 1fr; }
}
`;

const f = (v, d) => (typeof v === 'number' ? v.toFixed(d) : '–');

function span(intervalS, len) {
  const m = Math.round(intervalS * len / 60);
  return m >= 120 ? `-${Math.round(m / 60)}h` : `-${m}m`;
}

export default function (App) {
  if (!document.getElementById('bms-css')) {
    const s = document.createElement('style');
    s.id = 'bms-css';
    s.textContent = CSS;
    document.head.appendChild(s);
  }

  App.card({
    id: 'bms', title: 'Battery', order: 5, span: 3,
    mount(el, App) {
      el.innerHTML = `
        <dl class="bms-top">
          <div class="bms-tile"><dt>State of charge</dt><dd><span data-k="soc">–</span><small>%</small></dd>
            <div class="meter"><div data-k="bar" style="width:0"></div></div>
            <div class="sub" data-k="ah">&nbsp;</div></div>
          <div class="bms-tile"><dt>Power</dt><dd><span data-k="w">–</span><small>W</small></dd>
            <div class="sub" data-k="dir">&nbsp;</div></div>
          <div class="bms-tile"><dt>Voltage</dt><dd><span data-k="v">–</span><small>V</small></dd>
            <div class="sub" data-k="temps">&nbsp;</div></div>
          <div class="bms-tile"><dt>Current</dt><dd><span data-k="a">–</span><small>A</small></dd>
            <div class="sub" data-k="wf">&nbsp;</div></div>
        </dl>
        <div class="msg err bms-alert" data-k="alert"></div>
        <div class="bms-cells" data-k="cells"></div>
        <div class="bms-sum" data-k="sum"></div>
        <div class="bms-sum" data-k="lim"></div>
        <div class="bms-charts">
          <div><h3>State of charge (%)</h3><div data-k="c-soc"></div></div>
          <div><h3>Power (W, + charging)</h3><div data-k="c-pw"></div></div>
          <div><h3>Cell delta (mV)</h3><div data-k="c-dm"></div></div>
        </div>
        <div class="bms-st"><span class="dot" data-k="dot"></span><span data-k="state">waiting for first poll</span></div>
        <div class="bms-st" data-k="pickhint" hidden>No battery in range. <a href="#/settings/battery">Choose a battery</a></div>`;
      const $ = k => el.querySelector(`[data-k="${k}"]`);

      const LEN = 240;
      const opts = (label, decimals, minSpan) => ({ points: LEN, height: 110, label, decimals, minSpan, xLabels: ['', '', 'now'] });
      const charts = {
        soc: lineChart($('c-soc'), opts('State of charge', 0, 2)),
        power: lineChart($('c-pw'), opts('Power', 0, 50)),
        delta_mv: lineChart($('c-dm'), opts('Cell delta', 0, 5)),
      };
      for (const c of Object.values(charts)) c.message('Collecting…');
      let lastOk = null;

      async function loadHistory() {
        try {
          const h = await App.api('/api/bms/history');
          for (const k of Object.keys(charts)) charts[k].set(h[k] || []);
          const xl = [span(h.interval_s, h.len), span(h.interval_s, h.len / 2), 'now'];
          el.querySelectorAll('.bms-charts .chart-x').forEach(x => {
            x.querySelectorAll('span').forEach((s, i) => { s.textContent = xl[i] ?? ''; });
          });
        } catch { /* charts keep their message */ }
      }

      App.on('bms', d => {
        const s = d.sample;
        $('dot').className = 'dot ' + (d.ok ? 'ok' : d.polls_err ? 'err' : '');
        const who = [d.name, d.info && `version ${d.info.sw} · master ${d.info.fw} · IAP ${d.info.iap}`].filter(Boolean).join(' · ');
        const st = d.state === 'paused' ? `paused for ${Math.ceil(d.paused_s / 60)} min`
          : d.ok ? `updated ${s ? s.age_s : '?'} s ago, every ${d.interval_s} s` : (d.error || d.state);
        $('state').textContent = [st, who].filter(Boolean).join(' · ');
        const lost = !d.ok && /no BMS found/.test(d.error || '');
        $('pickhint').hidden = !lost;
        if (!s) return;
        $('soc').textContent = f(s.soc, 0);
        $('bar').style.width = Math.max(0, Math.min(100, s.soc)) + '%';
        $('ah').textContent = `${f(s.remaining_ah, 0)} of ${f(s.capacity_ah, 0)} Ah`;
        $('w').textContent = f(Math.abs(s.power), 0);
        const dir = s.current > 0.5 ? ['charging', 'chg'] : s.current < -0.5 ? ['discharging', 'dis'] : ['idle', ''];
        $('dir').textContent = dir[0];
        $('dir').className = 'sub ' + dir[1];
        $('v').textContent = f(s.voltage, 2);
        $('a').textContent = f(s.current, 1);
        $('temps').textContent = s.temps.length ? s.temps.map(t => f(t, 0)).join(' / ') + ' °C' : 'no temperature';
        const bank = s.bank_warn || s.bank_fault;
        $('wf').textContent = s.warn || s.fault ? `warn ${s.warn} · fault ${s.fault}` : bank ? 'warning in bank' : 'no warnings';
        $('wf').className = 'sub' + (s.warn || s.fault || bank ? ' dis' : '');
        $('alert').style.display = s.warn || s.fault || bank ? 'block' : 'none';
        $('alert').textContent = s.warn || s.fault ? `This pack reports warning ${s.warn}, fault ${s.fault}`
          : `Another pack in the bank reports warning ${s.bank_warn}, fault ${s.bank_fault}`;
        const l = s.limits;
        $('lim').textContent = [
          l && `Charge limit ${f(l.charge_v, 1)} V / ${f(l.charge_a, 0)} A · discharge limit ${f(l.discharge_v, 1)} V / ${f(l.discharge_a, 0)} A`,
          typeof s.soh === 'number' && `SoH ${f(s.soh, 0)} %`,
        ].filter(Boolean).join(' · ');
        const lo = s.cells.indexOf(s.cell_min), hi = s.cells.indexOf(s.cell_max);
        $('cells').innerHTML = s.cells.map((c, i) =>
          `<div class="bms-cell${i === lo ? ' min' : i === hi ? ' max' : ''}"><span>${i + 1}</span><b>${f(c, 3)}</b></div>`).join('');
        $('sum').textContent = `${s.cells.length} cells · min ${f(s.cell_min, 3)} V · max ${f(s.cell_max, 3)} V · ` +
          `delta ${f(s.cell_delta * 1000, 0)} mV · BLE ${s.rssi} dBm`;
        // Each successful poll adds one point (the history load covers the ones before).
        if (lastOk !== null && d.polls_ok > lastOk) {
          charts.soc.push(s.soc); charts.power.push(s.power); charts.delta_mv.push(s.cell_delta * 1000);
        }
        lastOk = d.polls_ok;
      });
      App.on('$open', () => { App.send('bms'); loadHistory(); });
      loadHistory();
      App.send('bms');
    },
  });

  // Settings → Battery → Connection. The battery list comes from POST /api/bms/scan (an 8 s
  // BLE search the board runs between polls), polled through GET /api/bms/scan.
  let stored = {}, root = null, ctxRef = null;   // not this.el: app.js keeps the section's own element there
  let sel = '';             // chosen address; '' = automatic
  let found = [];           // last search results
  const $s = k => root.querySelector(`[data-k="${k}"]`);
  const esc = t => String(t).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

  function renderPicker() {
    const rows = [{ addr: '', label: 'Automatic', sub: 'the first battery named F07…/F10… in range' }];
    for (const r of found) {
      const tags = [r.in_use && 'in use'].filter(Boolean).join(', ');
      rows.push({ addr: r.addr, label: r.name || r.addr, sub: `${r.addr} · ${r.rssi} dBm${tags ? ' · ' + tags : ''}` });
    }
    if (sel && !found.some(r => r.addr === sel)) {
      rows.push({ addr: sel, label: sel, sub: 'saved; not seen in the last search' });
    }
    $s('pick').innerHTML = rows.map(r => `
      <label class="bms-pick"><input type="radio" name="bms-pick" value="${esc(r.addr)}"${r.addr === sel ? ' checked' : ''}>
        <span><b>${esc(r.label)}</b><small>${esc(r.sub)}</small></span></label>`).join('');
    $s('pick').querySelectorAll('input').forEach(i => i.onchange = () => { sel = i.value; ctxRef.dirty(true); });
  }

  async function search() {
    const btn = $s('find'), msg = $s('find-msg');
    btn.disabled = true;
    msg.className = 'hint';
    msg.textContent = 'Searching for 8 s…';
    try {
      let r = await App.api('/api/bms/scan', { method: 'POST', body: {} });
      for (let i = 0; i < 20 && r.state !== 'done'; i++) {
        await new Promise(res => setTimeout(res, 1000));
        r = await App.api('/api/bms/scan');
      }
      found = r.results || [];
      msg.textContent = found.length ? `Found ${found.length}.`
        : 'No battery found. Is it in range, and is the FSolar app disconnected from it?';
      renderPicker();
    } catch (e) {
      msg.className = 'msg err';
      msg.textContent = e.message;
    } finally {
      btn.disabled = false;
    }
  }

  App.settings({
    id: 'bms', title: 'Connection', category: 'battery', order: 10,
    mount(el, ctx) {
      el.innerHTML = `
        <style>
          .bms-pick { display: flex; gap: 10px; align-items: flex-start; margin-top: 8px; padding: 8px 10px;
            border: 1px solid var(--line); border-radius: 8px; color: var(--text); font-size: 15px; cursor: pointer; }
          .bms-pick input { width: auto; margin-top: 3px; }
          .bms-pick small { display: block; color: var(--muted); font-size: 12px; }
        </style>
        <label style="margin-top:0">Battery</label>
        <div data-k="pick"></div>
        <div class="row"><button data-k="find">Find batteries</button><span class="hint" data-k="find-msg" style="margin:0"></span></div>
        <label>Pairing passkey<input data-k="pin" inputmode="numeric" autocomplete="off" placeholder=""></label>
        <p class="hint">Leave empty to keep the current passkey. Felicity's default is 123456. A passkey you set is stored on the board and never shown again.</p>
        <label>Poll interval (seconds, 10–3600)<input data-k="interval" type="number" min="10" max="3600"></label>
        <div class="row"><button data-k="pause">Pause 15 min for the FSolar app</button><button data-k="poll">Poll now</button></div>
        <div class="msg" data-k="msg"></div>`;
      root = el;
      ctxRef = ctx;
      for (const k of ['interval', 'pin']) $s(k).oninput = () => ctx.dirty(true);
      const act = async (body, text) => {
        try { await App.api('/api/bms', { method: 'POST', body }); $s('msg').className = 'msg ok'; $s('msg').textContent = text; }
        catch (e) { $s('msg').className = 'msg err'; $s('msg').textContent = e.message; }
      };
      $s('pause').onclick = () => act({ pause_min: 15 }, 'Paused for 15 minutes');
      $s('poll').onclick = () => act({ poll: true }, 'Polling');
      $s('find').onclick = search;
      renderPicker();
    },
    async show() {
      stored = await App.api('/api/bms');
      // Results of an earlier search, if any; otherwise the battery in use.
      const r = await App.api('/api/bms/scan').catch(() => ({ results: [] }));
      found = r.results?.length ? r.results
        : stored.addr ? [{ addr: stored.addr, name: stored.name, rssi: stored.sample?.rssi ?? 0, in_use: true }] : [];
      this.reset();
    },
    reset() {
      sel = stored.pinned_addr ?? '';
      renderPicker();
      $s('interval').value = stored.interval_s ?? 30;
      $s('pin').value = '';
      $s('pin').placeholder = stored.pin_set ? 'custom passkey set' : 'default (123456)';
    },
    async save() {
      const body = { interval_s: Number($s('interval').value) };
      // Only when changed: a new address makes the board forget the battery and connect afresh.
      if (sel !== (stored.pinned_addr ?? '')) body.addr = sel;
      if ($s('pin').value.trim()) body.pin = $s('pin').value.trim();
      stored = await App.api('/api/bms', { method: 'POST', body });
    },
  });
}
