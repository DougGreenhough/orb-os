#pragma once
#include <stdint.h>
// Screen cycle: step through a chosen set of apps on a loop, and stand still while
// someone is using the Orb.
//
//   - Every `secs` seconds the next chosen app slides in (registration order, wrapping).
//   - Any knob input pauses it. After `resume` seconds with no input it carries on,
//     from whatever is showing: if that isn't one of the chosen apps, the next chosen
//     one comes in straight away.
//   - Never while the app switcher is open.
//
// Settings come from orb-ponderer's `config` function (edited on its setup page, per
// Orb), fetched at start and every five minutes; the compiled defaults in config.h
// (ORB_CYCLE_*) apply until then, or for good without a relay. In the simulator,
// ORB_CYCLE="secs=8;resume=20;apps=Clock,Facts,Plasma" overrides both.
namespace orb_cycle {
    void noteInput();            // any knob turn or press (input_router calls this)
    void tick(uint32_t nowMs);   // UI loop, every pass
}
