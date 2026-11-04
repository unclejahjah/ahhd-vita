// Sound on the Vita: the game's own sound files (MS ADPCM WAVs named .adpcm, copied from
// the user's own PC copy), mixed into one output port.
//   sound effects: <dir>sounds/<linkage>.adpcm, decoded on first use and kept (LRU)
//   music:         <dir>music/<name>.adpcm, streamed
#ifndef CC_AUDIO_H
#define CC_AUDIO_H

int audio_init(const char *data_dir);   // 0 if there's no audio output
void audio_shutdown(void);

void audio_sfx_play(const char *linkage, float volume, float pan, int loops, const void *owner);
void audio_sfx_stop(const void *owner);   // what owner started; NULL: everything
void audio_sfx_set(const void *owner, float volume, float pan);   // what owner is playing
void audio_music_play(const char *name, int loop);
void audio_music_stop(void);
void audio_music_pause(int paused);
void audio_resumed(void);   // call when the app comes back from being suspended
void audio_set_volumes(float music, float sfx);   // 0..1; a negative value leaves that one

#endif
