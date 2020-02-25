# Docker container for et-server

## Build

```
$ make
$ docker images |grep et-.*
et-client                latest              54c495fe34dc        11 minutes ago      422MB
et-server                latest              1bf233faf414        11 minutes ago      425MB
```

## Run


```
$ docker run -it --rm -p 2022:2022 -p 2222:22 \
    -v /etc/ssh:/etc/ssh \
    -v /etc/passwd:/etc/passwd \
    -v /etc/shadow:/etc/shadow \
    -v /etc/group:/etc/group \
    -v /home:/home \
    et-server
```

## Notice

- Both ports 2022 and 2222 must be open at the server host (per example above);
- The container starts an sshd server to initiate et-server's handshake.
- You ssh client must be able to connect to container's sshd, not host's sshd;
- Running `ssh -p 2222 user@host` must work out-of-the box;
- Tip: add below to your client's `~/.ssh/config`:

```
Host myhost
  Port 2222
```
