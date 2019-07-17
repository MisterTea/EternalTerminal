If you have set up ET correctly and are having an issue connecting/maintaining a session, please consider running in verbose mode and adding client & server logs to your issue.

To run in verbose mode, pass the --v=9 flag to et.

To collect logs, run the following on your client:

tar -cvzPhf /tmp/etclientLogs.tar.gz /tmp/etclient_err /tmp/etclient.INFO

Then run this on your server:

tar -cvzPhf /tmp/etserverLogs.tar.gz /tmp/etserver_err /tmp/etserver.INFO

The logs will contain the IP addresses & username of the client and server, but will not contain any of the data transmitted.

If you are experiencing a crash, please also post a backtrace.  To do this, replace this line in your et script:

CLIENT_BINARY="etclient"

to this:

CLIENT_BINARY="lldb -- etclient"

then rerun with lldb and when it crashes, type "bt" to give me the stack trace.  If you are running the client under linux, replace with:

CLIENT_BINARY="gdb --args etclient"
