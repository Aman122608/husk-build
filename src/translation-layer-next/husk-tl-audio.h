/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The host's audio output for a guest's mixer thread.
 *
 * Android audio engines (FMOD here) write interleaved 16-bit samples from a thread of their own and
 * expect each write to block until the audio has made room, which paces the mixer. This is that
 * sink, over AudioQueue (the same on macOS and iOS): a short ring the mixer fills and the audio
 * hardware drains.
 */
#ifndef HUSK_TL_AUDIO_H
#define HUSK_TL_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Route the guest's audio output (tl_cocos_audio_hook) to the speakers, and re-arm a sink a previous
 * session close silenced, so the next game in this process plays. */
void tl_audio_install(void);

/* Silence the output and stop the clock while the game is not on screen (writers block until resumed). */
void tl_audio_set_paused(bool paused);

/* The session close's audio call, after the layer's own producer threads are stopped: pause the sink, mark it
 * closed so any writer that survived the close drops its samples instead of reopening the queue, and
 * deactivate the host audio session so the system's "audio is playing" indicator goes away too. Nothing is
 * freed -- a guest writer parked in the sink is left parked, not given a freed ring -- and the next launch's
 * tl_audio_install() re-arms all of it. */
void tl_audio_close(void);

/* The layer's own audio producer threads, ended at a session close. Each is restartable by the next app in
 * the same process (a second game must still be able to play). Call these before tl_audio_close(). */
void tl_aaudio_stop_all(void);     /* the AAudio data-callback pumps (husk-tl-bionic-ndk.c) */
void tl_opensl_stop_all(void);     /* the OpenSL player pumps (husk-tl-opensles.c) */
void tl_dvm_audio_stop(void);      /* the DVM mixer thread (husk-tl-dvm-audio.c) */

#ifdef __cplusplus
}
#endif

#endif
