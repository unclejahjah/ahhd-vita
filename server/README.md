# Score server

The online leaderboard for the main game: a Cloudflare Worker with a D1 database. The Vita sends each finished level's score to `POST /score` and reads boards from `GET /top`. See the comment at the top of `src/index.js` for the requests and replies.

## Deploy

With [wrangler](https://developers.cloudflare.com/workers/wrangler/) logged in to your Cloudflare account:

```
cd server
npx wrangler d1 create ahhd-scores          # paste the database_id it prints into wrangler.toml
npx wrangler d1 execute ahhd-scores --remote --file schema.sql
npx wrangler secret put SCORE_KEY           # any long random string; the VPK needs the same one
npx wrangler secret put ADMIN_TOKEN         # another long random string, for removing scores
npx wrangler deploy                          # prints the Worker's URL
```

Build the VPK with the same key and the Worker's URL:

```
cmake -DAH_LB_URL=http://ahhd-scores.<you>.workers.dev -DAH_LB_KEY=<SCORE_KEY> ..
```

A build without them has no online leaderboards. The URL is plain `http://`: the Vita's HTTPS doesn't accept the certificate the Worker serves, which is why each score is signed instead.

## Removing a score

```
curl -X POST -H "Authorization: Bearer <ADMIN_TOKEN>" "http://ahhd-scores.<you>.workers.dev/delete?name=<name>"
```

This removes every score sent under that name.

## Limits

- Scores over `MAX_SCORE` (in `wrangler.toml`) are refused.
- One score every 10 seconds per player.
- Every score is signed with `SCORE_KEY`. That stops casual fakes, but anyone who takes the key out of a VPK can still send scores.
- Stored per score: a random player ID made on the Vita, the Vita account name, difficulty, score and time. No IP addresses or accounts are stored.
