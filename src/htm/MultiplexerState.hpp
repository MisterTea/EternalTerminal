#ifndef __MULTIPLEXER_STATE_HPP__
#define __MULTIPLEXER_STATE_HPP__

#include "Headers.hpp"
#include "SocketHandler.hpp"
#include "TerminalHandler.hpp"

namespace et {
const int UUID_LENGTH = 36;

class MultiplexerState {
 protected:
  struct Pane;
  struct Split;
  struct Tab;

 public:
  MultiplexerState(shared_ptr<SocketHandler> _socketHandler);
  string toJsonString();
  void appendData(const string& uid, const string& data);
  void newTab(const string& tabId, const string& paneId);
  void newSplit(const string& sourceId, const string& paneId, bool vertical);
  void closePane(const string& paneId);
  void update(int endpointFd);
  void sendTerminalBuffers(int endpointFd);
  void resizePane(const string& paneId, int cols, int rows);
  inline int numPanes() { return int(panes.size()); }

 protected:
  shared_ptr<SocketHandler> socketHandler;
  map<string, shared_ptr<Tab>> tabs;
  map<string, shared_ptr<Pane>> panes;
  map<string, shared_ptr<Split>> splits;
  set<string> closed;

  inline shared_ptr<Tab> getTab(const string& id) {
    auto it = tabs.find(id);
    if (it == tabs.end()) {
      STFATAL << "Tried to get a pane that doesn't exist: " << id;
    }
    return it->second;
  }
  inline shared_ptr<Pane> getPane(const string& id) {
    auto it = panes.find(id);
    if (it == panes.end()) {
      STFATAL << "Tried to get a pane that doesn't exist: " << id;
    }
    return it->second;
  }
  inline shared_ptr<Split> getSplit(const string& id) {
    auto it = splits.find(id);
    if (it == splits.end()) {
      STFATAL << "Tried to get a pane that doesn't exist: " << id;
    }
    return it->second;
  }

  void fatalIfFound(const string& id);
};
}  // namespace et

#endif  // __MULTIPLEXER_STATE_HPP__
