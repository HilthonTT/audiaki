#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Check the shell completions against the option tables they describe.
#
# Completions are the one part of a CLI that nothing else exercises: an option
# added to cli.c works perfectly while its completion quietly does not exist,
# and nobody finds out until they press Tab. This compares the long options
# each parser accepts with the ones each completion file declares, in both
# directions - missing and stale - and is run by `make check` and by CI.
#
# Long options only. The short ones are single letters that no completion file
# needs to look up, and they cannot drift out of a getopt string unnoticed the
# way a long spelling can.

set -eu

cd "$(dirname "$0")/.."

status=0

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

# Real options that are deliberately not offered: alternative spellings kept
# working for anyone who types them, rather than ones to suggest. Absent from a
# completion is fine; present is fine too.
is_alias()
{
    case $1 in
        --visualise | --pre-roll) return 0 ;;
        *) return 1 ;;
    esac
}

# What each parser accepts.
sed -n 's/^ *{"\([a-z0-9-]*\)", *\(no\|required\|optional\)_argument.*/--\1/p' \
    src/cli.c | sort -u > "$work/cli.opts"
# grep rather than sed: a line testing two spellings at once would give sed's
# greedy match only the second one.
grep -oE 'strcmp\(arg, "--[a-z0-9-]+"\)' src/gui/app.c \
    | grep -oE -- '--[a-z0-9-]+' | sort -u > "$work/gui.opts"

for f in "$work/cli.opts" "$work/gui.opts"; do
    if [ ! -s "$f" ]; then
        echo "check-completions: found no options in the parser - has it moved?" >&2
        exit 1
    fi
done

# What each completion declares. bash keeps both commands in one file, because
# the two share their device lookup, so each function is read on its own.
awk '/^_audiaki\(\)/, /^_audiaki_gui\(\)/' completions/audiaki.bash \
    | grep -oE -- '--[a-z0-9-]+' | sort -u > "$work/cli.bash"
awk '/^_audiaki_gui\(\)/, /^complete -F/' completions/audiaki.bash \
    | grep -oE -- '--[a-z0-9-]+' | sort -u > "$work/gui.bash"

# Only the spec block: the device helper runs `audiaki --list`, which is a
# command it calls rather than an option it offers.
awk '/args=\(/, /^  \)/' completions/_audiaki \
    | grep -oE -- '--[a-z0-9-]+' | sort -u > "$work/cli.zsh"
awk '/args=\(/, /^  \)/' completions/_audiaki-gui \
    | grep -oE -- '--[a-z0-9-]+' | sort -u > "$work/gui.zsh"

# fish spells them '-l device' rather than '--device'.
grep -oE -- ' -l [a-z0-9-]+' completions/audiaki.fish \
    | awk '{ print "--" $2 }' | sort -u > "$work/cli.fish"
grep -oE -- ' -l [a-z0-9-]+' completions/audiaki-gui.fish \
    | awk '{ print "--" $2 }' | sort -u > "$work/gui.fish"

report()
{
    echo "check-completions: $1" >&2
    status=1
}

# Every option the parser takes is offered, and everything offered still exists.
for cmd in cli gui; do
    case $cmd in
        cli) parser=src/cli.c; name=audiaki ;;
        *) parser=src/gui/app.c; name=audiaki-gui ;;
    esac

    for shell in bash zsh fish; do
        while read -r opt; do
            is_alias "$opt" && continue
            grep -qx -- "$opt" "$work/$cmd.$shell" ||
                report "$name: $parser has $opt, the $shell completion does not"
        done < "$work/$cmd.opts"

        while read -r opt; do
            grep -qx -- "$opt" "$work/$cmd.opts" ||
                report "$name: the $shell completion offers $opt, $parser does not take it"
        done < "$work/$cmd.$shell"
    done
done

if [ "$status" -eq 0 ]; then
    echo "completions OK"
fi

exit "$status"
