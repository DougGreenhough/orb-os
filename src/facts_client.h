#pragma once
// Facts, the network half: one prefetched fact from orb-ponderer (fn=fact), held until the
// screen takes it, and an honest word about why there is none when there is none.
//
// The split every relay-fed screen here uses (see ponderer.h): netStep runs on the network
// side and may block; everything the UI reads comes out through take()/status() under a
// mutex, and the UI side hears about news through the uiApply it hands to start().
#include <stdint.h>

namespace facts {

struct Fact {
    char id[48];
    char text[200];    // the contract says at most 160; the rest is slack, not a promise
    char topic[32];    // at most 20 by contract
    char source[32];
};

enum Status : uint8_t {
    ST_WAITING,        // not asked yet, or asking
    ST_OK,             // the last answer was a fact
    ST_UNCONFIGURED,   // no relay key in this build
    ST_OFFLINE,        // no WiFi
    ST_UNREACHABLE,    // the relay did not answer at all
    ST_EMPTY,          // the relay answered: it has no facts
    ST_RELAY_ERROR,    // the relay answered hello but would not give a fact
};

// Registers the ponderer module. uiApply runs on the LVGL side whenever something changed.
void start(void (*uiApply)());

// While false the net side keeps at most one fact prefetched and asks for nothing more.
void setShowing(bool showing);

// UI side: hand over the prefetched fact, if there is one, and start fetching the next
// (asking the relay not to repeat this one).
bool take(Fact &out);

Status status();

}  // namespace facts
