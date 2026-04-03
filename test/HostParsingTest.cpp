#include "HostParsing.hpp"
#include "TestHeaders.hpp"

using namespace et;

TEST_CASE("parseHostString", "[HostParsing]") {
  SECTION("Simple hostname") {
    auto result = parseHostString("example.com");
    REQUIRE(result.user == "");
    REQUIRE(result.host == "example.com");
    REQUIRE(result.portSuffix == "");
  }

  SECTION("Hostname with port") {
    auto result = parseHostString("example.com:22");
    REQUIRE(result.user == "");
    REQUIRE(result.host == "example.com");
    REQUIRE(result.portSuffix == ":22");
  }

  SECTION("User and hostname") {
    auto result = parseHostString("user@example.com");
    REQUIRE(result.user == "user");
    REQUIRE(result.host == "example.com");
    REQUIRE(result.portSuffix == "");
  }

  SECTION("User, hostname, and port") {
    auto result = parseHostString("user@example.com:2222");
    REQUIRE(result.user == "user");
    REQUIRE(result.host == "example.com");
    REQUIRE(result.portSuffix == ":2222");
  }

  SECTION("IPv4 address") {
    auto result = parseHostString("192.168.1.1");
    REQUIRE(result.user == "");
    REQUIRE(result.host == "192.168.1.1");
    REQUIRE(result.portSuffix == "");
  }

  SECTION("IPv4 address with port") {
    auto result = parseHostString("192.168.1.1:22");
    REQUIRE(result.user == "");
    REQUIRE(result.host == "192.168.1.1");
    REQUIRE(result.portSuffix == ":22");
  }

  SECTION("IPv6 address in brackets") {
    auto result = parseHostString("[::1]");
    REQUIRE(result.user == "");
    REQUIRE(result.host == "[::1]");
    REQUIRE(result.portSuffix == "");
  }

  SECTION("IPv6 address with port") {
    auto result = parseHostString("[::1]:22");
    REQUIRE(result.user == "");
    REQUIRE(result.host == "[::1]");
    REQUIRE(result.portSuffix == ":22");
  }

  SECTION("User and IPv6 address") {
    auto result = parseHostString("user@[::1]");
    REQUIRE(result.user == "user");
    REQUIRE(result.host == "[::1]");
    REQUIRE(result.portSuffix == "");
  }

  SECTION("User, IPv6 address, and port") {
    auto result = parseHostString("user@[::1]:2222");
    REQUIRE(result.user == "user");
    REQUIRE(result.host == "[::1]");
    REQUIRE(result.portSuffix == ":2222");
  }

  SECTION("Full IPv6 address with port") {
    auto result = parseHostString("[2001:db8::1]:22");
    REQUIRE(result.user == "");
    REQUIRE(result.host == "[2001:db8::1]");
    REQUIRE(result.portSuffix == ":22");
  }

  SECTION("User with full IPv6 and port") {
    auto result = parseHostString("admin@[fe80::1%eth0]:22");
    REQUIRE(result.user == "admin");
    REQUIRE(result.host == "[fe80::1%eth0]");
    REQUIRE(result.portSuffix == ":22");
  }

  SECTION("Empty string") {
    auto result = parseHostString("");
    REQUIRE(result.user == "");
    REQUIRE(result.host == "");
    REQUIRE(result.portSuffix == "");
  }

  SECTION("Malformed IPv6 - missing close bracket") {
    auto result = parseHostString("[::1");
    REQUIRE(result.user == "");
    REQUIRE(result.host == "[::1");  // treated as literal
    REQUIRE(result.portSuffix == "");
  }

  SECTION("User with malformed IPv6") {
    auto result = parseHostString("user@[::1");
    REQUIRE(result.user == "user");
    REQUIRE(result.host == "[::1");
    REQUIRE(result.portSuffix == "");
  }
}
