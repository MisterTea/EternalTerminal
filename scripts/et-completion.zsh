#compdef et

_et() {
    local -a simple_flags
    local -a flags

    simple_flags=(
        '-N[Do not run a remote command]'
        '-T[Run the remote command without a pty]'
        '-t[Request a pty]'
        '-c[Cipher spec for the bootstrap ssh]:cipher'
        '-f[Background after the session is up]'
        '-h[Print help]'
        '-p[Remote sshd port]:port'
        '-r[Reverse tunnel]:tunnel'
        '-R[OpenSSH remote forward]:forward'
        '-L[OpenSSH local forward]:forward'
        '-D[Dynamic SOCKS forward]:port'
        '-W[Stdio forward]:hostport'
        '-u[Username]:username'
        '-l[Remote username]:username'
        '-v[Increase log verbosity]'
        '-x[Disable X11 forwarding]'
        '-e[Escape character]:char'
        '-k[Client keepalive duration in seconds]:seconds'
        '-i[Identity file for the bootstrap ssh]:file:_files'
        '-J[Jump host]:host:_hosts'
        '-F[SSH config file]:file:_files'
        '-o[Session option]:option'
        '-G[Print resolved configuration]'
        '-V[Print OpenSSH-compatible version]'
    )
    flags=(
        '--help[Print help]'
        '--version[Print version]'
        '--verbose[Log verbosity; overrides repeatable -v]:level'
        '--command[Run command on connect and exit]:command'
        '--forward-ssh-agent[Forward ssh-agent socket]'
        '--host[Remote host name]:host:_hosts'
        '--jumphost[jumphost between localhost and destination]:host:_hosts'
        '--jport[Jumphost machine port]:port'
        '--kill-other-sessions[kill all old sessions belonging to the user]'
        '--logtostdout[Write log to stdout]'
        '--no-terminal[Do not create a terminal]'
        '--tunnel[Tunnel]:tunnel'
        '--reversetunnel[Reverse Tunnel]:tunnel'
        '--port[Remote machine etserver port]:port'
        '--silent[Disable logging]'
        '--serverfifo[communicate to etserver on matching fifo name]:fifo'
        '--ssh-socket[The ssh-agent socket to forward]:socket'
        '--username[Username]:username'
        '--noexit[Used with --command to not exit after command is run]'
        '--jserverfifo[communicate to jumphost on matching fifo name]:fifo'
        '--macserver[Set when connecting to an macOS server]'
        '--keepalive[Client keepalive duration in seconds]:seconds'
        '--logdir[Base directory for log files]:directory:_files -/'
        '--telemetry[Allow et to anonymously send errors]'
        '--terminal-path[Path to etterminal on server side]:path:_files'
        '--ssh-option[Options to pass down to `ssh -o`]:option'
        '--name[Choose a saved session name]:name'
        '--attach[Reattach by saved name or title]:session'
        '--kill[End a saved remote session]:session'
        '--list[List saved sessions]'
        '--no-persist[Do not save session credentials]'
    )

    _arguments -s \
        $simple_flags \
        $flags \
        '*:host:_hosts'
}
