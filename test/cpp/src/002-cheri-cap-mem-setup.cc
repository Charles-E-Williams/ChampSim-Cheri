#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "capability_memory.h"
#include "champsim.h"

/*
 * CACHE and the CHERI tracereader index champsim::cap_mem by CPU, so every test
 * case starts with a fresh, unfinalized capability memory for each CPU.
 */
class cap_mem_reset_listener : public Catch::EventListenerBase
{
public:
  using Catch::EventListenerBase::EventListenerBase;

  void testCaseStarting(Catch::TestCaseInfo const&) override { champsim::initialize_capability_memory(NUM_CPUS); }
};

CATCH_REGISTER_LISTENER(cap_mem_reset_listener)
