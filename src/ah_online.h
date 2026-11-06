#ifndef AH_ONLINE_H
#define AH_ONLINE_H

// Sets the ah_lb_* hooks (ah_host.h) to the online leaderboard when the build has a server
// (-DAH_LB_URL, -DAH_LB_KEY) and the data folder has no "offline" file. Call once at start-up.
void ah_online_init(const char *data_dir);

#endif
