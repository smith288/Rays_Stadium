const content = document.getElementById('content');
const statusEl = document.getElementById('game-status');
const nextGameEl = document.getElementById('next-game');
const lightToggleEl = document.getElementById('light-toggle');
const lightOffInput = document.getElementById('light-off');
const lightOnInput = document.getElementById('light-on');
let lightOn = false;
let lightBusy = false;

function logoUrl(team) {
  return `https://www.mlbstatic.com/team-logos/team-cap-on-light/${team.id}.svg`;
}

function teamRow(side, team) {
  const winnerClass = team.isWinner ? ' winner' : '';
  const href = `https://www.mlb.com/${String(team.teamName || team.name).toLowerCase().replaceAll(' ', '-')}`;
  return `<a class="team-row${winnerClass}" href="${href}" aria-label="${team.name}, ${team.wins} wins and ${team.losses} losses">
    <img class="team-logo" src="${logoUrl(team)}" alt="${team.teamName || team.name}" width="28" height="28">
    <div class="team-name">
      <div class="desktop-name">${team.teamName || team.name}</div>
      <div class="mobile-name">${team.abbreviation}</div>
      <div class="record">${team.wins} - ${team.losses}</div>
    </div>
    <div class="score">${team.score}</div>
  </a>`;
}

function lineScore(data) {
  const innings = data.lineScore?.innings || [];
  const away = data.teams.away;
  const home = data.teams.home;
  const totals = data.lineScore?.totals || {};
  const heads = innings.map(i => `<th scope="col">${i.num}</th>`).join('');
  const awayRuns = innings.map(i => `<td>${i.away ?? ''}</td>`).join('');
  const homeRuns = innings.map(i => `<td>${i.home ?? ''}</td>`).join('');
  const awayTotals = totals.away || { runs: away.score, hits: 0, errors: 0 };
  const homeTotals = totals.home || { runs: home.score, hits: 0, errors: 0 };

  return `<div class="line-wrap">
    <table aria-label="Box score by inning">
      <thead><tr><th class="team-cell" scope="col"></th>${heads}<th class="total" scope="col">R</th><th class="total" scope="col">H</th><th class="total" scope="col">E</th></tr></thead>
      <tbody>
        <tr><th class="team-cell" scope="row">${away.abbreviation}</th>${awayRuns}<td class="total">${awayTotals.runs}</td><td class="total">${awayTotals.hits}</td><td class="total">${awayTotals.errors}</td></tr>
        <tr><th class="team-cell" scope="row">${home.abbreviation}</th>${homeRuns}<td class="total">${homeTotals.runs}</td><td class="total">${homeTotals.hits}</td><td class="total">${homeTotals.errors}</td></tr>
      </tbody>
    </table>
  </div>`;
}

function liveState(data) {
  const state = data.inningState;
  if (!data.live || !state) return '';

  const runners = state.runners || {};
  const label = [
    runners.first ? '1st' : '',
    runners.second ? '2nd' : '',
    runners.third ? '3rd' : ''
  ].filter(Boolean).join(' & ') || 'No runners on base';
  const basesLabel = label === 'No runners on base' ? label : `Runners at ${label}`;
  const baseFill = occupied => occupied ? '#000000' : 'transparent';
  const outFill = index => index < (state.outs || 0) ? '#000000' : 'transparent';

  return `<div class="live-state" aria-label="${state.inning || ''}, count ${state.balls ?? 0} and ${state.strikes ?? 0}, ${state.outs ?? 0} outs">
    <div class="inning-label">${state.inning || ''}</div>
    <div class="bases-outs">
      <svg width="24" role="img" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 17.25" aria-label="${basesLabel}">
        <title>${basesLabel}</title>
        <rect fill="${baseFill(runners.first)}" stroke-width="1" stroke="#000000" width="6" height="6" transform="translate(5.25, 7.25) rotate(-315)" rx="0px" ry="0px"></rect>
        <rect fill="${baseFill(runners.second)}" stroke-width="1" stroke="#000000" width="6" height="6" transform="translate(12, 0.75) rotate(-315)" rx="0px" ry="0px"></rect>
        <rect fill="${baseFill(runners.third)}" stroke-width="1" stroke="#000000" width="6" height="6" transform="translate(18.75, 7.25) rotate(-315)" rx="0px" ry="0px"></rect>
      </svg>
      <svg width="25" role="img" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 23.5 7.5" aria-label="${state.outs ?? 0} outs">
        <title>${state.outs ?? 0} outs</title>
        <circle class="outs-dot" cx="3.75" cy="3.75" r="2.5" fill="${outFill(0)}"></circle>
        <circle class="outs-dot" cx="11.75" cy="3.75" r="2.5" fill="${outFill(1)}"></circle>
        <circle class="outs-dot" cx="19.75" cy="3.75" r="2.5" fill="${outFill(2)}"></circle>
      </svg>
    </div>
    <div class="count" aria-label="The count is ${state.balls ?? 0} and ${state.strikes ?? 0}.">${state.balls ?? 0} - ${state.strikes ?? 0}</div>
  </div>`;
}

