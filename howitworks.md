---
layout: page
title: How Eternal Terminal Works
subtitle: A primer on how Eternal Terminal (ET) works under the hood
---

# Eternal TCP

Eternal TCP is a layer in between an application and unix TCP sockets that make the sockets robust to TCP disconnects including roaming and connection failure.

## Why resumable TCP is hard

When one calls send(...) or write(...) on a TCP socket, and the kernel
reports back that a set of bytes were written, there's no guarantee
that the bytes were sent nor received on the other end.  The kernel is
merely telling you that the bytes were written to a buffer to be sent.
When the client and server disconnect, neither know which bytes were
received and which were dropped.

## Solution: BackedReader and BackedWriter

Eternal TCP implements a **BackedReader** class that keeps track of
the number of bytes read (called the *sequence number*) and, upon
reconnect, informs the other party of the sequence number.  The
**BackedWriter** class keeps an encrypted buffer of the last *N* bytes
sent and the sequence number.  Upon reconnect, the BackedWriter
receives the sequence number from the BackedReader and re-sends the
last *M* bytes, where M is the difference between the sequence numbers
of the writer and the reader.

Both the client and the server of a TCP connection need both a
BackedReader and BackedWriter so they can send and receive messages.

Eternal TCP is an implementation of resumable TCP that can be used by any application.

# The Eternal Terminal (ET).

![E.T. Communicator](https://upload.wikimedia.org/wikipedia/commons/thumb/4/4c/ET_Communicator_Cropped.jpg/550px-ET_Communicator_Cropped.jpg "E.T. Communicator")

E.T. the Extra-Terrestrial was a movie about an alien and a boy that
together build a device to "phone home" and send the alien back to his
family and homeworld.

Just like the E.T. Communicator, the Eternal Terminal (ET) exists to allow developers to keep in contact with their remote machines.  ET survives IP roaming and other connection loss events.

## It all starts with SSH

The ways that one can authenticate and begin an secure connection are
numerous and complicated.  In addition, authentication must be handled
delicately to avoid security holes.  At the moment, ET does not handle
authentication, but relies on ssh to make the initial connection
(similar to [Mosh](www.mosh.org)).  Once an ssh connection is
established, the server creates a password that is good for the
duration of the client's session.  The one-time password is sent
securely back to the client and is also used to start a new ETServer
process on the server.  After the ETServer process begins on the
server, the ssh connection closes and ssh is no longer needed.

## No SSH Protocol

ET does not implement any of the SSH protocol.  Instead ET simply
creates a psuedo-terminal on the server side and connect this
psuedo-terminal to the client's terminal.  There's no concept of
channels, etc..  While it may be possible to implement the SSH
protocol on top of ET and thus support X-forwarding and other
features, it's not implemented at the moment.

## Works with tmux control center

Tmux control center (activated by running tmux with the -CC flag) is
supported in ET since it does not rely on the ssh protocol and is
effectively its own protocol written in terminal escape sequences.
This means that it's possible to have tabs and split screen support
within ET.  Unfortunately, iTerm2 for OS/X and an unsupported fork of
the Terminator terminal are the only terminals that support Tmux
control center.
