#include "MultiplexerState.hpp"

#include "HtmHeaderCodes.hpp"
#include "JsonLib.hpp"

namespace et {
struct MultiplexerState::Pane {
  string id;
  string parentId;
  shared_ptr<TerminalHandler> terminal;

  json toJson() {
    json pane;
    pane["id"] = id;
    return pane;
  }
};
struct MultiplexerState::Split {
  string id;
  string parentId;
  bool vertical;
  vector<string> panes_or_splits;
  vector<float> sizes;

  json toJson() {
    json split;
    split["id"] = id;
    split["vertical"] = vertical;
    split["panesOrSplits"] = panes_or_splits;
    split["sizes"] = sizes;

    return split;
  }
};
struct MultiplexerState::Tab {
  string id;
  string pane_or_split_id;
  int order;

  json toJson() {
    json tab;
    tab["id"] = id;
    tab["order"] = order;
    tab["paneOrSplit"] = pane_or_split_id;
    return tab;
  }
};
MultiplexerState::MultiplexerState(shared_ptr<SocketHandler> _socketHandler)
    : socketHandler(_socketHandler) {
  shared_ptr<Tab> t(new Tab());
  t->id = sole::uuid4().str();
  tabs.insert(make_pair(t->id, t));
  t->order = 0;

  {
    shared_ptr<Pane> p(new Pane());
    p->id = sole::uuid4().str();
    panes.insert(make_pair(p->id, p));
    p->parentId = t->id;
    auto terminal = shared_ptr<TerminalHandler>(new TerminalHandler());
    terminal->start();
    p->terminal = terminal;

    t->pane_or_split_id = p->id;
  }
}

string MultiplexerState::toJsonString() {
  json state;
  state["shell"] = string(::getenv("SHELL"));

  for (auto &it : tabs) {
    state["tabs"][it.first] = it.second->toJson();
  }

  for (auto &it : panes) {
    state["panes"][it.first] = it.second->toJson();
  }

  for (auto &it : splits) {
    state["splits"][it.first] = it.second->toJson();
  }

  return state.dump();
}

void MultiplexerState::appendData(const string &uid, const string &data) {
  if (panes.find(uid) == panes.end()) {
    STFATAL << "Tried to write to non-existent terminal";
  }
  panes[uid]->terminal->appendData(data);
}

void MultiplexerState::newTab(const string &tabId, const string &paneId) {
  fatalIfFound(tabId);
  fatalIfFound(paneId);
  auto tab = shared_ptr<Tab>(new Tab());
  tab->id = tabId;
  tab->order = tabs.size();
  tabs.insert(make_pair(tab->id, tab));
  tab->pane_or_split_id = paneId;

  auto pane = shared_ptr<Pane>(new Pane());
  pane->id = paneId;
  pane->parentId = tab->id;
  auto terminal = shared_ptr<TerminalHandler>(new TerminalHandler());
  terminal->start();
  pane->terminal = terminal;
  panes.insert(make_pair(paneId, pane));
}

void MultiplexerState::newSplit(const string &sourceId, const string &paneId,
                                bool vertical) {
  fatalIfFound(paneId);

  auto newPane = shared_ptr<Pane>(new Pane());
  newPane->id = paneId;
  panes.insert(make_pair(paneId, newPane));
  auto terminal = shared_ptr<TerminalHandler>(new TerminalHandler());
  terminal->start();
  newPane->terminal = terminal;

  auto sourcePane = getPane(sourceId);
  auto splitIt = splits.find(sourcePane->parentId);
  if (splitIt != splits.end() && splitIt->second->vertical == vertical) {
    LOG(INFO) << "Continuing a split";
    // The source is already part of a split with the same orientation.  Append
    // and resize
    auto split = splitIt->second;
    for (auto &it : split->sizes) {
      it /= 2.0;
    }
    split->sizes.push_back(0.5);
    split->panes_or_splits.push_back(paneId);
    newPane->parentId = split->id;
    return;
  }

  // We need to make a new split from an existing split
  if (splitIt != splits.end()) {
    LOG(INFO) << "Splitting in a new direction";
    // Parent is a split, but of the wrong orientation.  The sourceId will be
    // split in the other orientation.
    auto parentSplit = splitIt->second;

    // Create a new split with the sourceId & paneId in the correct direction
    auto newSplit = shared_ptr<Split>(new Split());
    newSplit->id = sole::uuid4().str();
    splits.insert(make_pair(newSplit->id, newSplit));
    newSplit->vertical = vertical;
    newSplit->panes_or_splits.push_back(sourceId);
    newSplit->panes_or_splits.push_back(paneId);
    newSplit->sizes.push_back(0.5);
    newSplit->sizes.push_back(0.5);
    newSplit->parentId = parentSplit->id;
    newPane->parentId = newSplit->id;
    sourcePane->parentId = newSplit->id;

    // Replace the sourceId with the new split
    for (size_t a = 0; a < parentSplit->panes_or_splits.size(); a++) {
      if (parentSplit->panes_or_splits[a] == sourceId) {
        parentSplit->panes_or_splits[a] = newSplit->id;
        break;
      }
      if (a + 1 == parentSplit->panes_or_splits.size()) {
        STFATAL << "SourcePane missing from parent split";
      }
    }
    return;
  }

  LOG(INFO) << "Splitting a root pane";
  // We are splitting a tab with a solo pane
  auto tab = getTab(sourcePane->parentId);

  // Create a new split with the sourceId & paneId in the correct direction
  auto newSplit = shared_ptr<Split>(new Split());
  newSplit->id = sole::uuid4().str();
  splits.insert(make_pair(newSplit->id, newSplit));
  newSplit->vertical = vertical;
  newSplit->panes_or_splits.push_back(sourceId);
  newSplit->panes_or_splits.push_back(paneId);
  newSplit->sizes.push_back(0.5);
  newSplit->sizes.push_back(0.5);
  newSplit->parentId = tab->id;
  newPane->parentId = newSplit->id;
  sourcePane->parentId = newSplit->id;

  // Set the tab's child to be the new split
  tab->pane_or_split_id = newSplit->id;
}

void MultiplexerState::closePane(const string &paneId) {
  if (closed.find(paneId) != closed.end()) {
    return;  // Already closed
  }
  if (panes.find(paneId) == panes.end()) {
    STFATAL << "Tried to close a pane that doesn't exist";
  }
  auto pane = panes[paneId];
  panes.erase(panes.find(paneId));
  closed.insert(paneId);
  LOG(INFO) << "Stopping terminal";
  pane->terminal->stop();
  LOG(INFO) << "Terminal stopped";
  // Delete the pane from the state
  if (tabs.find(pane->parentId) != tabs.end()) {
    // Delete the entire tab
    auto tab = getTab(pane->parentId);
    int order = tab->order;

    // Shift the order of the other tabs
    for (auto it : tabs) {
      if (it.second->order > order) {
        it.second->order -= 1;
      }
    }

    // Delete the tab and return
    for (auto it = tabs.begin(); it != tabs.end(); it++) {
      if (it->second->pane_or_split_id == pane->id) {
        tabs.erase(it);
        return;
      }
    }
    STFATAL << "Could not find tab";
  }

  // Not a top-level pane
  auto split = getSplit(pane->parentId);
  for (size_t a = 0; a < split->panes_or_splits.size(); a++) {
    if (split->panes_or_splits[a] == pane->id) {
      split->panes_or_splits.erase(split->panes_or_splits.begin() + a);
      split->sizes.erase(split->sizes.begin() + a);
      break;
    }
    if (a + 1 == split->panes_or_splits.size()) {
      STFATAL << "Parent pane " << split->id << " did not contain child pane "
              << pane->id;
    }
  }
  if (split->panes_or_splits.size() > 1) {
    // Normalize the sizes array
    int newSize = split->sizes.size();
    int oldSize = newSize + 1;
    for (auto it = split->sizes.begin(); it != split->sizes.end(); it++) {
      (*it) = ((*it) * oldSize) / float(newSize);
    }
  } else {
    // The split pane collapses into a regular pane.
    auto pane = getPane(split->panes_or_splits[0]);
    pane->parentId = split->parentId;
    if (tabs.find(pane->parentId) != tabs.end()) {
      // The parent is a tab, set the child id
      tabs.find(pane->parentId)->second->pane_or_split_id = pane->id;
    } else {
      auto parentSplit = getSplit(pane->parentId);
      for (size_t a = 0; a < parentSplit->panes_or_splits.size(); a++) {
        if (parentSplit->panes_or_splits[a] == split->id) {
          parentSplit->panes_or_splits[a] = pane->id;
          break;
        }
        if (a + 1 == parentSplit->panes_or_splits.size()) {
          STFATAL << "Could not find parent split";
        }
      }
    }
    splits.erase(splits.find(split->id));
  }
}

void MultiplexerState::update(int endpointFd) {
  char header;
  for (auto &it : panes) {
    shared_ptr<TerminalHandler> &terminal = it.second->terminal;
    string terminalData = terminal->pollUserTerminal();
    if (terminalData.length()) {
      const string &paneId = it.first;
      header = APPEND_TO_PANE;
      int32_t length = Base64::EncodedLength(terminalData) + paneId.length();
      VLOG(1) << "WRITING TO " << paneId << ":" << length;
      socketHandler->writeAllOrThrow(endpointFd, (const char *)&header, 1,
                                     false);
      socketHandler->writeB64(endpointFd, (const char *)&length, 4);
      socketHandler->writeAllOrThrow(endpointFd, &(paneId[0]), paneId.length(),
                                     false);
      socketHandler->writeB64(endpointFd, &terminalData[0],
                              terminalData.length());
      VLOG(1) << "WROTE TO " << paneId << ":" << length;
      fflush(stdout);
    }
    if (!terminal->isRunning()) {
      string paneId = it.first;
      closePane(paneId);
      header = SERVER_CLOSE_PANE;
      int32_t length = paneId.length();
      VLOG(1) << "CLOSING " << paneId << ":" << length;
      socketHandler->writeAllOrThrow(endpointFd, (const char *)&header, 1,
                                     false);
      socketHandler->writeB64(endpointFd, (const char *)&length, 4);
      socketHandler->writeAllOrThrow(endpointFd, &(paneId[0]), paneId.length(),
                                     false);
      // Break to avoid erase + iterate of panes
      break;
    }
  }
}

void MultiplexerState::sendTerminalBuffers(int endpointFd) {
  char header;
  for (auto &it : panes) {
    const string &paneId = it.first;
    shared_ptr<TerminalHandler> &terminal = it.second->terminal;
    const deque<string> &terminalBuffer = terminal->getBuffer();
    if (terminalBuffer.size()) {
      int byteCount = 0;
      for (const string &it : terminalBuffer) {
        byteCount += it.length() + 1;
      }
      string terminalData;
      terminalData.reserve(byteCount);
      for (const string &it : terminalBuffer) {
        if (terminalData.length()) {
          terminalData.append(1, '\n');
        }
        terminalData.append(it);
      }
      header = APPEND_TO_PANE;
      int32_t length = Base64::EncodedLength(terminalData) + paneId.length();
      VLOG(1) << "WRITING TO " << paneId << ":" << length;
      socketHandler->writeAllOrThrow(endpointFd, (const char *)&header, 1,
                                     false);
      socketHandler->writeB64(endpointFd, (const char *)&length, 4);
      socketHandler->writeAllOrThrow(endpointFd, &(paneId[0]), paneId.length(),
                                     false);
      socketHandler->writeB64(endpointFd, &terminalData[0],
                              terminalData.length());
      VLOG(1) << "WROTE TO " << paneId << ":" << terminalData.length();
    }
  }
}

void MultiplexerState::resizePane(const string &paneId, int cols, int rows) {
  getPane(paneId)->terminal->updateTerminalSize(cols, rows);
}

void MultiplexerState::fatalIfFound(const string &id) {
  if (panes.find(id) != panes.end()) {
    STFATAL << "Found unexpected id in panes: " << id;
  }
  if (splits.find(id) != splits.end()) {
    STFATAL << "Found unexpected id in splits: " << id;
  }
  if (tabs.find(id) != tabs.end()) {
    STFATAL << "Found unexpected id in tabs: " << id;
  }
}

}  // namespace et