function formatNextGame(nextGame) {
  if (!nextGame?.opponent) return '';
  const start = new Date(nextGame.gameDate);
  const dateStr = start.toLocaleDateString(undefined, { weekday: 'short', month: 'short', day: 'numeric' });
  const timeStr = start.toLocaleTimeString(undefined, { hour: 'numeric', minute: '2-digit' });
  const prefix = nextGame.raysHome ? 'vs' : '@';
  const opponent = nextGame.opponent;
  return `<img class="next-game-logo" src="${logoUrl(opponent)}" alt="${opponent.name}" width="22" height="22">
    <div class="next-game-copy">
      <div><span class="next-label">Next</span><span class="next-opponent">${prefix} ${opponent.abbreviation || opponent.name}</span></div>
      <span class="next-meta">${dateStr} · ${timeStr}</span>
    </div>`;
}

function renderNextGame(data) {
  if (!data.nextGame) {
    nextGameEl.hidden = true;
    nextGameEl.innerHTML = '';
    return;
  }
  nextGameEl.hidden = false;
  nextGameEl.innerHTML = formatNextGame(data.nextGame);
}

function render(data) {
  statusEl.textContent = data.status || 'Final';
  document.getElementById('page-title').textContent = data.live ? 'Live Game' : 'Last Final';
  content.className = '';
  content.innerHTML = `<div class="matchup">
    ${teamRow('away', data.teams.away)}
    ${teamRow('home', data.teams.home)}
  </div>${lineScore(data)}${liveState(data)}`;
  renderNextGame(data);

  if (data.live) {
    setTimeout(load, 30000);
  }
}

function renderLightStatus(light) {
  lightOn = !!light.on;
  lightOffInput.checked = !lightOn;
  lightOnInput.checked = lightOn;
  lightToggleEl.classList.toggle('is-busy', lightBusy);
}

async function loadLightStatus() {
  try {
    const res = await fetch('/api/status');
    const data = await res.json();
    if (!res.ok || !data.light) return;
    renderLightStatus(data.light);
  } catch (_) {}
}

async function setLight(requestedOn) {
  if (lightBusy || requestedOn === lightOn) return;
  lightBusy = true;
  renderLightStatus({ on: lightOn });
  try {
    const res = await fetch('/api/light', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ light: requestedOn })
    });
    const data = await res.json();
    if (!res.ok || data.error) throw new Error(data.error || 'Could not update light.');
    if (data.light) renderLightStatus(data.light);
  } catch (_) {
    renderLightStatus({ on: lightOn });
  } finally {
    lightBusy = false;
    renderLightStatus({ on: lightOn });
  }
}

async function load() {
  try {
    const res = await fetch('/api/last-game');
    const data = await res.json();
    if (!res.ok || data.error) throw new Error(data.error || 'Could not load game.');
    render(data);
  } catch (err) {
    statusEl.textContent = 'Unavailable';
    content.className = 'state error';
    content.textContent = err.message;
  }
}

lightToggleEl.addEventListener('change', (event) => {
  if (event.target.name !== 'stadium-light') return;
  setLight(event.target.value === 'on');
});
loadLightStatus();
setInterval(loadLightStatus, 10000);
load();
