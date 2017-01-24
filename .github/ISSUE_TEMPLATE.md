If you have set up ET correctly and are having an issue connecting/maintaining a session, please consider adding client & server logs to your issue.  Run the following on your client:

tar cvzf /tmp/etclientLogs.tar.gz /tmp/et_err /tmp/etclient.INFO

Then run this on your server:

tar cvzf /tmp/etserverLogs.tar.gz /tmp/et_err /tmp/etserver.INFO

The logs will contain the IP addresses & username of the client and server, but will not contain any of the data transmitted.
