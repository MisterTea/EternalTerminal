If you have set up ET correctly and are having an issue connecting/maintaining a session, please consider running in verbose mode and adding client & server logs to your issue.

To run in verbose mode, pass the -v flag to et.

To collect logs, run the following on your client:

tar -cvzPhf /tmp/etclientLogs.tar.gz /tmp/et_err /tmp/etclient.INFO

Then run this on your server:

tar -cvzPhf /tmp/etserverLogs.tar.gz /tmp/et_err /tmp/etserver.INFO

The logs will contain the IP addresses & username of the client and server, but will not contain any of the data transmitted.
