# Issue 782 — Reconnect status indicator

Status: bounded UX improvement using connection state the client already knows (ClientConnection reconnectThread, getSocketFd, isShuttingDown).

Approach: non-invasive status-line via escape sequence (`\033[31mRECONNECTING\033[0m`) when `getSocketFd() < 0` and reconnect active. Terminal compatibility covered in `ClientReconnectStatusTest.cpp`.
