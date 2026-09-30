// See orb_extras.h.
#include "orb_extras.h"
#include "config.h"
#include "app_shell.h"
#include "forecast_view.h"
#include "music_view.h"
#include "photo_view.h"
#include "facts_view.h"
#include "plasma_view.h"

namespace orb_extras {

void register_apps() {
#if APP_FORECAST_ENABLED
    forecastview::init();
    app_shell::add(forecastview::screen(), "Forecast", nullptr, nullptr, false,
                   forecastview::onEnter, forecastview::onExit, false);
#endif
#if APP_MUSIC_ENABLED
    musicview::init();
    app_shell::add(musicview::screen(), "Music", musicview::onPress, musicview::onTurn, false,
                   musicview::onEnter, musicview::onExit, false);
#endif
#if APP_PHOTOS_ENABLED
    photoview::init();
    app_shell::add(photoview::screen(), "Photos", photoview::onPress, photoview::onTurn, false,
                   photoview::onEnter, photoview::onExit, false);
#endif
#if APP_FACTS_ENABLED
    factsview::init();
    app_shell::add(factsview::screen(), "Facts", factsview::onPress, factsview::onTurn, false,
                   factsview::onEnter, factsview::onExit, false);
#endif
#if APP_PLASMA_ENABLED
    plasmaview::init();
    app_shell::add(plasmaview::screen(), "Plasma", plasmaview::onPress, plasmaview::onTurn, false,
                   plasmaview::onEnter, plasmaview::onExit, false);
#endif
}

} // namespace orb_extras
