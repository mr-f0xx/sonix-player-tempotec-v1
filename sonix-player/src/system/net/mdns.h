#ifndef MDNS_H
#define MDNS_H

#include <stdbool.h>

// Answer IPv4 A queries for <name>.local on this Wi-Fi address, so the file
// manager can be typed by name on clients that resolve .local. The numeric
// address is the fallback (including for the QR). No Bonjour service records.
// Start and stop must be serialized by the transfer service.
bool mdns_start(const char *name, const char *ip);
void mdns_stop(void);

#endif /* MDNS_H */
