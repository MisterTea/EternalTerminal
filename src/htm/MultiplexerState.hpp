#ifndef __MULTIPLEXER_STATE_HPP__
#define __MULTIPLEXER_STATE_HPP__

#include "ControlMode.hpp"
#include "Headers.hpp"
#include "PaneScreen.hpp"
#include "SocketHandler.hpp"
#include "TerminalHandler.hpp"

namespace et {

struct ControlClientFlags {
  int pauseAfterSec = -1;
  bool waitExit = false;
  bool noOutput = false;
  map<uint32_t, string> paneGate;  // "on" "off" "pause"
};

/**
 * @brief Tabs/windows, splits, and panes for HTM control mode.
 */
class MultiplexerState {
 public:
  MultiplexerState();
  ~MultiplexerState();

  void setWriter(ControlWriter* writer) { this->writer = writer; }
  void attachNotifications();
  void pollOutput();
  void stopAll();
  bool empty() const;

  uint32_t newWindow(const string& name, const string& cwd);
  uint32_t splitWindow(uint32_t sourcePane, bool stacked, const string& cwd);
  void closePane(uint32_t paneId);
  void closeWindow(uint32_t windowId);
  void swapPanes(uint32_t a, uint32_t b);
  void movePane(uint32_t src, uint32_t dst, bool stacked, bool before);
  uint32_t breakPane(uint32_t paneId);
  void moveWindowToSession(uint32_t windowId, uint32_t sessionId);
  void setUserOption(char scope, uint32_t targetId, const string& name,
                     const string& value, bool append = false);
  void unsetUserOption(char scope, uint32_t targetId, const string& name);
  string getUserOption(char scope, uint32_t targetId, const string& name) const;
  void sendKeys(uint32_t paneId, const string& data);
  void selectPane(uint32_t paneId);
  void selectWindow(uint32_t windowId);
  void resizePaneDir(uint32_t paneId, char dir, int amount);
  void zoomToggle(uint32_t paneId);
  void setClientSize(int cols, int rows);
  void setWindowSize(uint32_t windowId, int cols, int rows);
  void renameWindow(uint32_t windowId, const string& name);
  void setPaneTitle(uint32_t paneId, const string& title);
  void selectLayout(uint32_t windowId, const string& layout);

  uint32_t newSession(const string& name);
  void closeSession(uint32_t sessionId);
  void renameSession(uint32_t sessionId, const string& name);
  void attachSession(uint32_t sessionId);

  string listSessions(const string& format);
  string listWindows(const string& format, uint32_t sessionId);
  string listPanes(const string& format, uint32_t windowId);
  string listAllPanes(const string& format);
  string capturePane(uint32_t paneId, bool escapes, bool alt, int startLine,
                     int endLine, bool joinWrap, bool preserveTrailing);
  string displayFormat(const string& format, uint32_t sessionId,
                       uint32_t windowId, uint32_t paneId);

  uint32_t parsePaneTarget(const string& target);
  uint32_t parseWindowTarget(const string& target);
  uint32_t parseSessionTarget(const string& target);
  uint32_t activePaneId() const;
  uint32_t activeWindowId() const;
  uint32_t activeSessionId() const { return attachedSession; }
  int clientCols() const { return width; }
  int clientRows() const { return height; }
  bool hasPane(uint32_t id) const { return panes.count(id) != 0; }
  bool hasWindow(uint32_t id) const { return windows.count(id) != 0; }
  bool hasSession(uint32_t id) const { return sessions.count(id) != 0; }
  int numPanes() const { return int(panes.size()); }

  ControlClientFlags clientFlags;
  map<string, string> pasteBuffers;

  uint16_t layoutChecksum(const string& layout) const;
  string dumpLayout(uint32_t windowId, bool visible) const;

 protected:
  struct Pane;
  struct Split;
  struct Window;
  struct Session;

  ControlWriter* writer;
  uint32_t nextSessionId;
  uint32_t nextWindowId;
  uint32_t nextPaneId;
  uint32_t nextSplitId;
  uint32_t attachedSession;
  int width;
  int height;

  map<uint32_t, shared_ptr<Session>> sessions;
  map<uint32_t, shared_ptr<Window>> windows;
  map<uint32_t, shared_ptr<Pane>> panes;
  map<uint32_t, shared_ptr<Split>> splits;
  map<string, string> globalOptions;
  map<string, string> serverOptions;

  shared_ptr<Pane> makePane(uint32_t parentWindow, const string& cwd);
  void layoutWindow(Window* window);
  void layoutNode(uint32_t id, int x, int y, int cols, int rows);
  string dumpNode(uint32_t id) const;
  void emitLayout(Window* window);
  string windowRawFlags(const Window* window) const;
  void notify(const string& line);
  string expand(const string& format, Session* session, Window* window,
                Pane* pane);
  string expandOne(const string& name, Session* session, Window* window,
                   Pane* pane);
  void applyPaneSize(Pane* pane);
  void collectPanes(uint32_t id, vector<uint32_t>* out) const;
  string paneCwd(Pane* pane) const;
  void unlinkPaneFromTree(uint32_t paneId, bool* windowEmptied);
  void insertPaneBeside(uint32_t srcPane, uint32_t destPane, bool stacked,
                        bool before);
  void relayoutWindow(uint32_t windowId);
  map<string, string>* optionStore(char scope, uint32_t targetId);
  const map<string, string>* optionStore(char scope, uint32_t targetId) const;
};

}  // namespace et

#endif
