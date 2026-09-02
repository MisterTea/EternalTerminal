#include "MultiplexerState.hpp"

#include <limits.h>

#include <climits>
#include <cmath>

#include "ControlMode.hpp"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef WIN32
#include <unistd.h>
#endif

namespace et {
struct MultiplexerState::Pane {
  uint32_t id = 0;
  uint32_t parentId = 0;  // split or window (windows use window id)
  uint32_t windowId = 0;
  bool parentIsWindow = true;
  string title;
  int x = 0;
  int y = 0;
  int cols = 80;
  int rows = 24;
  shared_ptr<TerminalHandler> terminal;
  unique_ptr<PaneScreen> screen;
  map<string, string> options;
};

struct MultiplexerState::Split {
  uint32_t id = 0;
  uint32_t parentId = 0;
  bool parentIsWindow = true;
  uint32_t windowId = 0;
  bool stacked = false;  // tmux -v / '['
  vector<uint32_t> children;
  vector<float> sizes;
};

struct MultiplexerState::Window {
  uint32_t id = 0;
  uint32_t sessionId = 0;
  string name;
  int order = 0;
  int cols = 80;
  int rows = 24;
  uint32_t rootId = 0;
  bool rootIsPane = true;
  uint32_t activePane = 0;
  uint32_t zoomedPane = 0;
  map<string, string> options;
};

struct MultiplexerState::Session {
  uint32_t id = 0;
  string name;
  vector<uint32_t> windowIds;
  uint32_t activeWindow = 0;
  map<string, string> options;
};

namespace {
string defaultCwd() {
#ifdef WIN32
  const char* home = ::getenv("USERPROFILE");
  return home ? string(home) : string();
#else
  const char* home = ::getenv("HOME");
  return home ? string(home) : string("/");
#endif
}

const uint32_t kNoNode = 0xFFFFFFFFu;

int splitDim(int total, const vector<float>& sizes, size_t index) {
  int used = 0;
  int last = int(sizes.size()) - 1;
  for (int i = 0; i < last; i++) {
    int piece = max(1, int(round(sizes[static_cast<size_t>(i)] * total)));
    if (i == int(index)) {
      return piece;
    }
    used += piece;
  }
  return max(1, total - used);
}
}  // namespace

MultiplexerState::MultiplexerState()
    : writer(nullptr),
      nextSessionId(1),
      nextWindowId(1),
      nextPaneId(0),
      nextSplitId(0x40000000u),
      attachedSession(0),
      width(80),
      height(24) {
  uint32_t sid = newSession("htm");
  attachSession(sid);
}

MultiplexerState::~MultiplexerState() { stopAll(); }

void MultiplexerState::stopAll() {
  for (auto& it : panes) {
    if (it.second && it.second->terminal) {
      it.second->terminal->stop();
    }
  }
}

bool MultiplexerState::empty() const { return panes.empty(); }

void MultiplexerState::notify(const string& line) {
  if (writer) {
    writer->notify(line);
  }
}

shared_ptr<MultiplexerState::Pane> MultiplexerState::makePane(
    uint32_t parentWindow, const string& cwd) {
  auto pane = make_shared<Pane>();
  pane->id = nextPaneId++;
  pane->windowId = parentWindow;
  pane->cols = width;
  pane->rows = height;
  pane->terminal = make_shared<TerminalHandler>();
  pane->terminal->start(cwd.empty() ? defaultCwd() : cwd, pane->cols,
                        pane->rows);
  pane->screen.reset(new PaneScreen(pane->cols, pane->rows));
  panes[pane->id] = pane;
  return pane;
}

uint32_t MultiplexerState::newSession(const string& name) {
  auto session = make_shared<Session>();
  session->id = nextSessionId++;
  session->name = name.empty() ? to_string(session->id) : name;
  sessions[session->id] = session;
  uint32_t prevAttached = attachedSession;
  attachedSession = session->id;
  uint32_t wid = newWindow("", "");
  attachedSession = prevAttached ? prevAttached : session->id;
  session->activeWindow = wid;
  notify("%sessions-changed");
  return session->id;
}

uint32_t MultiplexerState::newWindow(const string& name, const string& cwd) {
  auto session = sessions[attachedSession];
  auto window = make_shared<Window>();
  window->id = nextWindowId++;
  window->sessionId = session->id;
  window->name = name.empty() ? to_string(session->windowIds.size()) : name;
  window->order = int(session->windowIds.size());
  window->cols = width;
  window->rows = height;
  auto pane = makePane(window->id, cwd);
  pane->parentId = window->id;
  pane->parentIsWindow = true;
  window->rootId = pane->id;
  window->rootIsPane = true;
  window->activePane = pane->id;
  windows[window->id] = window;
  session->windowIds.push_back(window->id);
  session->activeWindow = window->id;
  layoutWindow(window.get());
  notify("%window-add @" + to_string(window->id));
  notify("%session-window-changed $" + to_string(session->id) + " @" +
         to_string(window->id));
  emitLayout(window.get());
  return window->id;
}

uint32_t MultiplexerState::splitWindow(uint32_t sourcePane, bool stacked,
                                       const string& cwd) {
  auto src = panes.at(sourcePane);
  auto window = windows.at(src->windowId);
  auto newPane = makePane(window->id, cwd);
  newPane->cols = src->cols;
  newPane->rows = src->rows;

  auto attachToSplit = [&](shared_ptr<Split> split) {
    for (float& sz : split->sizes) {
      sz *= 0.5f;
    }
    split->sizes.push_back(0.5f);
    split->children.push_back(newPane->id);
    newPane->parentId = split->id;
    newPane->parentIsWindow = false;
    newPane->windowId = window->id;
  };

  if (!src->parentIsWindow) {
    auto split = splits[src->parentId];
    if (split && split->stacked == stacked) {
      attachToSplit(split);
      window->activePane = newPane->id;
      layoutWindow(window.get());
      emitLayout(window.get());
      notify("%window-pane-changed @" + to_string(window->id) + " %" +
             to_string(newPane->id));
      return newPane->id;
    }
  }

  auto split = make_shared<Split>();
  split->id = nextSplitId++;
  split->stacked = stacked;
  split->windowId = window->id;
  split->children.push_back(sourcePane);
  split->children.push_back(newPane->id);
  split->sizes.push_back(0.5f);
  split->sizes.push_back(0.5f);
  splits[split->id] = split;

  if (src->parentIsWindow) {
    split->parentId = window->id;
    split->parentIsWindow = true;
    window->rootId = split->id;
    window->rootIsPane = false;
  } else {
    auto parent = splits[src->parentId];
    split->parentId = parent->id;
    split->parentIsWindow = false;
    for (uint32_t& child : parent->children) {
      if (child == sourcePane) {
        child = split->id;
        break;
      }
    }
  }
  src->parentId = split->id;
  src->parentIsWindow = false;
  newPane->parentId = split->id;
  newPane->parentIsWindow = false;
  window->activePane = newPane->id;
  layoutWindow(window.get());
  emitLayout(window.get());
  notify("%window-pane-changed @" + to_string(window->id) + " %" +
         to_string(newPane->id));
  return newPane->id;
}

void MultiplexerState::collectPanes(uint32_t id, vector<uint32_t>* out) const {
  if (id == kNoNode) {
    return;
  }
  if (panes.count(id)) {
    out->push_back(id);
    return;
  }
  auto it = splits.find(id);
  if (it == splits.end()) {
    return;
  }
  for (uint32_t child : it->second->children) {
    collectPanes(child, out);
  }
}

void MultiplexerState::unlinkPaneFromTree(uint32_t paneId,
                                          bool* windowEmptied) {
  auto pane = panes.at(paneId);
  auto windowIt = windows.find(pane->windowId);
  if (windowIt == windows.end()) {
    if (windowEmptied) {
      *windowEmptied = true;
    }
    return;
  }
  auto window = windowIt->second;
  if (pane->parentIsWindow) {
    if (windowEmptied) {
      *windowEmptied = true;
    }
    window->rootId = kNoNode;
    window->rootIsPane = true;
    window->activePane = 0;
    if (window->zoomedPane == paneId) {
      window->zoomedPane = 0;
    }
    return;
  }
  if (windowEmptied) {
    *windowEmptied = false;
  }

  auto split = splits[pane->parentId];
  for (size_t i = 0; i < split->children.size(); i++) {
    if (split->children[i] == paneId) {
      split->children.erase(split->children.begin() + static_cast<long>(i));
      split->sizes.erase(split->sizes.begin() + static_cast<long>(i));
      break;
    }
  }
  float sum = 0;
  for (float s : split->sizes) {
    sum += s;
  }
  if (sum > 0) {
    for (float& s : split->sizes) {
      s /= sum;
    }
  }

  if (split->children.size() == 1) {
    uint32_t remain = split->children[0];
    if (panes.count(remain)) {
      auto remainPane = panes[remain];
      remainPane->parentId = split->parentId;
      remainPane->parentIsWindow = split->parentIsWindow;
    } else if (splits.count(remain)) {
      auto remainSplit = splits[remain];
      remainSplit->parentId = split->parentId;
      remainSplit->parentIsWindow = split->parentIsWindow;
    }
    if (split->parentIsWindow) {
      window->rootId = remain;
      window->rootIsPane = panes.count(remain) != 0;
    } else {
      auto parent = splits[split->parentId];
      for (uint32_t& child : parent->children) {
        if (child == split->id) {
          child = remain;
          break;
        }
      }
    }
    splits.erase(split->id);
  }

  if (window->activePane == paneId || window->zoomedPane == paneId) {
    vector<uint32_t> remaining;
    collectPanes(window->rootId, &remaining);
    window->activePane = remaining.empty() ? 0 : remaining[0];
    if (window->zoomedPane == paneId) {
      window->zoomedPane = 0;
    }
  }
}

void MultiplexerState::relayoutWindow(uint32_t windowId) {
  auto it = windows.find(windowId);
  if (it == windows.end() || it->second->rootId == kNoNode) {
    return;
  }
  layoutWindow(it->second.get());
  emitLayout(it->second.get());
}

void MultiplexerState::closePane(uint32_t paneId) {
  auto it = panes.find(paneId);
  if (it == panes.end()) {
    return;
  }
  auto pane = it->second;
  uint32_t windowId = pane->windowId;
  if (!windows.count(windowId)) {
    pane->terminal->stop();
    panes.erase(it);
    return;
  }
  pane->terminal->stop();
  bool emptied = false;
  unlinkPaneFromTree(paneId, &emptied);
  panes.erase(paneId);
  if (emptied) {
    closeWindow(windowId);
    return;
  }
  auto window = windows[windowId];
  layoutWindow(window.get());
  emitLayout(window.get());
  notify("%window-pane-changed @" + to_string(window->id) + " %" +
         to_string(window->activePane));
}

void MultiplexerState::closeWindow(uint32_t windowId) {
  auto it = windows.find(windowId);
  if (it == windows.end()) {
    return;
  }
  auto window = it->second;
  vector<uint32_t> ids;
  collectPanes(window->rootId, &ids);
  for (uint32_t pid : ids) {
    if (panes.count(pid)) {
      panes[pid]->terminal->stop();
      panes.erase(pid);
    }
  }
  windows.erase(it);
  auto session = sessions[window->sessionId];
  session->windowIds.erase(
      remove(session->windowIds.begin(), session->windowIds.end(), windowId),
      session->windowIds.end());
  notify("%window-close @" + to_string(windowId));
  if (session->windowIds.empty()) {
    sessions.erase(session->id);
    notify("%sessions-changed");
    if (attachedSession == session->id) {
      attachedSession = sessions.empty() ? 0 : sessions.begin()->first;
      if (attachedSession) {
        notify("%session-changed $" + to_string(attachedSession) + " " +
               sessions[attachedSession]->name);
      }
    }
  } else if (session->activeWindow == windowId) {
    session->activeWindow = session->windowIds.back();
    notify("%session-window-changed $" + to_string(session->id) + " @" +
           to_string(session->activeWindow));
  }
}

void MultiplexerState::swapPanes(uint32_t a, uint32_t b) {
  if (a == b || !panes.count(a) || !panes.count(b)) {
    return;
  }
  auto pa = panes[a];
  auto pb = panes[b];
  uint32_t parentA = pa->parentId;
  bool aWin = pa->parentIsWindow;
  uint32_t aWindowId = pa->windowId;
  uint32_t parentB = pb->parentId;
  bool bWin = pb->parentIsWindow;
  uint32_t bWindowId = pb->windowId;

  if (aWin && bWin) {
    windows.at(aWindowId)->rootId = b;
    windows.at(bWindowId)->rootId = a;
  } else if (!aWin && !bWin && parentA == parentB) {
    for (uint32_t& child : splits.at(parentA)->children) {
      if (child == a) {
        child = b;
      } else if (child == b) {
        child = a;
      }
    }
  } else {
    auto replaceChild = [&](bool parentIsWindow, uint32_t parentId,
                            uint32_t windowId, uint32_t from, uint32_t to) {
      if (parentIsWindow) {
        auto w = windows.at(windowId);
        if (w->rootId == from) {
          w->rootId = to;
          w->rootIsPane = true;
        }
        return;
      }
      for (uint32_t& child : splits.at(parentId)->children) {
        if (child == from) {
          child = to;
          break;
        }
      }
    };
    replaceChild(aWin, parentA, aWindowId, a, b);
    replaceChild(bWin, parentB, bWindowId, b, a);
  }

  pa->parentId = parentB;
  pa->parentIsWindow = bWin;
  pa->windowId = bWindowId;
  pb->parentId = parentA;
  pb->parentIsWindow = aWin;
  pb->windowId = aWindowId;

  auto fixFocus = [&](uint32_t windowId, uint32_t incoming) {
    auto w = windows.at(windowId);
    if (!panes.count(w->activePane) ||
        panes[w->activePane]->windowId != windowId) {
      w->activePane = incoming;
    }
    if (w->zoomedPane && (!panes.count(w->zoomedPane) ||
                          panes[w->zoomedPane]->windowId != windowId)) {
      w->zoomedPane = 0;
    }
  };
  fixFocus(aWindowId, b);
  if (bWindowId != aWindowId) {
    fixFocus(bWindowId, a);
  }
  relayoutWindow(aWindowId);
  if (bWindowId != aWindowId) {
    relayoutWindow(bWindowId);
  }
}

void MultiplexerState::insertPaneBeside(uint32_t srcPane, uint32_t destPane,
                                        bool stacked, bool before) {
  auto src = panes.at(srcPane);
  auto dest = panes.at(destPane);
  auto window = windows.at(dest->windowId);
  src->windowId = window->id;

  auto attachToSplit = [&](shared_ptr<Split> split) {
    size_t destIdx = 0;
    for (size_t i = 0; i < split->children.size(); i++) {
      if (split->children[i] == destPane) {
        destIdx = i;
        break;
      }
    }
    size_t at = before ? destIdx : destIdx + 1;
    for (float& sz : split->sizes) {
      sz *= 0.5f;
    }
    split->sizes.insert(split->sizes.begin() + static_cast<long>(at), 0.5f);
    split->children.insert(split->children.begin() + static_cast<long>(at),
                           src->id);
    src->parentId = split->id;
    src->parentIsWindow = false;
  };

  if (!dest->parentIsWindow) {
    auto split = splits[dest->parentId];
    if (split && split->stacked == stacked) {
      attachToSplit(split);
      window->activePane = src->id;
      layoutWindow(window.get());
      emitLayout(window.get());
      notify("%window-pane-changed @" + to_string(window->id) + " %" +
             to_string(src->id));
      return;
    }
  }

  auto split = make_shared<Split>();
  split->id = nextSplitId++;
  split->stacked = stacked;
  split->windowId = window->id;
  if (before) {
    split->children.push_back(src->id);
    split->children.push_back(destPane);
  } else {
    split->children.push_back(destPane);
    split->children.push_back(src->id);
  }
  split->sizes.push_back(0.5f);
  split->sizes.push_back(0.5f);
  splits[split->id] = split;

  if (dest->parentIsWindow) {
    split->parentId = window->id;
    split->parentIsWindow = true;
    window->rootId = split->id;
    window->rootIsPane = false;
  } else {
    auto parent = splits[dest->parentId];
    split->parentId = parent->id;
    split->parentIsWindow = false;
    for (uint32_t& child : parent->children) {
      if (child == destPane) {
        child = split->id;
        break;
      }
    }
  }
  dest->parentId = split->id;
  dest->parentIsWindow = false;
  src->parentId = split->id;
  src->parentIsWindow = false;
  window->activePane = src->id;
  layoutWindow(window.get());
  emitLayout(window.get());
  notify("%window-pane-changed @" + to_string(window->id) + " %" +
         to_string(src->id));
}

void MultiplexerState::movePane(uint32_t src, uint32_t dst, bool stacked,
                                bool before) {
  if (src == dst || !panes.count(src) || !panes.count(dst)) {
    return;
  }
  uint32_t srcWin = panes[src]->windowId;
  uint32_t dstWin = panes[dst]->windowId;
  bool emptied = false;
  unlinkPaneFromTree(src, &emptied);
  if (!emptied && srcWin != dstWin) {
    relayoutWindow(srcWin);
    auto sw = windows[srcWin];
    notify("%window-pane-changed @" + to_string(sw->id) + " %" +
           to_string(sw->activePane));
  }
  insertPaneBeside(src, dst, stacked, before);
  if (emptied && srcWin != dstWin) {
    closeWindow(srcWin);
  }
}

uint32_t MultiplexerState::breakPane(uint32_t paneId) {
  if (!panes.count(paneId)) {
    return 0;
  }
  auto pane = panes[paneId];
  uint32_t oldWid = pane->windowId;
  auto oldWindow = windows.at(oldWid);
  vector<uint32_t> ids;
  collectPanes(oldWindow->rootId, &ids);
  if (ids.size() <= 1) {
    return oldWid;
  }
  uint32_t sid = oldWindow->sessionId;
  bool emptied = false;
  unlinkPaneFromTree(paneId, &emptied);
  relayoutWindow(oldWid);

  auto session = sessions[sid];
  auto window = make_shared<Window>();
  window->id = nextWindowId++;
  window->sessionId = session->id;
  window->name = to_string(session->windowIds.size());
  window->order = int(session->windowIds.size());
  window->cols = width;
  window->rows = height;
  pane->parentId = window->id;
  pane->parentIsWindow = true;
  pane->windowId = window->id;
  window->rootId = pane->id;
  window->rootIsPane = true;
  window->activePane = pane->id;
  windows[window->id] = window;
  session->windowIds.push_back(window->id);
  session->activeWindow = window->id;
  layoutWindow(window.get());
  notify("%window-add @" + to_string(window->id));
  notify("%session-window-changed $" + to_string(session->id) + " @" +
         to_string(window->id));
  emitLayout(window.get());
  if (emptied) {
    closeWindow(oldWid);
  }
  return window->id;
}

void MultiplexerState::moveWindowToSession(uint32_t windowId,
                                           uint32_t sessionId) {
  if (!windows.count(windowId) || !sessions.count(sessionId)) {
    return;
  }
  auto window = windows[windowId];
  auto src = sessions[window->sessionId];
  auto dst = sessions[sessionId];
  src->windowIds.erase(
      remove(src->windowIds.begin(), src->windowIds.end(), windowId),
      src->windowIds.end());
  if (src->activeWindow == windowId) {
    src->activeWindow = src->windowIds.empty() ? 0 : src->windowIds.back();
    if (src->activeWindow) {
      notify("%session-window-changed $" + to_string(src->id) + " @" +
             to_string(src->activeWindow));
    }
  }
  window->sessionId = sessionId;
  window->order = int(dst->windowIds.size());
  dst->windowIds.push_back(windowId);
  dst->activeWindow = windowId;
  notify("%session-window-changed $" + to_string(dst->id) + " @" +
         to_string(windowId));
  if (src->id != dst->id && src->windowIds.empty()) {
    sessions.erase(src->id);
    notify("%sessions-changed");
    if (attachedSession == src->id) {
      attachedSession = dst->id;
      notify("%session-changed $" + to_string(attachedSession) + " " +
             dst->name);
    }
  }
}

map<string, string>* MultiplexerState::optionStore(char scope,
                                                   uint32_t targetId) {
  return const_cast<map<string, string>*>(
      static_cast<const MultiplexerState*>(this)->optionStore(scope, targetId));
}

const map<string, string>* MultiplexerState::optionStore(
    char scope, uint32_t targetId) const {
  switch (scope) {
    case 'g':
      return &globalOptions;
    case 's':
      return &serverOptions;
    case 'p': {
      auto it = panes.find(targetId);
      return it == panes.end() ? nullptr : &it->second->options;
    }
    case 'w': {
      auto it = windows.find(targetId);
      return it == windows.end() ? nullptr : &it->second->options;
    }
    default: {
      auto it = sessions.find(targetId);
      return it == sessions.end() ? nullptr : &it->second->options;
    }
  }
}

void MultiplexerState::setUserOption(char scope, uint32_t targetId,
                                     const string& name, const string& value,
                                     bool append) {
  if (name.empty() || name[0] != '@') {
    return;
  }
  auto* store = optionStore(scope, targetId);
  if (!store) {
    return;
  }
  if (append) {
    (*store)[name] += value;
  } else {
    (*store)[name] = value;
  }
}

void MultiplexerState::unsetUserOption(char scope, uint32_t targetId,
                                       const string& name) {
  auto* store = optionStore(scope, targetId);
  if (!store) {
    return;
  }
  store->erase(name);
}

string MultiplexerState::getUserOption(char scope, uint32_t targetId,
                                       const string& name) const {
  auto* store = optionStore(scope, targetId);
  if (!store) {
    return "";
  }
  auto it = store->find(name);
  return it == store->end() ? "" : it->second;
}

void MultiplexerState::closeSession(uint32_t sessionId) {
  auto it = sessions.find(sessionId);
  if (it == sessions.end()) {
    return;
  }
  auto session = it->second;
  auto ids = session->windowIds;
  for (uint32_t wid : ids) {
    if (windows.count(wid)) {
      closeWindow(wid);
    }
  }
  sessions.erase(sessionId);
  notify("%sessions-changed");
  if (attachedSession == sessionId) {
    if (!sessions.empty()) {
      attachedSession = sessions.begin()->first;
      notify("%session-changed $" + to_string(attachedSession) + " " +
             sessions[attachedSession]->name);
    } else {
      attachedSession = 0;
    }
  }
}

void MultiplexerState::renameSession(uint32_t sessionId, const string& name) {
  sessions.at(sessionId)->name = name;
  notify("%session-renamed $" + to_string(sessionId) + " " + name);
}

void MultiplexerState::attachSession(uint32_t sessionId) {
  attachedSession = sessionId;
  auto session = sessions.at(sessionId);
  notify("%session-changed $" + to_string(sessionId) + " " + session->name);
}

void MultiplexerState::sendKeys(uint32_t paneId, const string& data) {
  panes.at(paneId)->terminal->appendData(data);
}

void MultiplexerState::selectPane(uint32_t paneId) {
  auto pane = panes.at(paneId);
  auto window = windows.at(pane->windowId);
  window->activePane = paneId;
  sessions[window->sessionId]->activeWindow = window->id;
  notify("%window-pane-changed @" + to_string(window->id) + " %" +
         to_string(paneId));
}

void MultiplexerState::selectWindow(uint32_t windowId) {
  auto window = windows.at(windowId);
  sessions[window->sessionId]->activeWindow = windowId;
  notify("%session-window-changed $" + to_string(window->sessionId) + " @" +
         to_string(windowId));
}

void MultiplexerState::renameWindow(uint32_t windowId, const string& name) {
  windows.at(windowId)->name = name;
  notify("%window-renamed @" + to_string(windowId) + " " + name);
}

void MultiplexerState::setPaneTitle(uint32_t paneId, const string& title) {
  panes.at(paneId)->title = title;
}

void MultiplexerState::setClientSize(int cols, int rows) {
  width = max(1, cols);
  height = max(1, rows);
  if (!attachedSession || !sessions.count(attachedSession)) {
    return;
  }
  for (uint32_t wid : sessions[attachedSession]->windowIds) {
    setWindowSize(wid, width, height);
  }
}

void MultiplexerState::setWindowSize(uint32_t windowId, int cols, int rows) {
  auto window = windows.at(windowId);
  window->cols = max(1, cols);
  window->rows = max(1, rows);
  layoutWindow(window.get());
  emitLayout(window.get());
}

void MultiplexerState::zoomToggle(uint32_t paneId) {
  auto pane = panes.at(paneId);
  auto window = windows.at(pane->windowId);
  if (window->zoomedPane) {
    window->zoomedPane = 0;
  } else {
    window->zoomedPane = paneId;
  }
  layoutWindow(window.get());
  emitLayout(window.get());
}

void MultiplexerState::resizePaneDir(uint32_t paneId, char dir, int amount) {
  auto pane = panes.at(paneId);
  if (pane->parentIsWindow) {
    return;
  }
  auto split = splits[pane->parentId];
  bool horizMove = (dir == 'L' || dir == 'R');
  if (split->stacked == horizMove) {
    return;
  }
  size_t idx = 0;
  for (; idx < split->children.size(); idx++) {
    if (split->children[idx] == paneId) {
      break;
    }
  }
  if (idx >= split->children.size()) {
    return;
  }
  size_t neighbor = idx;
  if (dir == 'L' || dir == 'U') {
    if (idx == 0) {
      return;
    }
    neighbor = idx - 1;
  } else {
    if (idx + 1 >= split->children.size()) {
      return;
    }
    neighbor = idx + 1;
  }
  int total = split->stacked ? pane->rows : pane->cols;
  (void)total;
  float delta = amount / 100.0f;
  if (dir == 'L' || dir == 'U') {
    split->sizes[idx] += delta;
    split->sizes[neighbor] -= delta;
  } else {
    split->sizes[idx] += delta;
    split->sizes[neighbor] -= delta;
  }
  for (float& s : split->sizes) {
    if (s < 0.05f) {
      s = 0.05f;
    }
  }
  float sum = 0;
  for (float s : split->sizes) {
    sum += s;
  }
  for (float& s : split->sizes) {
    s /= sum;
  }
  auto window = windows[pane->windowId];
  layoutWindow(window.get());
  emitLayout(window.get());
}

void MultiplexerState::selectLayout(uint32_t windowId, const string& layout) {
  auto window = windows.at(windowId);
  vector<uint32_t> ids;
  collectPanes(window->rootId, &ids);
  if (ids.size() < 2) {
    return;
  }
  bool stacked = (layout.find("vertical") != string::npos ||
                  layout.find("even-vertical") != string::npos);
  auto split = make_shared<Split>();
  split->id = nextSplitId++;
  split->stacked =
      stacked || layout == "even-vertical" || layout == "main-horizontal";
  if (layout == "even-horizontal" || layout == "main-vertical") {
    split->stacked = false;
  }
  split->windowId = window->id;
  split->parentIsWindow = true;
  split->parentId = window->id;
  float even = 1.0f / float(ids.size());
  for (uint32_t pid : ids) {
    split->children.push_back(pid);
    split->sizes.push_back(even);
    panes[pid]->parentId = split->id;
    panes[pid]->parentIsWindow = false;
  }
  splits[split->id] = split;
  window->rootId = split->id;
  window->rootIsPane = false;
  layoutWindow(window.get());
  emitLayout(window.get());
}

void MultiplexerState::layoutNode(uint32_t id, int x, int y, int cols,
                                  int rows) {
  if (panes.count(id)) {
    auto pane = panes[id];
    pane->x = x;
    pane->y = y;
    pane->cols = max(1, cols);
    pane->rows = max(1, rows);
    applyPaneSize(pane.get());
    return;
  }
  auto split = splits.at(id);
  int cursor = split->stacked ? y : x;
  int total = split->stacked ? rows : cols;
  for (size_t i = 0; i < split->children.size(); i++) {
    int piece = splitDim(total, split->sizes, i);
    if (split->stacked) {
      layoutNode(split->children[i], x, cursor, cols, piece);
      cursor += piece;
    } else {
      layoutNode(split->children[i], cursor, y, piece, rows);
      cursor += piece;
    }
  }
}

void MultiplexerState::applyPaneSize(Pane* pane) {
  if (pane->terminal) {
    pane->terminal->updateTerminalSize(pane->cols, pane->rows);
  }
  if (pane->screen) {
    pane->screen->resize(pane->cols, pane->rows);
  }
}

void MultiplexerState::layoutWindow(Window* window) {
  if (window->rootId == kNoNode) {
    return;
  }
  if (window->zoomedPane && panes.count(window->zoomedPane)) {
    auto pane = panes[window->zoomedPane];
    pane->x = 0;
    pane->y = 0;
    pane->cols = window->cols;
    pane->rows = window->rows;
    applyPaneSize(pane.get());
    return;
  }
  layoutNode(window->rootId, 0, 0, window->cols, window->rows);
}

uint16_t MultiplexerState::layoutChecksum(const string& layout) const {
  uint16_t csum = 0;
  for (unsigned char c : layout) {
    csum = static_cast<uint16_t>((csum >> 1) + ((csum & 1) << 15) + c);
  }
  return csum;
}

string MultiplexerState::dumpNode(uint32_t id) const {
  if (panes.count(id)) {
    auto p = panes.at(id);
    return to_string(p->cols) + "x" + to_string(p->rows) + "," +
           to_string(p->x) + "," + to_string(p->y) + "," + to_string(p->id);
  }
  auto split = splits.at(id);
  string inner;
  for (size_t i = 0; i < split->children.size(); i++) {
    if (i) {
      inner += ",";
    }
    inner += dumpNode(split->children[i]);
  }
  char wrapOpen = split->stacked ? '[' : '{';
  char wrapClose = split->stacked ? ']' : '}';
  // Use the first child's origin and combined size from the split parent
  // window if this is the root; otherwise from first pane in the subtree.
  vector<uint32_t> ids;
  collectPanes(id, &ids);
  int x = 0, y = 0, cols = width, rows = height;
  if (!ids.empty()) {
    auto first = panes.at(ids.front());
    auto last = panes.at(ids.back());
    x = first->x;
    y = first->y;
    if (split->stacked) {
      cols = first->cols;
      rows = last->y + last->rows - first->y;
    } else {
      rows = first->rows;
      cols = last->x + last->cols - first->x;
    }
  }
  return to_string(cols) + "x" + to_string(rows) + "," + to_string(x) + "," +
         to_string(y) + wrapOpen + inner + wrapClose;
}

string MultiplexerState::dumpLayout(uint32_t windowId, bool visible) const {
  auto window = windows.at(windowId);
  string body;
  if (visible && window->zoomedPane && panes.count(window->zoomedPane)) {
    auto p = panes.at(window->zoomedPane);
    body = to_string(window->cols) + "x" + to_string(window->rows) + ",0,0," +
           to_string(p->id);
  } else if (window->rootId == kNoNode) {
    body = to_string(window->cols) + "x" + to_string(window->rows) + ",0,0";
  } else {
    body = dumpNode(window->rootId);
  }
  char buf[8];
  snprintf(buf, sizeof(buf), "%04x", layoutChecksum(body));
  return string(buf) + "," + body;
}

string MultiplexerState::windowRawFlags(const Window* window) const {
  string f;
  auto sit = sessions.find(window->sessionId);
  if (sit != sessions.end() && sit->second->activeWindow == window->id) {
    f.push_back('*');
  }
  if (window->zoomedPane) {
    f.push_back('Z');
  }
  return f;
}

void MultiplexerState::emitLayout(Window* window) {
  // tmux 3.x: %layout-change #{window_id} #{window_layout}
  // #{window_visible_layout} #{window_raw_flags}
  // Always include the flags field (possibly empty) so the space before it
  // remains; clients such as WezTerm require LAYOUT LAYOUT FLAGS.
  notify("%layout-change @" + to_string(window->id) + " " +
         dumpLayout(window->id, false) + " " + dumpLayout(window->id, true) +
         " " + windowRawFlags(window));
}

void MultiplexerState::attachNotifications() {
  if (!attachedSession || !sessions.count(attachedSession)) {
    return;
  }
  auto session = sessions[attachedSession];
  notify("%sessions-changed");
  notify("%session-changed $" + to_string(session->id) + " " + session->name);
  for (uint32_t wid : session->windowIds) {
    notify("%window-add @" + to_string(wid));
    emitLayout(windows[wid].get());
  }
  notify("%session-window-changed $" + to_string(session->id) + " @" +
         to_string(session->activeWindow));
  auto window = windows[session->activeWindow];
  notify("%window-pane-changed @" + to_string(window->id) + " %" +
         to_string(window->activePane));
}

void MultiplexerState::pollOutput() {
  vector<uint32_t> dead;
  for (auto& it : panes) {
    auto pane = it.second;
    string data = pane->terminal->pollUserTerminal();
    if (!data.empty()) {
      if (pane->screen) {
        pane->screen->feed(data);
      }
      string gate = clientFlags.paneGate.count(pane->id)
                        ? clientFlags.paneGate[pane->id]
                        : string("on");
      if (!clientFlags.noOutput && gate != "off" && gate != "pause") {
        if (clientFlags.pauseAfterSec >= 0) {
          notify("%extended-output %" + to_string(pane->id) +
                 " 0 : " + controlOctalEscape(data));
          if (clientFlags.pauseAfterSec == 0) {
            clientFlags.paneGate[pane->id] = "pause";
            notify("%pause %" + to_string(pane->id));
          }
        } else {
          notify("%output %" + to_string(pane->id) + " " +
                 controlOctalEscape(data));
        }
      }
    }
    if (!pane->terminal->isRunning()) {
      dead.push_back(pane->id);
    }
  }
  for (uint32_t id : dead) {
    closePane(id);
  }
}

string MultiplexerState::paneCwd(Pane* pane) const {
#ifdef __linux__
  int64_t pid = pane->terminal->childProcessId();
  if (pid > 0) {
    char buf[PATH_MAX];
    string link = "/proc/" + to_string(pid) + "/cwd";
    ssize_t n = readlink(link.c_str(), buf, sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = '\0';
      return string(buf);
    }
  }
#endif
  (void)pane;
  return defaultCwd();
}

string MultiplexerState::expandOne(const string& name, Session* session,
                                   Window* window, Pane* pane) {
  if (name == "version") {
    return HTM_TMUX_VERSION;
  }
  if (name == "socket_path") {
    return "htm";
  }
  if (name == "pid") {
    return "0";
  }
  if (name == "pane-border-status" || name == "pane_border_status") {
    return "off";
  }
  if (session) {
    if (name == "session_id") {
      return "$" + to_string(session->id);
    }
    if (name == "session_name") {
      return session->name;
    }
  }
  if (window) {
    if (name == "window_id") {
      return "@" + to_string(window->id);
    }
    if (name == "window_name") {
      return window->name;
    }
    if (name == "window_index") {
      return to_string(window->order);
    }
    if (name == "window_layout") {
      return dumpLayout(window->id, false);
    }
    if (name == "window_visible_layout") {
      return dumpLayout(window->id, true);
    }
    if (name == "window_flags") {
      string f = windowRawFlags(window);
      return f.empty() ? "-" : f;
    }
    if (name == "window_raw_flags") {
      return windowRawFlags(window);
    }
    if (name == "window_width") {
      return to_string(window->cols);
    }
    if (name == "window_height") {
      return to_string(window->rows);
    }
    if (name == "window_active") {
      return (session && session->activeWindow == window->id) ? "1" : "0";
    }
    if (name == "history_limit") {
      return "2000";
    }
  }
  if (pane) {
    if (name == "pane_id") {
      return "%" + to_string(pane->id);
    }
    if (name == "pane_width") {
      return to_string(pane->cols);
    }
    if (name == "pane_height") {
      return to_string(pane->rows);
    }
    if (name == "pane_left") {
      return to_string(pane->x);
    }
    if (name == "pane_top") {
      return to_string(pane->y);
    }
    if (name == "pane_index") {
      return "0";
    }
    if (name == "pane_title") {
      return pane->title;
    }
    if (name == "pane_current_path") {
      return paneCwd(pane);
    }
    if (name == "pane_dead") {
      return pane->terminal->isRunning() ? "0" : "1";
    }
    if (name == "pane_pid") {
      return to_string(pane->terminal->childProcessId());
    }
    if (name == "pane_active") {
      auto w = windows[pane->windowId];
      return w->activePane == pane->id ? "1" : "0";
    }
    int cx = 0, cy = 0;
    if (pane->screen) {
      pane->screen->cursor(&cx, &cy);
    }
    if (name == "cursor_x") {
      return to_string(cx);
    }
    if (name == "cursor_y") {
      return to_string(cy);
    }
    if (name == "cursor_flag") {
      return pane->screen && pane->screen->cursorVisible() ? "1" : "0";
    }
    if (name == "alternate_on") {
      return pane->screen && pane->screen->alternateScreen() ? "1" : "0";
    }
    if (name == "alternate_saved_x" || name == "alternate_saved_y") {
      return "0";
    }
    if (name == "insert_flag" || name == "wrap_flag" || name == "keypad_flag" ||
        name == "keypad_cursor_flag" || name == "origin_flag" ||
        name == "focus_flag" || name == "bracketed_paste") {
      return "0";
    }
    if (name == "scroll_region_upper") {
      return "0";
    }
    if (name == "scroll_region_lower") {
      return pane->screen ? to_string(pane->screen->rows() - 1) : "0";
    }
    if (name == "pane_tabs") {
      return "";
    }
    if (name == "mouse_standard_flag" || name == "mouse_button_flag" ||
        name == "mouse_any_flag" || name == "mouse_utf8_flag" ||
        name == "mouse_sgr_flag") {
      return "0";
    }
    if (name == "cursor_shape") {
      return "0";
    }
    if (name == "cursor_colour") {
      return "";
    }
    if (name == "cursor_blinking") {
      return "0";
    }
  }
  return "";
}

string MultiplexerState::expand(const string& format, Session* session,
                                Window* window, Pane* pane) {
  string out;
  for (size_t i = 0; i < format.size(); i++) {
    if (format[i] == '#' && i + 1 < format.size() && format[i + 1] == '{') {
      int depth = 0;
      size_t end = string::npos;
      for (size_t j = i + 1; j < format.size(); j++) {
        if (format[j] == '{') {
          depth++;
        } else if (format[j] == '}') {
          depth--;
          if (depth == 0) {
            end = j;
            break;
          }
        }
      }
      if (end == string::npos) {
        out.push_back(format[i]);
        continue;
      }
      string inner = format.substr(i + 2, end - (i + 2));
      if (inner.size() > 2 && inner[0] == 'q' && inner[1] == ':') {
        inner = inner.substr(2);
      }
      if (!inner.empty() && inner[0] == '?') {
        string spec = inner.substr(1);
        vector<string> parts;
        string cur;
        int partDepth = 0;
        for (char c : spec) {
          if (c == '{') {
            partDepth++;
          } else if (c == '}') {
            partDepth--;
          }
          if (c == ',' && partDepth == 0) {
            parts.push_back(cur);
            cur.clear();
            continue;
          }
          cur.push_back(c);
        }
        parts.push_back(cur);
        string cond = parts.empty() ? string() : parts[0];
        string yes = parts.size() > 1 ? parts[1] : string();
        string no = parts.size() > 2 ? parts[2] : string();
        string condVal = cond.find("#{") != string::npos
                             ? expand(cond, session, window, pane)
                             : expandOne(cond, session, window, pane);
        bool truthy = !condVal.empty() && condVal != "0";
        string chosen = truthy ? yes : no;
        out += chosen.find("#{") != string::npos
                   ? expand(chosen, session, window, pane)
                   : chosen;
        i = end;
        continue;
      }
      out += expandOne(inner, session, window, pane);
      i = end;
      continue;
    }
    out.push_back(format[i]);
  }
  return out;
}

string MultiplexerState::listSessions(const string& format) {
  string fmt = format.empty() ? "#{session_id} #{session_name}" : format;
  string out;
  for (auto& it : sessions) {
    if (!out.empty()) {
      out += "\n";
    }
    out += expand(fmt, it.second.get(), nullptr, nullptr);
  }
  return out;
}

string MultiplexerState::listWindows(const string& format, uint32_t sessionId) {
  string fmt = format.empty() ? "#{window_id} #{window_layout} #{window_flags} "
                                "#{window_visible_layout}"
                              : format;
  auto session = sessions.at(sessionId ? sessionId : attachedSession);
  string out;
  for (uint32_t wid : session->windowIds) {
    auto window = windows[wid];
    Pane* pane = panes.count(window->activePane)
                     ? panes[window->activePane].get()
                     : nullptr;
    if (!out.empty()) {
      out += "\n";
    }
    out += expand(fmt, session.get(), window.get(), pane);
  }
  return out;
}

string MultiplexerState::listPanes(const string& format, uint32_t windowId) {
  string fmt =
      format.empty() ? "#{pane_id} #{pane_width} #{pane_height}" : format;
  uint32_t wid = windowId
                     ? windowId
                     : windows[sessions[attachedSession]->activeWindow]->id;
  if (panes.count(windowId)) {
    wid = panes[windowId]->windowId;
  }
  auto window = windows.at(wid);
  auto session = sessions[window->sessionId];
  vector<uint32_t> ids;
  collectPanes(window->rootId, &ids);
  string out;
  for (uint32_t pid : ids) {
    if (!out.empty()) {
      out += "\n";
    }
    out += expand(fmt, session.get(), window.get(), panes[pid].get());
  }
  return out;
}

string MultiplexerState::listAllPanes(const string& format) {
  if (!attachedSession || !sessions.count(attachedSession)) {
    return "";
  }
  string out;
  for (uint32_t wid : sessions[attachedSession]->windowIds) {
    string chunk = listPanes(format, wid);
    if (chunk.empty()) {
      continue;
    }
    if (!out.empty()) {
      out += "\n";
    }
    out += chunk;
  }
  return out;
}

string MultiplexerState::capturePane(uint32_t paneId, bool escapes, bool alt,
                                     int startLine, int endLine, bool joinWrap,
                                     bool preserveTrailing) {
  return panes.at(paneId)->screen->capture(escapes, alt, startLine, endLine,
                                           joinWrap, preserveTrailing);
}

string MultiplexerState::displayFormat(const string& format, uint32_t sessionId,
                                       uint32_t windowId, uint32_t paneId) {
  Session* session = sessions.count(sessionId)
                         ? sessions[sessionId].get()
                         : sessions[attachedSession].get();
  Window* window = nullptr;
  Pane* pane = nullptr;
  if (windowId && windows.count(windowId)) {
    window = windows[windowId].get();
  } else if (session) {
    window = windows[session->activeWindow].get();
  }
  if (paneId && panes.count(paneId)) {
    pane = panes[paneId].get();
  } else if (window) {
    pane = panes[window->activePane].get();
  }
  return expand(format, session, window, pane);
}

uint32_t MultiplexerState::activePaneId() const {
  if (!attachedSession || !sessions.count(attachedSession)) {
    return 0;
  }
  auto window = windows.at(sessions.at(attachedSession)->activeWindow);
  return window->activePane;
}

uint32_t MultiplexerState::activeWindowId() const {
  if (!attachedSession || !sessions.count(attachedSession)) {
    return 0;
  }
  return sessions.at(attachedSession)->activeWindow;
}

uint32_t MultiplexerState::parsePaneTarget(const string& target) {
  if (target.empty()) {
    return activePaneId();
  }
  string t = target;
  if (t[0] == '%') {
    return static_cast<uint32_t>(stoul(t.substr(1)));
  }
  if (t[0] == '@') {
    auto window = windows.at(static_cast<uint32_t>(stoul(t.substr(1))));
    return window->activePane;
  }
  if (panes.count(static_cast<uint32_t>(stoul(t)))) {
    return static_cast<uint32_t>(stoul(t));
  }
  return activePaneId();
}

uint32_t MultiplexerState::parseWindowTarget(const string& target) {
  if (target.empty()) {
    return activeWindowId();
  }
  string t = target;
  size_t at = t.find('@');
  if (at != string::npos && at + 1 < t.size()) {
    return static_cast<uint32_t>(stoul(t.substr(at + 1)));
  }
  if (t[0] == '%') {
    return panes.at(static_cast<uint32_t>(stoul(t.substr(1))))->windowId;
  }
  return activeWindowId();
}

uint32_t MultiplexerState::parseSessionTarget(const string& target) {
  if (target.empty()) {
    return attachedSession;
  }
  string t = target;
  if (t[0] == '$') {
    return static_cast<uint32_t>(stoul(t.substr(1)));
  }
  for (auto& it : sessions) {
    if (it.second->name == t) {
      return it.first;
    }
  }
  return attachedSession;
}

}  // namespace et
