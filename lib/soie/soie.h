/*
 * Soie, a lightweight HTML rendering engine.
 * Include this header only; it pulls in the others.
 */
#ifndef SOIE_H
#define SOIE_H

#define SOIE_VERSION_MAJOR 0
#define SOIE_VERSION_MINOR 1
#define SOIE_VERSION_PATCH 0

#include "dom.h"
#include "html.h"
#include "style.h"

/* Version of the library actually linked, "major.minor.patch". */
const char *soie_version(void);

#endif
