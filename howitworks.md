---
layout: page
title: How ET Works
subtitle: A primer on how ET works under the hood
---

### It all starts with SSH

The ways that one can authenticate and begin an secure connection are
numerous and complicated.  In addition, authentication must be handled
delicately to avoid security holes.  At the moment, ET does not handle
authentication, but relies on ssh to make the initial connection (similar to [Mosh](www.mosh.org)).  Once an ssh connection is established, the server creates a password that is good for the duration of the client's session.  The one-time password is sent securely back to the client and is also used to start a new ETServer process on the server.
