#pragma once
/* Internal helpers shared between core translation units. Not public API. */
#include "radeon_legacy.h"

void rlg__irq_update(RLGDevice *d);
void rlg__log(RLGDevice *d, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
uint32_t rlg__mc_read(RLGDevice *d, uint32_t reg);
void rlg__mc_write(RLGDevice *d, uint32_t reg, uint32_t value);
void rlg__gart_update(RLGDevice *d);
int rlg__cp_exec_ib(RLGDevice *d, uint32_t gpu_addr, uint32_t ndw);
void rlg__scratch_writeback(RLGDevice *d, unsigned index, uint32_t value);
