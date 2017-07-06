---
layout: page
title: Tutorial
---

# Installing

1. Follow the instructions on the [download page](https://mistertea.github.io/EternalTCP/download) for both your client and server.
2. Verify that the client is installed correctly by looking for the et executable: ```which et```
3. Verify that the server is installed correctly by looking for the etserver executable: ```which etserver```
4. You are ready to start using ET!

# Using

1. ET uses ssh for handshaking and encryption, so you must be able to ssh into the machine from the client.  Make sure that you can ```ssh user@hostname```.
2. ET uses TCP, so you need an open port on your server.  By default, it uses 2022.
3. Once you have an open port, the syntax is similar to ssh: ```et user@hostname[:port]```
4. If you have SSH listening on a different port you can use, ```et -s="-p [SSHport] user@hostname" user@hostname[:ETport]```

# Reporting issues

If you have any problems with installation or usage, please file an issue on [github](https://github.com/MisterTea/EternalTCP/issues/new).
