# Bash completions for et (Eternal Terminal).         -*- shell-script -*-

_et() {
    local cur prev words cword
    _init_completion || return

    case "$prev" in
        --logdir|-l|--serverfifo|--jserverfifo|--terminal-path)
            _filedir -d
            return
            ;;
        --ssh-socket)
            _filedir
            return
            ;;
        --host|--jumphost)
            _known_hosts_real -- "$cur"
            return
            ;;
        --port|--jport|--tunnel|--reversetunnel|--keepalive)
            return
            ;;
        --username|-u)
            COMPREPLY=($(compgen -u -- "$cur"))
            return
            ;;
        --command|-c)
            COMPREPLY=($(compgen -c -- "$cur"))
            return
            ;;
        --ssh-option|-o)
            return
            ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=($(compgen -W "
            --help --version --verbose --command --forward-ssh-agent
            --host --jumphost --jport --kill-other-sessions --logtostdout
            --no-terminal --tunnel --reversetunnel --port --silent
            --serverfifo --ssh-socket --username --noexit --jserverfifo
            --macserver --keepalive --logdir --telemetry --terminal-path
            --ssh-option
            -N -c -f -h -p -r -t -u -v -x -e -k -l -o
        " -- "$cur"))
        return
    fi

    _known_hosts_real -- "$cur"
}

complete -F _et et
