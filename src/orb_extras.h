#pragma once
// The apps this fork adds, registered in one place so main.cpp and sim_main.cpp each
// need a single call and cannot disagree about the order. They sit between the Flight
// Tracker and News, in the order of app_shell::Slot.
namespace orb_extras {
    void register_apps();   // init() each enabled app and app_shell::add() it
}
