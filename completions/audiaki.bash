# bash completion for audiaki(1) and audiaki-gui(1)     -*- shell-script -*-
# SPDX-License-Identifier: MIT

# The capture devices audiaki can see, asked of audiaki itself so the answer
# comes from whichever backend is actually running. Bounded by timeout(1) where
# there is one: a sound server that has stopped answering should cost a moment
# at the Tab key, not a shell that never comes back.
_audiaki_devices()
{
    local lister
    lister=$(command -v audiaki 2>/dev/null) || return 0

    if command -v timeout >/dev/null 2>&1; then
        timeout 2 "$lister" --list 2>/dev/null | awk 'NR > 1 { print $1 }'
    else
        "$lister" --list 2>/dev/null | awk 'NR > 1 { print $1 }'
    fi
}

# _filedir comes from bash-completion, which is what loads this file. The
# fallback is for anyone sourcing it by hand.
_audiaki_files()
{
    if declare -F _filedir >/dev/null 2>&1; then
        _filedir "$@"
    else
        mapfile -t COMPREPLY < <(compgen -f -- "$cur")
    fi
}

# Device names carry ':' and '=' - hw:CARD=Box,DEV=0 - and both are word break
# characters to readline, which would otherwise complete only the tail after
# the last one. -n keeps them inside the word; -s handles --device=NAME.
_audiaki_init()
{
    if declare -F _init_completion >/dev/null 2>&1; then
        _init_completion -s -n := || return 1
    else
        split=false
        cur=${COMP_WORDS[COMP_CWORD]}
        prev=${COMP_WORDS[COMP_CWORD - 1]}
    fi
    return 0
}

_audiaki()
{
    local cur prev words cword split
    local opts

    _audiaki_init || return

    case $prev in
        # an output rather than an input, but audiaki only enumerates capture
        # devices; the names overlap, and 'default' is the common answer
        -D | --device | --monitor-device)
            mapfile -t COMPREPLY < <(compgen -W "default $(_audiaki_devices)" -- "$cur")
            return
            ;;
        --backend)
            mapfile -t COMPREPLY < <(compgen -W "auto pipewire alsa" -- "$cur")
            return
            ;;
        -f | --format)
            mapfile -t COMPREPLY < <(compgen -W "s16_le s24_3le s24_le s32_le" -- "$cur")
            return
            ;;
        --style)
            mapfile -t COMPREPLY < <(compgen -W "bars scope waveform" -- "$cur")
            return
            ;;
        --size)
            mapfile -t COMPREPLY < <(compgen -W "480p 720p 1080p 1440p 2160p" -- "$cur")
            return
            ;;
        -r | --rate)
            mapfile -t COMPREPLY < <(compgen -W "44100 48000 88200 96000 192000" -- "$cur")
            return
            ;;
        -c | --channels)
            mapfile -t COMPREPLY < <(compgen -W "1 2 4 6 8" -- "$cur")
            return
            ;;
        --channel)
            mapfile -t COMPREPLY < <(compgen -W "1 2 3 4 5 6 7 8" -- "$cur")
            return
            ;;
        --visualize | --visualise | --info | --play)
            _audiaki_files wav
            return
            ;;
        # a prefix rather than a file, but it can carry a directory
        -o | --output | --take)
            _audiaki_files
            return
            ;;
        # free text, with nothing to guess
        --note)
            return
            ;;
        # numbers, with nothing worth guessing
        -t | --duration | -p | --period | -n | --periods | --fps | --bars | --a4 | \
        --preroll | --pre-roll | --monitor-gain | --click | --click-beats | --click-gain)
            return
            ;;
    esac

    $split && return

    if [[ $cur == -* ]]; then
        opts="--device --backend --rate --channels --channel --format --duration --period
              --periods --output --force --take --preroll --note --no-metadata
              --spectrum --no-meter
              --monitor --monitor-device --monitor-gain
              --click --click-beats --click-gain
              --visualize --style --size --fps --bars --tune --a4 --info --play --json
              --quiet --verbose --list --probe --help --version"
        mapfile -t COMPREPLY < <(compgen -W "$opts" -- "$cur")
        return
    fi

    _audiaki_files wav
}

_audiaki_gui()
{
    local cur prev words cword split
    local opts

    _audiaki_init || return

    case $prev in
        -D | --device)
            mapfile -t COMPREPLY < <(compgen -W "default $(_audiaki_devices)" -- "$cur")
            return
            ;;
        -b | --backend)
            mapfile -t COMPREPLY < <(compgen -W "auto pipewire alsa" -- "$cur")
            return
            ;;
        -s | --style)
            mapfile -t COMPREPLY < <(compgen -W "bars mirror radial scope waterfall tuner" -- "$cur")
            return
            ;;
        --video-size)
            mapfile -t COMPREPLY < <(compgen -W "480p 720p 1080p 1440p 2160p" -- "$cur")
            return
            ;;
        -r | --rate)
            mapfile -t COMPREPLY < <(compgen -W "44100 48000 88200 96000 192000" -- "$cur")
            return
            ;;
        -c | --channels)
            mapfile -t COMPREPLY < <(compgen -W "1 2 4 6 8" -- "$cur")
            return
            ;;
        -o | --take)
            _audiaki_files
            return
            ;;
        --video-fps | --preroll | --pre-roll)
            return
            ;;
    esac

    $split && return

    opts="--device --backend --rate --channels --take --style --preroll --video
          --video-silent --video-size --video-fps --monitor --verbose --help"
    mapfile -t COMPREPLY < <(compgen -W "$opts" -- "$cur")
}

complete -F _audiaki audiaki
complete -F _audiaki_gui audiaki-gui
