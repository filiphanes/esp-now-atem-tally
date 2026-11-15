#pragma once

#include "esp_err.h"

// Advertises the controller as "tally.local" and an `_http._tcp` service on
// port 80, matching the Arduino build (mdns_hostname_set("tally"),
// mdns_instance_name_set("TallyBridge")).
//
// Call after the Ethernet netif exists; actual announcements start once the
// interface has an IP address.
esp_err_t mdns_advertise_start(void);
