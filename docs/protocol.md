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

    et->>etserver: ConnectRequest (client id, version)
    Note right of etserver: Match client id with terminal
    etserver->>et: ConnectResponse

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
- Once it connects to the server, it sends a `TERMINAL_USER_INFO` packet with [TerminalUserInfo](https://github.com/MisterTea/EternalTerminal/blob/113fb23133eabce3d11681392d75ba4772814b44/proto/ETerminal.proto#L79-L85) containing the **client-id** and **passkey** to register the terminal with the server.  These are registered into the ServerConnection [`clientKeys` map](https://github.com/MisterTea/EternalTerminal/blob/113fb23133eabce3d11681392d75ba4772814b44/src/base/ServerConnection.hpp#L37-L40) awaiting a user connection.
- After etterminal connects to etserver, it outputs the **client-id** and **passkey**, to inform the client in cases where it regenerated them.
- etterminal then waits for a client connect, waiting for a `TERMINAL_INIT` ([TermInit](https://github.com/MisterTea/EternalTerminal/blob/113fb23133eabce3d11681392d75ba4772814b44/proto/ETerminal.proto#L74-L77)) packet.
- After receiving this packet UserTerminalHandler enters the `runUserTerminal` run loop, and proxies input/output until the terminal exits. See the [Terminal Run Loop](#terminal-run-loop).

## Client Connection

After the terminal launches, **et** connects to the **etserver** over the EternalTerminal port (defaults to 2022), and sends a [ConnectRequest](https://github.com/MisterTea/EternalTerminal/blob/113fb23133eabce3d11681392d75ba4772814b44/proto/ET.proto#L12-L15) message containing the **client-id** and protocol version.  Note that this since encryption is client-specific, this client-id is sent unencrypted.

The **client-id** is looked up in the ServerConnection `clientKeys` map, and if it is found a ServerClientConnection is created, which contains the BackedReader and BackedWriter used for EternalTCP buffering.

A ConnectResponse is returned with a status of either `INVALID_KEY`, `NEW_CLIENT`, or `RETURNING_CLIENT` based on the results of the `clientKeys` lookup.  If there's an error the socket is then closed.

## Reconnection

One of the core features of EternalTerminal is handling reconnections, in a way that is seamless to the user: If the previous connection gets interrupted, a new connection is established and continues where the previous connection left off.

When a client disconnects, the etterminal process continues running, and the client id remains registered with etserver.

To enable reconnects, **et** opens a new connection to the EternalTerminal port, and sends a new [ConnectRequest](https://github.com/MisterTea/EternalTerminal/blob/113fb23133eabce3d11681392d75ba4772814b44/proto/ET.proto#L12-L15) message containing the same **client-id** and protocol version as the initial request.

Upon reconnect, if the server identifies the ServerClientConnection already exists, it sends a ConnectResponse with status `RETURNING_CLIENT` as a response, and then bidirectional SequenceHeader protobufs are exchanged which contain the last received **sequence number** for each side.

Based on this, a CatchupBuffer protobufs are swapped, containing the missing encrypted packets based on the **sequence number**.

## Port Forwarding

Port forwarding is supported in Eternal Terminal using the same connection that transmits the terminal updates.  Both forward (server port exposed on client) and reverse forwarding (client port exposed on server) are supported.

### Forward Port Forwarding

![Simple Connection with Port Forwarding](images/port_forwarding.png)

Forward port forwarding listens to a port on the client, and forwards connections to it to the server, which "tunnels" the connection to the server's port. It is activated by passing either a `-t` (or `--tunnel`) parameter to `et`, and providing a source and destination port or range.

The port range is in the form of `source:destination` or `srcStart-srcStart-srcEnd:dstStart-dstEnd` (inclusive), where `source` is the port on the client, and `destination` is the port on the server.  Multiple ports may be forwarded by specifying a comma-separated list.

| Command | Description |
| ------- | ----------- |
| `et -x -t 8080:8080 user@myhost` | Forwards connections to port 8080 on the client to 8080 on the server. |
| `et -x -t 2222:22 user@myhost` | Forwards connections to port 2222 on the client to port 22 on the server. |
| `et -x -t 8080:8080,2222:22 user@myhost` | Forwards connections to both 8080 and 2022 on the client to port 8080 and 22 on the server (respectively). |
| `et -x -t 8080-8089:8080-8089 user@myhost` | Forwards connections to port 8080-8089 (inclusive) on the client to the server. |

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

Reverse port forwarding is available by providing the `-r` or `--reversetunnel` parameter, and accepts the same port range parameter as forward tunnels. These are in the form of `source:destination` or `srcStart-srcStart-srcEnd:dstStart-dstEnd` (inclusive), where `source` is the port on the *server*, and `destination` is the port on the `client`.  Multiple ports may be forwarded by specifying a comma-separated list.

It's also possible to forward Unix sockets, by using the syntax of `ENV_VAR_NAME:/var/run/example.sock`, which will create a temporary file on the server and forward it to `/var/run/example.sock` on the client.  It will then set the temporary file path to the provided environment variable, `ENV_VAR_NAME` in this case.

| Command | Description |
| ------- | ----------- |
| `et -x -r 8080:8080 user@myhost` | Forwards connections to port 8080 on the server to 8080 on the client. |
| `et -x -r 22:2222 user@myhost` | Forwards connections to port 22 on the server to port 2222 on the client. |
| `et -x -r 5037:5037 user@myhost` | Forwards connections to both 5037 (adb) from the server to the client, enabling adb to be used from the server to a locally-connected device. |
| `et -x -r 5037:5037,8080:8080 user@myhost` | Forwards connections from the server to client on port 5037 (adb) and port 8080. |
| `et -x -r ENV_VAR_NAME:/var/run/example.sock user@myhost` | Creates a socket in the temp dir on the server, sets its path to `ENV_VAR_NAME`, and forwards connections to `/var/run/example.sock` on the client. |

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

It proxies between the user terminal fd (`masterFd`) and the router fifo. When terminal output is generated, it is read and the raw bytes are forwarded to the router fifo.

From the router fifo, packets may be sent to either forward input to the terminal or configure the terminal state:
- `TERMINAL_BUFFER` (with a TerminalBuffer payload) data is written to the terminal as user input.
- `TERMINAL_INFO` (with a TerminalInfo) is used to adjust the window size of the terminal.

## Jumphost Run Loop

The jumphost run loop is within [`UserJumphostHandler::run`](https://github.com/MisterTea/EternalTerminal/blob/113fb23133eabce3d11681392d75ba4772814b44/src/terminal/UserJumphostHandler.cpp#L124), and runs within the `etterminal` process on the jumphost, after the connection has been started and the InitialResponse has been received.

In the run-loop, UserJumpHostHandler acts as a proxy between the destination server and the jumphost `etserver`:
- It reads packets from the local `etserver` over the fifo, and forwards them to the destination server.
- It reads reads packets from the destination server, and forwards them to the local `etserver` fifo.
- If the user disconnects from the jumphost, it closes the connection to the destination server.
