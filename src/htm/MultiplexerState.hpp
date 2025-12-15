#ifndef __MULTIPLEXER_STATE_HPP__
#define __MULTIPLEXER_STATE_HPP__

#include "Headers.hpp"
#include "SocketHandler.hpp"
#include "TerminalHandler.hpp"

namespace et {
/** @brief Length of the UUID strings used for tabs/panes/splits. */
const int UUID_LENGTH = 36;

/**
 * @brief Keeps track of tabs, splits, and running terminals for HTM sessions.
 *
 * Each pane owns a `TerminalHandler`, and `update()` streams new data back
 * to the client while reacting to pane/split/tab lifecycle events.
 */
class MultiplexerState {
 protected:
  struct Pane;
  struct Split;
  struct Tab;

 public:
  /** @brief Initializes the multiplexer using the supplied IPC handler. */
  MultiplexerState(shared_ptr<SocketHandler> _socketHandler);
  /** @brief Serializes the current tabs/panes/splits into JSON for INIT_STATE.
   */
  string toJsonString();
  /** @brief Sends keystrokes from the client into the pane's terminal. */
  void appendData(const string& uid, const string& data);
  /** @brief Creates a new tab that is backed by a newly spawned terminal. */
  void newTab(const string& tabId, const string& paneId);
  /** @brief Adds a split alongside `sourceId` using the requested orientation.
   */
  void newSplit(const string& sourceId, const string& paneId, bool vertical);
  /** @brief Stops and removes a pane, collapsing its split/tab as needed. */
  void closePane(const string& paneId);
  /** @brief Reads from every `TerminalHandler` and streams data to the client.
   */
  void update(int endpointFd);
  /** @brief Dumps the current terminal buffers whenever a client reconnects. */
  void sendTerminalBuffers(int endpointFd);
  /** @brief Adjusts the terminal window size for a pane. */
  void resizePane(const string& paneId, int cols, int rows);
  /** @brief Returns the current number of active panes. */
  inline int numPanes() { return int(panes.size()); }

 protected:
  /** @brief Socket handler used for all HTM IPC reads/writes. */
  shared_ptr<SocketHandler> socketHandler;
  /** @brief Registered tabs by ID. */
  map<string, shared_ptr<Tab>> tabs;
  /** @brief Active panes keyed by their UUID. */
  map<string, shared_ptr<Pane>> panes;
  /** @brief Split nodes identified by their UUID. */
  map<string, shared_ptr<Split>> splits;
  /** @brief IDs that have already been closed to prevent reuse. */
  set<string> closed;

  /** @brief Fetches a tab object, verifying it exists. */
  inline shared_ptr<Tab> getTab(const string& id) {
    auto it = tabs.find(id);
    if (it == tabs.end()) {
      STFATAL << "Tried to get a pane that doesn't exist: " << id;
    }
    return it->second;
  }
  /** @brief Fetches a pane object, verifying it exists. */
  inline shared_ptr<Pane> getPane(const string& id) {
    auto it = panes.find(id);
    if (it == panes.end()) {
      STFATAL << "Tried to get a pane that doesn't exist: " << id;
    }
    return it->second;
  }
  /** @brief Fetches a split object, verifying it exists. */
  inline shared_ptr<Split> getSplit(const string& id) {
    auto it = splits.find(id);
    if (it == splits.end()) {
      STFATAL << "Tried to get a pane that doesn't exist: " << id;
    }
    return it->second;
  }

  /** @brief Helper that ensures new IDs are unique across tabs/panes/splits. */
  void fatalIfFound(const string& id);
};
}  // namespace et

#endif  // __MULTIPLEXER_STATE_HPP__
