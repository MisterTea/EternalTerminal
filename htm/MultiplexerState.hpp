#ifndef __MULTIPLEXER_STATE_HPP__
#define __MULTIPLEXER_STATE_HPP__

#include "Headers.hpp"

#include "HTM.pb.h"
#include "TerminalHandler.hpp"

namespace et {
const int UUID_LENGTH = 36;

class MultiplexerState {
 protected:
  struct InternalPane {
    string id;
    string parentId;
    shared_ptr<TerminalHandler> terminal;

    Pane toProto() {
      Pane p;
      p.set_id(id);
      return p;
    }
  };
  struct InternalSplit {
    string id;
    string parentId;
    bool vertical;
    vector<string> panes_or_splits;
    vector<float> sizes;

    Split toProto() {
      Split split;
      split.set_id(id);
      split.set_vertical(vertical);
      *(split.mutable_panes_or_splits()) = {panes_or_splits.begin(),
                                            panes_or_splits.end()};
      *(split.mutable_sizes()) = {sizes.begin(), sizes.end()};

      return split;
    }
  };
  struct InternalTab {
    string id;
    string pane_or_split_id;
    int order;

    Tab toProto() {
      Tab tab;
      tab.set_id(id);
      tab.set_order(order);
      tab.set_pane_or_split(pane_or_split_id);
      return tab;
    }
  };

 public:
  MultiplexerState();
  State getStateProto();
  void appendData(const string& uid, const string& data);
  void newTab(const string& tabId, const string& paneId);
  void newSplit(const string& sourceId, const string& paneId, bool vertical);
  void closePane(const string& paneId);
  void update(int endpointFd);
  void sendTerminalBuffers(int endpointFd);
  void resizePane(const string& paneId, int cols, int rows);

 protected:
  map<string, shared_ptr<InternalTab>> tabs;
  map<string, shared_ptr<InternalPane>> panes;
  map<string, shared_ptr<InternalSplit>> splits;
  set<string> closed;

  inline shared_ptr<InternalTab> getTab(const string& id) {
    auto it = tabs.find(id);
    if (it == tabs.end()) {
      LOG(FATAL) << "Tried to get a pane that doesn't exist: " << id;
    }
    return it->second;
  }
  inline shared_ptr<InternalPane> getPane(const string& id) {
    auto it = panes.find(id);
    if (it == panes.end()) {
      LOG(FATAL) << "Tried to get a pane that doesn't exist: " << id;
    }
    return it->second;
  }
  inline shared_ptr<InternalSplit> getSplit(const string& id) {
    auto it = splits.find(id);
    if (it == splits.end()) {
      LOG(FATAL) << "Tried to get a pane that doesn't exist: " << id;
    }
    return it->second;
  }

  void fatalIfFound(const string& id);
};
}  // namespace et

#endif  // __MULTIPLEXER_STATE_HPP__