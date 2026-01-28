#pragma once

// Read network configuration and populate routing table, device IPs, etc.
// Called during boot after network devices and routing subsystem are initialised.
void netcfg_init(void);
