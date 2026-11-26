// Party-record actions shared by the hosts. The caller decides whether the field is free; this trades two whole records,
// reads both back, and restores them if either write did not take (the app's swapPartySlots).
#ifndef PD_PARTY_H
#define PD_PARTY_H

#include <stdbool.h>
#include <stdint.h>

#include "pd_binput.h"

bool pd_party_swap_records(uint32_t party, int stride, int count, int a, int b, pd_read_fn read, pd_write_fn write, void* ctx);

#endif
