#ifndef __SMS_CHERI_HELPER_H__
#define __SMS_CHERI_HELPER_H__

#include <stdint.h>

#include "../sms/bitmap.h"


class FTEntry
{
public:
  uint64_t region_id;       // region identity (object-chunk VA or PA-based)
  uint64_t pc;
  uint32_t trigger_offset;  // CL index within region that triggered this entry

  // Capability metadata captured at insertion time.
  // Used later for bounds-checked prefetch generation.
  uint64_t cap_base;
  uint64_t cap_top;         // cap_base + cap_length
  uint64_t demand_pa_page;  // physical page of the triggering demand (for VA->PA)
  uint64_t demand_va_page;  // virtual page of the triggering demand

public:
  void reset()
  {
    region_id = 0xdeadbeef;
    pc = 0xdeadbeef;
    trigger_offset = 0;
    cap_base = 0;
    cap_top = 0;
    demand_pa_page = 0;
    demand_va_page = 0;
  }
  FTEntry() { reset(); }
  ~FTEntry() {}
};

class ATEntry
{
public:
  uint64_t region_id;
  uint64_t pc;
  uint32_t trigger_offset;
  Bitmap   pattern;
  uint32_t age;

  uint64_t cap_base;
  uint64_t cap_top;
  uint64_t demand_pa_page;
  uint64_t demand_va_page;

public:
  void reset()
  {
    region_id = pc = 0xdeadbeef;
    trigger_offset = 0;
    pattern.reset();
    age = 0;
    cap_base = 0;
    cap_top = 0;
    demand_pa_page = 0;
    demand_va_page = 0;
  }
  ATEntry() { reset(); }
  ~ATEntry() {}
};

// Pattern History Table entry.
class PHTEntry
{
public:
  uint64_t signature;
  Bitmap   pattern;
  uint32_t age;

public:
  void reset()
  {
    signature = 0xdeadbeef;
    pattern.reset();
    age = 0;
  }
  PHTEntry() { reset(); }
  ~PHTEntry() {}
};

#endif /* __SMS_CHERI_HELPER_H__ */