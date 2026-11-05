// Alien Hominid HD Vita: online leaderboard for the main game.
//
//   POST /score    body: player, name, difficulty, score, time, sig (form-encoded)
//   GET  /top      ?difficulty=&timeframe=0|1|2&player=   (week, month, all time)
//   POST /delete   ?player= or ?name=   with "Authorization: Bearer <ADMIN_TOKEN>" (removes every score)
//
// Replies are plain text so the Vita needs no JSON parser. /top answers
//   me <row index or -1>
//   <rank>\t<name>\t<score>      (up to 100 rows, best first, then the player's row
//                                  if they're outside the top 100)
//
// Secrets (wrangler secret put): SCORE_KEY (also built into the VPK), ADMIN_TOKEN.

const TIMEFRAMES = [7 * 86400, 30 * 86400, 0];
const TOP = 100;

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    try {
      if (req.method === "POST" && url.pathname === "/score") return await postScore(req, env);
      if (req.method === "GET" && url.pathname === "/top") return await getTop(url, env);
      if (req.method === "POST" && url.pathname === "/delete") return await deletePlayer(req, url, env);
      return text("not found", 404);
    } catch (e) {
      return text("error", 500);
    }
  },
};

function text(body, status = 200) {
  return new Response(body, { status, headers: { "content-type": "text/plain; charset=utf-8" } });
}

const PLAYER_RE = /^[0-9a-f]{32}$/;

function cleanName(s) {
  // the Vita's name: printable, at most 16 characters
  s = String(s || "").replace(/[\u0000-\u001f\u007f\t]/g, "").trim().slice(0, 16);
  return s || "Player";
}

async function hmacHex(key, msg) {
  const k = await crypto.subtle.importKey("raw", new TextEncoder().encode(key), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  const sig = await crypto.subtle.sign("HMAC", k, new TextEncoder().encode(msg));
  return [...new Uint8Array(sig)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function postScore(req, env) {
  const f = new URLSearchParams(await req.text());
  const player = f.get("player") || "";
  const name = cleanName(f.get("name"));
  const difficulty = Number(f.get("difficulty"));
  const score = Number(f.get("score"));
  const time = Number(f.get("time"));
  const now = Math.floor(Date.now() / 1000);
  if (!PLAYER_RE.test(player)) return text("bad player", 400);
  if (!Number.isInteger(difficulty) || difficulty < 0 || difficulty > 9) return text("bad difficulty", 400);
  if (!Number.isInteger(score) || score <= 0 || score > Number(env.MAX_SCORE || 20000000)) return text("bad score", 400);
  // a queued score may arrive days late; anything older than 30 days or from the future is refused
  if (!Number.isInteger(time) || time > now + 600 || time < now - 30 * 86400) return text("bad time", 400);
  const want = await hmacHex(env.SCORE_KEY || "", `${player}|${name}|${difficulty}|${score}|${time}`);
  if (want !== (f.get("sig") || "")) return text("bad signature", 403);

  // one score every 10 seconds per player
  const last = await env.DB.prepare("SELECT MAX(at) AS at FROM scores WHERE player = ?").bind(player).first();
  if (last && last.at && now - last.at < 10) return text("slow down", 429);

  await env.DB.prepare("INSERT INTO scores (player, name, difficulty, score, at) VALUES (?, ?, ?, ?, ?)")
    .bind(player, name, difficulty, score, Math.min(time, now)).run();
  return text("ok");
}

async function getTop(url, env) {
  const difficulty = Number(url.searchParams.get("difficulty"));
  const timeframe = Number(url.searchParams.get("timeframe"));
  const player = url.searchParams.get("player") || "";
  if (!Number.isInteger(difficulty)) return text("bad difficulty", 400);
  const span = TIMEFRAMES[timeframe] ?? 0;
  const since = span ? Math.floor(Date.now() / 1000) - span : 0;

  // each player's best in the time frame (SQLite returns the name from the row holding MAX)
  const best = `SELECT player, name, MAX(score) AS score FROM scores
                WHERE difficulty = ? AND at >= ? GROUP BY player`;
  const top = await env.DB.prepare(`${best} ORDER BY score DESC, player LIMIT ${TOP}`).bind(difficulty, since).all();
  const rows = top.results.map((r, i) => ({ rank: i + 1, ...r }));

  let me = rows.findIndex((r) => r.player === player);
  if (me < 0 && PLAYER_RE.test(player)) {
    const mine = await env.DB.prepare(`SELECT name, MAX(score) AS score FROM scores
                 WHERE difficulty = ? AND at >= ? AND player = ?`).bind(difficulty, since, player).first();
    if (mine && mine.score) {
      const above = await env.DB.prepare(`SELECT COUNT(*) AS n FROM (${best}) WHERE score > ?`)
        .bind(difficulty, since, mine.score).first();
      rows.push({ rank: above.n + 1, name: mine.name, score: mine.score, player });
      me = rows.length - 1;
    }
  }
  const lines = [`me ${me}`, ...rows.map((r) => `${r.rank}\t${r.name}\t${r.score}`)];
  return text(lines.join("\n") + "\n");
}

async function deletePlayer(req, url, env) {
  if (!env.ADMIN_TOKEN || req.headers.get("authorization") !== `Bearer ${env.ADMIN_TOKEN}`) return text("no", 403);
  const player = url.searchParams.get("player") || "";
  const name = url.searchParams.get("name");
  let r;
  if (PLAYER_RE.test(player)) r = await env.DB.prepare("DELETE FROM scores WHERE player = ?").bind(player).run();
  else if (name) r = await env.DB.prepare("DELETE FROM scores WHERE name = ?").bind(name).run();
  else return text("bad player", 400);
  return text(`deleted ${r.meta.changes}`);
}
