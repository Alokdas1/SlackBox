#ifndef SLACKBOX_VIRTUALSPOOF_H
#define SLACKBOX_VIRTUALSPOOF_H

// Installs the system-property interception used by guest processes.
//
// The behaviour lives entirely in VirtualSpoof.cpp. This header exists only so
// BoxCore.cpp can call install_property_hooks() without pulling in libc
// internals.
//
// Call order matters. This must run before a guest calls into
// SystemProperties for the first time, so from enableIO() rather than lazily.

void install_property_hooks();

#endif  // SLACKBOX_VIRTUALSPOOF_H
