#include "SocksUtils.hpp"
#include "TestHeaders.hpp"

using namespace et;
using Catch::Matchers::ContainsSubstring;

namespace {

string socks5AuthNoAuth() { return string("\x05\x01\x00", 3); }

string socks5ConnectIpv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                         uint16_t port) {
  string req("\x05\x01\x00\x01", 4);
  req.push_back(static_cast<char>(a));
  req.push_back(static_cast<char>(b));
  req.push_back(static_cast<char>(c));
  req.push_back(static_cast<char>(d));
  req.push_back(static_cast<char>((port >> 8) & 0xff));
  req.push_back(static_cast<char>(port & 0xff));
  return req;
}

string socks5ConnectDomain(const string& host, uint16_t port) {
  string req("\x05\x01\x00\x03", 4);
  req.push_back(static_cast<char>(host.size()));
  req += host;
  req.push_back(static_cast<char>((port >> 8) & 0xff));
  req.push_back(static_cast<char>(port & 0xff));
  return req;
}

}  // namespace

TEST_CASE("parseDynamicForwardArg port only", "[SocksUtils]") {
  auto endpoint = parseDynamicForwardArg("1080");
  CHECK(endpoint.name() == "127.0.0.1");
  CHECK(endpoint.port() == 1080);
}

TEST_CASE("parseDynamicForwardArg bind and port", "[SocksUtils]") {
  auto endpoint = parseDynamicForwardArg("localhost:9050");
  CHECK(endpoint.name() == "localhost");
  CHECK(endpoint.port() == 9050);
}

TEST_CASE("parseDynamicForwardArg ipv6 bind", "[SocksUtils]") {
  auto endpoint = parseDynamicForwardArg("[::1]:1080");
  CHECK(endpoint.name() == "::1");
  CHECK(endpoint.port() == 1080);
}

TEST_CASE("parseStdioForwardArg host port", "[SocksUtils]") {
  auto endpoint = parseStdioForwardArg("example.com:443");
  CHECK(endpoint.name() == "example.com");
  CHECK(endpoint.port() == 443);
}

TEST_CASE("parseStdioForwardArg unix path", "[SocksUtils]") {
  auto endpoint = parseStdioForwardArg("/tmp/remote.sock");
  CHECK(endpoint.name() == "/tmp/remote.sock");
  CHECK_FALSE(endpoint.has_port());
}

TEST_CASE("parseStdioForwardArg rejects bare host", "[SocksUtils]") {
  CHECK_THROWS_WITH(parseStdioForwardArg("onlyhost"),
                    ContainsSubstring("host:port"));
}

TEST_CASE("feedSocksHandshake SOCKS5 ipv4", "[SocksUtils]") {
  SocksHandshake state;
  state.input = socks5AuthNoAuth() + socks5ConnectIpv4(127, 0, 0, 1, 8080);
  REQUIRE(feedSocksHandshake(&state) == SocksParseStatus::Complete);
  CHECK(state.destination.name() == "127.0.0.1");
  CHECK(state.destination.port() == 8080);
  REQUIRE(state.reply.size() >= 2);
  CHECK(static_cast<uint8_t>(state.reply[0]) == 0x05);
  CHECK(static_cast<uint8_t>(state.reply[1]) == 0x00);
}

TEST_CASE("feedSocksHandshake SOCKS5 domain then unix path", "[SocksUtils]") {
  SocksHandshake state;
  state.input = socks5AuthNoAuth();
  REQUIRE(feedSocksHandshake(&state) == SocksParseStatus::NeedMore);
  REQUIRE(state.reply == string("\x05\x00", 2));
  state.reply.clear();

  state.input = socks5ConnectDomain("/tmp/app.sock", 0);
  REQUIRE(feedSocksHandshake(&state) == SocksParseStatus::Complete);
  CHECK(state.destination.name() == "/tmp/app.sock");
  CHECK_FALSE(state.destination.has_port());
}

TEST_CASE("feedSocksHandshake SOCKS4", "[SocksUtils]") {
  SocksHandshake state;
  // VN CD PORT PORT IP(1.2.3.4) USERID NUL
  state.input = string("\x04\x01\x00\x50\x01\x02\x03\x04user\x00", 13);
  REQUIRE(feedSocksHandshake(&state) == SocksParseStatus::Complete);
  CHECK(state.destination.name() == "1.2.3.4");
  CHECK(state.destination.port() == 80);
}
