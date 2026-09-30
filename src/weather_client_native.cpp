// The simulator's weather_fetch(): the same Open-Meteo request and the same fields as the
// device's weather_client.cpp, over net_fetch's libcurl path instead of WiFi/HTTPClient.
// Kept as its own file rather than an #ifdef inside weather_client.cpp so the device
// file stays exactly as upstream has it.
#ifndef ARDUINO
#include "weather_client.h"
#include "net_fetch.h"
#include "config.h"
#include <ArduinoJson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool weather_fetch(double lat, double lon, WeatherSnapshot &out) {
    char url[512];
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f"
             "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             "&forecast_days=4&timezone=auto", lat, lon);

    uint8_t *body = nullptr;
    size_t len = 0;
    if (!net_fetch_psram(url, ADSB_USER_AGENT, &body, &len, 64 * 1024, 3500, 8000)) {
        printf("[weather] fetch failed\n");
        return false;
    }
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, (const char *)body, len);
    free(body);
    if (err) {
        printf("[weather] JSON error: %s\n", err.c_str());
        return false;
    }

    JsonObjectConst current = doc["current"].as<JsonObjectConst>();
    JsonObjectConst daily = doc["daily"].as<JsonObjectConst>();
    if (current.isNull() || daily.isNull()) {
        printf("[weather] response missing current/daily data\n");
        return false;
    }

    WeatherSnapshot next = {};
    const char *stamp = current["time"] | "";
    const char *clock = strchr(stamp, 'T');
    snprintf(next.updated, sizeof(next.updated), "%.5s", clock ? clock + 1 : "--:--");
    next.code     = current["weather_code"] | -1;
    next.tempC    = current["temperature_2m"] | 0.0f;
    next.feelsC   = current["apparent_temperature"] | 0.0f;
    next.humidity = current["relative_humidity_2m"] | 0;
    next.windKmh  = current["wind_speed_10m"] | 0.0f;
    next.windDeg  = current["wind_direction_10m"] | 0;

    JsonArrayConst dates = daily["time"].as<JsonArrayConst>();
    const int n = dates.size() < 4 ? (int)dates.size() : 4;
    for (int i = 0; i < n; ++i) {
        WeatherDay &d = next.days[i];
        snprintf(d.date, sizeof(d.date), "%s", dates[i] | "");
        d.code       = daily["weather_code"][i] | -1;
        d.tempMaxC   = daily["temperature_2m_max"][i] | 0.0f;
        d.tempMinC   = daily["temperature_2m_min"][i] | 0.0f;
        d.rainChance = daily["precipitation_probability_max"][i] | 0;
    }
    next.dayCount = n;
    next.valid = true;
    out = next;
    return true;
}
#endif
