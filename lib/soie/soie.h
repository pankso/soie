/*
 * Soie, a lightweight HTML rendering engine.
 * Include this header only; it pulls in the others.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef SOIE_H
#define SOIE_H

#define SOIE_VERSION_MAJOR 1
#define SOIE_VERSION_MINOR 3
#define SOIE_VERSION_PATCH 0

#include "dom.h"
#include "html.h"
#include "style.h"
#include "layout.h"
#include "ext.h"

/* Version of the library actually linked, "major.minor.patch". */
const char *soie_version(void);

#endif
