# Bash completions for et (Eternal Terminal).         -*- shell-script -*-

_et() {
    local cur prev words cword
    _init_completion || return

    case "$prev" in
        --logdir|--serverfifo|--jserverfifo|--terminal-path)
            _filedir -d
            return
            ;;
        --ssh-socket|-i)
            _filedir
            return
            ;;
        --host|--jumphost|-J)
            _known_hosts_real -- "$cur"
            return
            ;;
        --port|-p|--jport|--tunnel|--reversetunnel|-L|-R|-D|-W|--keepalive|-e)
            return
            ;;
        --username|-u|-l)
            COMPREPLY=($(compgen -u -- "$cur"))
            return
            ;;
        --command)
            COMPREPLY=($(compgen -c -- "$cur"))
            return
            ;;
        --ssh-option|-o|-c)
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
            -N -T -D -W -L -R -M -S -O -F -G -V
            -c -f -h -p -r -t -u -v -x -e -k -l -o -i -J
        " -- "$cur"))
        return
    fi

    _known_hosts_real -- "$cur"
}

complete -F _et et
