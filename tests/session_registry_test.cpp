#include <catch2/catch.hpp>

#include "SessionRegistry.hpp"
#include "TestHeaders.hpp"

using namespace et;
using namespace std::chrono_literals;

TEST_CASE("SessionRegistry tracks identity and attachment", "[session_registry]") {
  SessionRegistry registry(60s);

  // Empty initially
  REQUIRE(registry.size() == 0);
  REQUIRE(!registry.exists("s1"));

  // Attach creates session
  registry.attach("s1", "test-session", 60s);
  REQUIRE(registry.exists("s1"));
  REQUIRE(registry.size() == 1);

  auto entry = registry.get("s1");
  REQUIRE(entry != nullptr);
  REQUIRE(entry->id == "s1");
  REQUIRE(entry->name == "test-session");
  REQUIRE(entry->attached == true);
  REQUIRE(!entry->isExpired());
}

TEST_CASE("SessionRegistry lists unattached sessions", "[session_registry]") {
  SessionRegistry registry(60s);
  registry.attach("s1", "attached", 60s);
  registry.attach("s2", "detached", 60s);

  registry.detach("s2");

  auto unattached = registry.list(true);
  REQUIRE(unattached.size() == 1);
  REQUIRE(unattached[0]->id == "s2");
  REQUIRE(unattached[0]->attached == false);
}

TEST_CASE("SessionRegistry expiry", "[session_registry]") {
  SessionRegistry registry(0s);
  registry.attach("expired", "test", -1s);  // expired immediately via negative

  REQUIRE(registry.listExpired().size() == 1);

  size_t purged = registry.purgeExpired();
  REQUIRE(purged == 1);
  REQUIRE(registry.size() == 0);
}

TEST_CASE("Unified session management avoids separate introspection",
          "[session_registry]") {
  SessionRegistry registry(30s);
  registry.attach("session-a", "A", 30s);
  registry.attach("session-b", "B", 30s);

  // Listing should include identity, time, and expiry in one call.
  auto all = registry.list(false);
  REQUIRE(all.size() == 2);
  for (auto s : all) {
    REQUIRE(s->attached == true);
    REQUIRE(!s->isExpired());
  }
}
