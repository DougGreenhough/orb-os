#pragma once
// Music's network half: polls orb-ponderer's spotify.now, fetches cover art when the art id
// changes, and sends the knob's commands. See docs/DEVICE-API.md in the orb-ponderer repo.
//
// Threads. Everything with "net" in its comment runs inside a ponderer::Module netStep (core
// 0 on the device, a background thread in the simulator) and never touches LVGL. The rest is
// called from the UI side only. The two meet in a handful of fields behind one std::mutex
// and a few atomics, all file-scope in music_client.cpp; nothing large is ever copied onto
// the network task's ~7 KB stack.
//
// Art ownership. The cover is the body ponderer::get() returned (PSRAM, ORB5). The net side
// fetches it and parks it as "pending"; takeArt() hands the whole allocation to the UI,
// which then owns it and gives it back through ponderer::release(). If the screen has been
// left by the time a fetch lands, the net side releases it itself, under the same lock that
// setShowing(false) takes, so there is no moment when neither or both think they own it.
#include <stddef.h>
#include <stdint.h>

namespace music {

// Cover size asked of spotify.art (s=, 40..300). 200 px is 80 KB of PSRAM as RGB565 and
// leaves the corners well inside the progress ring on a 466 px circle.
constexpr int ART_PX = 200;

enum Status : uint8_t {
    ST_WAITING = 0,   // entered, no answer yet
    ST_UNREACHABLE,   // the relay is not answering (or refused the key; get() cannot tell)
    ST_UNLINKED,      // linked:false, Spotify not connected on the setup page
    ST_IDLE,          // active:false, nothing playing anywhere
    ST_ACTIVE,        // something is loaded on a device, playing or paused
};

struct Now {
    Status   status;
    bool     playing;
    bool     canVolume;
    int      volume;        // 0..100, -1 unknown
    int32_t  progressMs;    // as of the answer
    int32_t  durationMs;
    uint32_t cmdSeq;        // the last command that had been sent when this poll started
    char     title[128];
    char     artist[128];
    char     device[48];
    char     artId[72];     // "" = no cover
};

enum Cmd : uint8_t { CMD_TOGGLE, CMD_NEXT, CMD_PREV, CMD_VOL };

// UI side ---------------------------------------------------------------------------------
void begin(void (*uiApply)());   // registers the ponderer module; once, from init()
void setShowing(bool on);         // onEnter / onExit. Off also drops any art not yet taken.
void pollSoon();                  // ask for a spotify.now on the next net pass
bool takeNow(Now &out);           // copy the latest answer; true when it is new
// Hand over the newest cover if one arrived. The caller owns *body afterwards.
bool takeArt(uint8_t **body, const uint16_t **px, int *w, int *h, char *id, size_t idLen);
uint32_t queue(Cmd c, int arg = 0);   // returns the command's sequence number
bool takeCmdFailed();             // a command came back non-200 since the last call

}  // namespace music
