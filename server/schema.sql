-- One row per score sent. Boards keep each player's best within the time frame asked for.
CREATE TABLE IF NOT EXISTS scores (
  player TEXT NOT NULL,        -- random id the Vita makes once (not an account)
  name TEXT NOT NULL,          -- the Vita account name, as sent
  difficulty INTEGER NOT NULL,
  score INTEGER NOT NULL,
  at INTEGER NOT NULL          -- unix seconds
);
CREATE INDEX IF NOT EXISTS scores_board ON scores (difficulty, at, score);
CREATE INDEX IF NOT EXISTS scores_player ON scores (player);
