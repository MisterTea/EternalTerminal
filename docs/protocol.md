# EternalTerminal Protocol

## Architecture 

![Connection Overview](images/connection_overview.png)

EternalTerminal uses three processes:
* **et** on the client machine.
* **etterminal** which runs as the user and hosts the terminal.
* **etserver** which connects users to their terminals.

## Terminal Connection

```mermaid
sequenceDiagram
    participant et
    participant etserver
    participant etterminal
    
    et->>etterminal: launch (over ssh)

    etterminal->>etserver: TerminalUserInfo (client id, passkey)
    etterminal->>et: client id and passkey

    loop
      etserver->>etserver: Wait for client connect<br>(see Client Connection)
    end

    etserver->>etterminal: TermInit

    et->>etserver: TerminalBuffer input (encrypted)
    etserver-->etterminal: terminal in/out
    etserver->>et: TerminalBuffer output (encrypted)
```

EternalTerminal uses SSH for authentication, and provides reconnectable sessions through the etserver process.

### etterminal launch

To start, et (which runs on the client), connects to the server over ssh to launch the etterminal process. Due to legacy reasons, the client sends the id and passkey to the server, but for supported clients the server will regenerate them and send updated ones to the client.

In [`SshSetupHandler.cpp`](https://github.com/MisterTea/EternalTerminal/blob/113fb23133eabce3d11681392d75ba4772814b44/src/terminal/SshSetupHandler.cpp#L11-L12), a command like the following is constructed:

```sh
echo 'XXX1234567890123/12345678901234567890123456789012_xterm-256color' | etterminal
```

The string is composed of a 16-character **client-id**, followed by a slash (`/`), followed by a 32-character **passkey**, an underscore (`_`), and value of the `$TERM` environment variable.

The **client-id** begins with "XXX" for newer clients to indicate that the server should regenerate it.

Once etterminal launches:
- It detects if the **client-id** begins with "XXX", and optionally regenerates the **client-id** and **passkey**.
- It [locates the server fifo](https://github.com/MisterTea/EternalTerminal/blob/113fb23133eabce3d11681392d75ba4772814b44/src/terminal/ServerFifoPath.cpp) to connect to the etserver process:
  - If `/var/run/etserver.idpasskey.fifo` exists, when etserver is running as root, this path is used.
  - Otherwise, `$XDG_RUNTIME_DIR/etserver/etserver.ifpasskey.fifo` is used, resolving `$XDG_RUNTIME_DIR` to `$HOME/.local/share` if the environment variable is not set.
- Once it connects to the server, it sends a `TERMINAL_USER_INFO` packet with [TerminalUserInfo](../proto/ETerminal.proto#L97-L110) containing the **client-id** and **passkey** to register the terminal with the server.  These are registered into the ServerConnection [`clientKeys` map](https://github.com/MisterTea/EternalTerminal/blob/113fb23133eabce3d11681392d75ba4772814b44/src/base/ServerConnection.hpp#L37-L40) awaiting a user connection.
- After etterminal connects to etserver, it outputs the **client-id** and **passkey**, to inform the client in cases where it regenerated them.
- etterminal then waits for a client connect, waiting for a `TERMINAL_INIT` ([TermInit](../proto/ETerminal.proto#L89-L95)) packet.
- After receiving this packet UserTerminalHandler enters the `runUserTerminal` run loop, and proxies input/output until the terminal exits. See the [Terminal Run Loop](#terminal-run-loop).

## Client Connection

```mermaid
sequenceDiagram
    participant et
    participant etserver
    participant etterminal
    
    et->>etserver: ConnectRequest (client id, version, supportsChallenge)
    alt protocol version mismatch
        etserver->>et: ConnectResponse (MISMATCHED_PROTOCOL)
        etserver-->>et: Close connection
    else known legacy client (no supportsChallenge)
        etserver->>et: ConnectResponse (NEW_CLIENT or RETURNING_CLIENT)
    else known client
        Note right of etserver: Match client id with terminal
        etserver->>et: ConnectResponse (fresh authChallenge)
        et->>etserver: ConnectAuth (keyed proof)
        etserver->>et: ConnectResponse (NEW_CLIENT or RETURNING_CLIENT)
    end

    et->>etserver: InitialPayload
    etserver->>et: InitialResponse

    Note left of et: Connection complete

    et->>etserver: TerminalBuffer input (encrypted)
    etserver-->etterminal: terminal in/out
    etserver->>et: TerminalBuffer output (encrypted)
```

After the terminal launches, **et** connects to the **etserver** over the EternalTerminal port (defaults to 2022), and sends a [ConnectRequest](../proto/ET.proto#L12-L19) message containing the **client-id** and protocol version.  Since encryption is client-specific, this client-id is sent unencrypted.

The server answers a version mismatch with `MISMATCHED_PROTOCOL` and closes the socket, and an unknown **client-id** with `INVALID_KEY` (or `RETRY_LATER` during the recovery grace period after an etserver restart). For a registered client that sets `supportsChallenge`, the server first sends a [ConnectResponse](../proto/ET.proto#L29-L41) carrying a fresh `authChallenge`. The client answers with [ConnectAuth](../proto/ET.proto#L43-L46), a keyed proof over the client id, protocol version, and challenge. If the proof checks out, the server sends the final `NEW_CLIENT` or `RETURNING_CLIENT` response and creates or resumes the ServerClientConnection, which holds the BackedReader and BackedWriter used for EternalTCP buffering. A bad proof gets `INVALID_KEY`.

The final response carries `resetProof`, a keyed proof over the challenge, `status`, `resetRequired`, and `resetSalt`, so it cannot be replayed into another handshake.

### Legacy handshake

The challenge is a capability within protocol 6, not a version bump. A request without `supportsChallenge` gets the original single `ConnectResponse`, with no challenge, reset fields, or `RETRY_LATER`. A client that gets no `authChallenge` treats the first response as final; if it asked to reattach (`resetIntent`) and gets `RETURNING_CLIENT`, it fails with "Server does not support session reattach; upgrade etserver". The legacy path goes away at the next `PROTOCOL_VERSION` bump.

The client then sends an `INITIAL_PAYLOAD` (with an [InitialPayload](../proto/ETerminal.proto#L71-L78)), which contains port forwarding information or the jumphost flag, to which the server responds with an `INITIAL_RESPONSE` ([InitialResponse](../proto/ETerminal.proto#L80-L82)).  If there's an error during connect, the InitialResponse will contain an error string.

## Reconnection

```mermaid
sequenceDiagram
    participant et
    participant etserver
    participant etterminal
    
    et->>etserver: ConnectRequest (client id, version)
    etserver->>et: ConnectResponse (fresh authChallenge)
    et->>etserver: ConnectAuth (keyed proof)
    etserver->>et: ConnectResponse (RETURNING_CLIENT)

    et->>etserver: SequenceHeader
    etserver->>et: SequenceHeader

    et->>etserver: CatchupBuffer (w/ encrypted packets)
    etserver->>et: CatchupBuffer (w/ encrypted packets)
```

One of the core features of EternalTerminal is handling reconnections, in a way that is seamless to the user: If the previous connection gets interrupted, a new connection is established and continues where the previous connection left off.

When a client disconnects, the etterminal process continues running, and the client id remains registered with etserver. If `etserver` was started with `--disconnect-timeout MINUTES` (or `disconnect_timeout` in `et.cfg`), a terminal that stays disconnected for that long is closed. `0`, the default, leaves the session up. A client may also set `InitialPayload.disconnect_timeout_seconds` via `et --disconnect-timeout MINUTES` (converted to seconds on the wire); when present, that value overrides the etserver global for that session.

To enable reconnects, **et** opens a new connection to the EternalTerminal port, and sends a new [ConnectRequest](../proto/ET.proto#L12-L19) message containing the same **client-id** and protocol version as the initial request, and repeats the challenge exchange.

Upon reconnect, if the server identifies the authenticated client and the ServerClientConnection already exists, it sends a ConnectResponse with status `RETURNING_CLIENT`, and then bidirectional SequenceHeader protobufs are exchanged which contain the last received **sequence number** for each side.

Based on this, a CatchupBuffer protobufs are swapped, containing the missing encrypted packets based on the **sequence number**.

### Reset recovery

A fresh client process (`--attach`, or any initial connect) sets `ConnectRequest.resetIntent`, since it has no sequence history. When one side has lost its history, the final `ConnectResponse` sets `resetRequired` with a fresh `resetSalt`. Both peers echo the salt in their [SequenceHeader](../proto/ET.proto#L48-L56), exchange empty catchup buffers, and start over at sequence zero under a key derived from the salt. A `reset` bit that the authenticated handshake did not select is rejected.

After an etserver restart, the surviving etterminal re-registers with `TerminalUserInfo.ptyactive` set, and the server resumes its shell instead of bootstrapping a new one. Until then clients get `RETRY_LATER`.

### Ending a session

`et --kill` sends `TerminalInfo.command = KILL_SESSION`; the server answers with a `KEEP_ALIVE` carrying `ET_SESSION_KILLED_V1` once the terminal exits.

## Port Forwarding

Port forwarding is supported in Eternal Terminal using the same connection that transmits the terminal updates.  Both forward (server port exposed on client) and reverse forwarding (client port exposed on server) are supported.

### Forward Port Forwarding

![Simple Connection with Port Forwarding](images/port_forwarding.png)

Forward port forwarding listens to a port on the client, and forwards connections to it to the server, which "tunnels" the connection to the server's port. It is activated by passing `--tunnel` or OpenSSH-style `-L` to `et`, and providing a source and destination port or range.

The port range is in the form of `source:destination` or `srcStart-srcEnd:dstStart-dstEnd` (inclusive), where `source` is the port on the client, and `destination` is the port on the server. These forms connect to loopback on the server. Multiple two-part ports or ranges may be forwarded by specifying a comma-separated list.

An SSH-style argument in the form `bind_address:source:destination_host:destination` connects to an explicit host from the server. IPv6 addresses in this form must be enclosed in square brackets. SSH-style arguments cannot be combined in a comma-separated list.

| Command | Description |
| ------- | ----------- |
| `et --tunnel 8080:8080 user@myhost` | Forwards connections to port 8080 on the client to 8080 on the server. |
| `et -L 2222:localhost:22 user@myhost` | Forwards connections to port 2222 on the client to port 22 on the server. |
| `et --tunnel 127.0.0.1:2222:destination.example.com:22 user@gateway` | Listens on `127.0.0.1:2222` on the client and forwards through `gateway` to `destination.example.com:22`. |
| `et --tunnel 8080:8080,2222:22 user@myhost` | Forwards connections to both 8080 and 2222 on the client to port 8080 and 22 on the server (respectively). |
| `et --tunnel 8080-8089:8080-8089 user@myhost` | Forwards connections to port 8080-8089 (inclusive) on the client to the server. |

```mermaid
sequenceDiagram
    participant user
    participant et
    participant etserver
    participant destination

    user->>et: Connect to port
    et->>etserver: PortForwardDestinationRequest
    etserver->>destination: Open tcp connection
    etserver->>et: PortForwardDestinationResponse

    loop Transmit
      user->>et: TCP traffic
      et->>etserver: PortForwardData
      etserver->>destination: TCP traffic
    end

    loop Receive
      destination->>etserver: TCP traffic
      etserver->>et: PortForwardData
      et->>user: TCP traffic
    end
```

To establish port forwarding:
- `et` first parses port ranges, translating each of them to a PortForwardSourceRequest.
- A ForwardSourceHandler is created for each request by calling `PortForwardHandler::createSource`.
- The ForwardSourceHandler starts listening on the client port for connections.
- In the TerminalClient run loop, `PortForwardHandler::update` is called, and within this function any pending connections to the client port are accepted.
- When a connection is accepted on the client, it sends a `PORT_FORWARD_DESTINATION_REQUEST` (with a PortForwardDestinationRequest) packet to the server.
- `PortForwardHandler::update` also reads any data on active connections, and returns a vector of `PortForwardData` to send to the server as well.
- Upon receiving the `PORT_FORWARD_DESTINATION_REQUEST`, the server forwards the packet to `PortForwardHandler::handlePacket`, where it calls `createDestination` to open a connection to the destination port on the server.
- The server then returns a `PORT_FORWARD_DESTINATION_RESPONSE` (PortForwardDestinationResponse) containing the **client fd**, **socket id**, or an error message.
- When the client receives this response, it saves the fd to socket id mapping so it can tag packets to the server.
- Once the response has been saved, forwarded data received from `PORT_FORWARD_DATA` (PortForwardData) packets is mapped to the matching socket and forwarded, and outputs read from the client's port are forwarded to the server by generating `PORT_FORWARD_DATA` messages as well.

### Reverse Port Forwarding

Reverse port forwarding is available by providing `-r`, `--reversetunnel`, or OpenSSH-style `-R`, and accepts the same port range parameter as forward tunnels. These are in the form of `source:destination` or `srcStart-srcStart-srcEnd:dstStart-dstEnd` (inclusive), where `source` is the port on the *server*, and `destination` is the port on the `client`.  Multiple ports may be forwarded by specifying a comma-separated list.

It's also possible to forward Unix sockets, by using the syntax of `ENV_VAR_NAME:/var/run/example.sock`, which will create a temporary file on the server and forward it to `/var/run/example.sock` on the client.  It will then set the temporary file path to the provided environment variable, `ENV_VAR_NAME` in this case.

| Command | Description |
| ------- | ----------- |
| `et -r 8080:8080 user@myhost` | Forwards connections to port 8080 on the server to 8080 on the client. |
| `et -R 22:localhost:2222 user@myhost` | Forwards connections to port 22 on the server to port 2222 on the client. |
| `et -r 5037:5037 user@myhost` | Forwards connections to port 5037 (adb) from the server to the client, enabling adb to be used from the server to a locally-connected device. |
| `et -r 5037:5037,8080:8080 user@myhost` | Forwards connections from the server to client on port 5037 (adb) and port 8080. |
| `et -r ENV_VAR_NAME:/var/run/example.sock user@myhost` | Creates a socket in the temp dir on the server, sets its path to `ENV_VAR_NAME`, and forwards connections to `/var/run/example.sock` on the client. |

```mermaid
sequenceDiagram
    participant destination
    participant et
    participant etserver
    participant user as Server-side user

    et->>etserver: Login with reversetunnels in InitialPayload
    Note right of etserver: Listen to ports
    user->>etserver: Connect to port
    etserver->>et: PortForwardDestinationRequest
    et->>destination: Open tcp connection
    et->>etserver: PortForwardDestinationResponse

    loop Transmit and receive
      user-->etserver: TCP traffic
      etserver-->>et: PortForwardData
      et-->destination: TCP traffic
      et-->>etserver: PortForwardData
    end
```

For reverse tunnels, connections are initiated from the server side by:
- `et` parses the port forwarding parameter and builds a list of PortForwardSourceRequest.
- `et` uses this to populate the `reversetunnels` field of the InitialPayload.
- `etserver` creates a ForwardSourceHandler for each port in the request, to start listening to the requested ports on server.
- When `etserver` receives a connection on the port, it sends a `PORT_FORWARD_DESTINATION_REQUEST` (with a PortForwardDestinationRequest) to the client.
- The client responds with `PORT_FORWARD_DESTINATION_RESPONSE` (PortForwardDestinationResponse), essentially mirroring the flow of forward tunnels.
- Data is exchanged in the same way as forward tunnels, by wrapping traffic in `PORT_FORWARD_DATA` (PortForwardData) packets.

## Jumphosts

![Jumphost Architecture](images/jumphost_architecture.png)

**et** may optionally connect to the destination server through a jumphost, enabling it to reach destinations that are not directly accessible.  This is enabled by passing the `--jumphost` parameter or specified in the SSH config files.

When a jumphost is enabled, **et** launches two `etterminal` processes, one on the jumphost and another on the destination.  On the jumphost, `etterminal` is launched with the `--jump` parameter which configures it to launch in jumphost mode.

When the `etterminal` jumphost launches, a UserJumphostHandler is created which connects to **etserver** the same way as a terminal: by sending a UserTerminalInfo packet.

After jumphost `etterminal` connects to the jumphost `etserver` process, it sends a `JUMPHOST_INIT` (with an InitialPayload) packet, instead of the terminal's `TERMINAL_INIT`.  After receiving this, UserJumphostHandler knows the client has connected, and creates a ClientConnection to the destination server.

It then forwards the InitialPayload to the destination server, and waits for an `INITIAL_RESPONSE` (with an InitialResponse). If this response is successful, UserJumphostHandler enters its run loop, which is described in [Jumphost Run Loop](#jumphost-run-loop).

## Terminal Run Loop

The terminal run loop is within [`UserTerminalHandler::runUserTerminal`](https://github.com/MisterTea/EternalTerminal/blob/113fb23133eabce3d11681392d75ba4772814b44/src/terminal/UserTerminalHandler.cpp#L64), within the `etterminal` process, and starts after the `TERMINAL_INIT` (with a TermInit payload) is received.

It proxies between the user terminal fd (`masterFd`) and the router fifo. When terminal output is generated, it is read and forwarded to the router as a length-prefixed `TERMINAL_BUFFER` packet. When the session ends, etterminal reaps the child shell and sends a `TERMINAL_EXIT_STATUS` packet (with a `TerminalExitStatus` payload) before closing the router connection.

From the router fifo, packets may be sent to either forward input to the terminal or configure the terminal state:
- `TERMINAL_BUFFER` (with a TerminalBuffer payload) data is written to the terminal as user input.
- `TERMINAL_INFO` (with a TerminalInfo) is used to adjust the window size of the terminal.
- `TERMINAL_EXIT_STATUS` is forwarded from etterminal through etserver to the client only when `InitialPayload.supports_exit_status` is set, so `et --command` can exit with the remote command status. Clients that leave the field unset (including et-v7.0.0) never see packet type 12.

## Jumphost Run Loop

The jumphost run loop is within [`UserJumphostHandler::run`](https://github.com/MisterTea/EternalTerminal/blob/113fb23133eabce3d11681392d75ba4772814b44/src/terminal/UserJumphostHandler.cpp#L124), and runs within the `etterminal` process on the jumphost, after the connection has been started and the InitialResponse has been received.

In the run-loop, UserJumpHostHandler acts as a proxy between the destination server and the jumphost `etserver`:
- It reads packets from the local `etserver` over the fifo, and forwards them to the destination server.
- It reads reads packets from the destination server, and forwards them to the local `etserver` fifo.
- If the user disconnects from the jumphost, it closes the connection to the destination server.
