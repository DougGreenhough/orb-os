#pragma once
// Where orb-ponderer lives and this Orb's key for it.
//
// The key is a secret, so it is not written here. Put it in src/ponderer_secrets.h
// (gitignored), which is picked up automatically when present:
//
//     #define PONDERER_KEY "the key the setup page showed you"
//     // optionally: #define PONDERER_URL "http://192.168.1.20:8790/"
//
// Plain http:// on purpose: the device cannot do TLS (see intel_client.cpp).
#if __has_include("ponderer_secrets.h")
#include "ponderer_secrets.h"
#endif

#ifndef PONDERER_URL
#define PONDERER_URL "http://www.spiritdemon.net/orb-ponderer/"
#endif
#ifndef PONDERER_KEY
#define PONDERER_KEY ""
#endif
