#include "HtmTestHelpers.hpp"
#include "IpcPairClient.hpp"
#include "IpcPairServer.hpp"
#include "JsonLib.hpp"
#include "MultiplexerState.hpp"
#include "TestHeaders.hpp"

using namespace et;
using namespace et::htmtest;

namespace {
class SilentServer : public IpcPairServer {
 public:
  SilentServer(shared_ptr<SocketHandler> handler, const SocketEndpoint& endpoint)
      : IpcPairServer(handler, endpoint) {}
  void recover() override {}
};
}  // namespace

TEST_CASE("MultiplexerState serializes the initial tab and pane",
          "[Htm][MultiplexerState]") {
  UniqueIpcPath ipc;
  auto handler = std::make_shared<PipeSocketHandler>();
  SilentServer server(handler, endpointFor(ipc.path));
  IpcPairClient client(handler, endpointFor(ipc.path));
  server.pollAccept();

  MultiplexerState mux(handler);
  REQUIRE(mux.numPanes() == 1);
  json state = json::parse(mux.toJsonString());
  REQUIRE(state["shell"].get<string>().size() > 0);
  REQUIRE(state["tabs"].size() == 1);
  REQUIRE(state["panes"].size() == 1);
  string paneId = firstJsonKey(state["panes"]);
  mux.resizePane(paneId, 80, 24);
#ifdef WIN32
  mux.appendData(paneId, "echo MUX_ECHO_99\r\n");
#else
  mux.appendData(paneId, "printf 'MUX_ECHO_99\\n'\n");
#endif
  REQUIRE(waitUntil(
      [&]() {
        mux.update(server.getEndpointFd());
        return handler->hasData(client.getEndpointFd());
      },
      8000));
  mux.sendTerminalBuffers(server.getEndpointFd());
  client.closeEndpoint();
}

TEST_CASE("MultiplexerState splits, nested splits, tabs, and close collapse",
          "[Htm][MultiplexerState]") {
  UniqueIpcPath ipc;
  auto handler = std::make_shared<PipeSocketHandler>();
  SilentServer server(handler, endpointFor(ipc.path));
  IpcPairClient client(handler, endpointFor(ipc.path));
  server.pollAccept();

  MultiplexerState mux(handler);
  json initial = json::parse(mux.toJsonString());
  string pane1 = firstJsonKey(initial["panes"]);

  string pane2 = sole::uuid4().str();
  mux.newSplit(pane1, pane2, true);
  json afterVertical = json::parse(mux.toJsonString());
  REQUIRE(mux.numPanes() == 2);
  REQUIRE(afterVertical["splits"].size() == 1);

  string pane3 = sole::uuid4().str();
  mux.newSplit(pane1, pane3, true);
  json continued = json::parse(mux.toJsonString());
  REQUIRE(mux.numPanes() == 3);
  REQUIRE(continued["splits"].size() == 1);
  auto split = firstJsonValue(continued["splits"]);
  REQUIRE(split["panesOrSplits"].size() == 3);

  string pane4 = sole::uuid4().str();
  mux.newSplit(pane2, pane4, false);
  json nested = json::parse(mux.toJsonString());
  REQUIRE(mux.numPanes() == 4);
  REQUIRE(nested["splits"].size() == 2);

  mux.closePane(pane4);
  json collapsedNested = json::parse(mux.toJsonString());
  REQUIRE(mux.numPanes() == 3);
  REQUIRE(collapsedNested["splits"].size() == 1);

  mux.closePane(pane3);
  json stillSplit = json::parse(mux.toJsonString());
  REQUIRE(mux.numPanes() == 2);
  REQUIRE(stillSplit["splits"].size() == 1);

  mux.closePane(pane2);
  json rootPane = json::parse(mux.toJsonString());
  REQUIRE(mux.numPanes() == 1);
  REQUIRE(rootPane["splits"].empty());

  string tab2 = sole::uuid4().str();
  string pane5 = sole::uuid4().str();
  mux.newTab(tab2, pane5);
  REQUIRE(mux.numPanes() == 2);
  json twoTabs = json::parse(mux.toJsonString());
  REQUIRE(twoTabs["tabs"].size() == 2);

  mux.closePane(pane5);
  mux.closePane(pane5);
  json oneTab = json::parse(mux.toJsonString());
  REQUIRE(mux.numPanes() == 1);
  REQUIRE(oneTab["tabs"].size() == 1);

  mux.closePane(pane1);
  REQUIRE(mux.numPanes() == 0);
  client.closeEndpoint();
}

TEST_CASE("MultiplexerState reports a pane that exits",
          "[Htm][MultiplexerState]") {
  UniqueIpcPath ipc;
  auto handler = std::make_shared<PipeSocketHandler>();
  SilentServer server(handler, endpointFor(ipc.path));
  IpcPairClient client(handler, endpointFor(ipc.path));
  server.pollAccept();

  MultiplexerState mux(handler);
  string paneId = firstJsonKey(json::parse(mux.toJsonString())["panes"]);
#ifdef WIN32
  mux.appendData(paneId, "exit\r\n");
#else
  mux.appendData(paneId, "exit\n");
#endif
  REQUIRE(waitUntil(
      [&]() {
        mux.update(server.getEndpointFd());
        return mux.numPanes() == 0;
      },
      8000));
  string incoming = readUntil(handler, client.getEndpointFd(), 1, 1000);
  consumeInitSequence(&incoming);
  bool sawClose = false;
  HtmPacket packet;
  while (popPacket(&incoming, &packet)) {
    if (packet.header == SERVER_CLOSE_PANE) {
      sawClose = true;
    }
  }
  REQUIRE(sawClose);
  client.closeEndpoint();
}
