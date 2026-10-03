#pragma once
// Themes over WiFi from orb-ponderer (this fork), beside upstream's theme_pull, which
// fetches from Zion's account server and is left exactly as it was.
//
// The relay's setup page says which uploaded themes this Orb should hold and which one to
// wear. Every ten minutes the Orb asks for that list's revision; when it has changed, it
// fetches the files that differ, removes the themes it installed that are no longer on the
// list, and restarts into the chosen one. Themes it did NOT install (copied onto the card
// by hand, or sent by Orb Studio) are never touched: a folder is only "ours" if it carries
// the record this module writes (_ponderer.json).
//
// The network half runs on the network side (ponderer::Module netStep), one file a pass,
// through the encrypted transport. Everything that touches the SD card runs on the UI side,
// because that is the only place this firmware touches the card (docs/memory.md).
//
// Works in the desktop simulator (the card is sim/sdcard). Not in the browser build: its
// card is in memory and a restart is a page reload, which would fetch everything again
// for ever. The demo page gets a theme by having one preloaded at build time instead.
namespace orb_themes {
    void init();   // once, after the apps are registered
}
