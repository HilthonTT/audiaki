# fish completion for audiaki(1)
# SPDX-License-Identifier: MIT

# Asked of audiaki itself, so the names come from whichever backend is running.
# Bounded by timeout(1) where there is one, so a sound server that has stopped
# answering costs a moment at the Tab key rather than the shell.
function __audiaki_devices
    command -q audiaki; or return
    echo default
    if command -q timeout
        timeout 2 audiaki --list 2>/dev/null | awk 'NR > 1 { print $1 }'
    else
        audiaki --list 2>/dev/null | awk 'NR > 1 { print $1 }'
    end
end

# No positional argument is ever a subcommand, so nothing is ever completed as
# one; the take name is a file, and everything else hangs off a flag.
complete -c audiaki -f
complete -c audiaki -k -a '(__fish_complete_suffix .wav)' -d 'take to write'

complete -c audiaki -s D -l device -d 'Capture device' -x -a '(__audiaki_devices)'
complete -c audiaki -l backend -d 'Audio system to talk to' -x -a 'auto pipewire alsa'
complete -c audiaki -s r -l rate -d 'Sample rate in Hz' -x -a '44100 48000 88200 96000 192000'
complete -c audiaki -s c -l channels -d 'How many channels to capture' -x -a '1 2 4 6 8'
complete -c audiaki -l channel -d 'Write only capture channel N, as mono' -x -a '1 2 3 4 5 6 7 8'
complete -c audiaki -s f -l format -d 'Sample format' -x -a 's16_le s24_3le s24_le s32_le'
complete -c audiaki -s t -l duration -d 'Stop after SS, MM:SS or HH:MM:SS' -x
complete -c audiaki -s p -l period -d 'Period size in frames' -x
complete -c audiaki -s n -l periods -d 'Periods per buffer' -x
complete -c audiaki -l take -d 'Write the next free PREFIX-001.wav' -r
complete -c audiaki -l preroll -d 'Hold SECS and wait for Enter before recording' -x
complete -c audiaki -l note -d 'Stamp the take with a note, up to 200 characters' -x
complete -c audiaki -l no-metadata -d 'Write a plain 44-byte header, unstamped'
complete -c audiaki -l spectrum -d 'Show live spectrum bars instead of the peak bar'
complete -c audiaki -l no-meter -d 'Draw nothing while recording'
complete -c audiaki -s M -l monitor -d 'Hear the input while recording it (use headphones)'
complete -c audiaki -l monitor-device -d 'Output to monitor through' -x -a 'default (__audiaki_devices)'
complete -c audiaki -l monitor-gain -d 'Scale what is monitored, 0.0 to 2.0' -x
complete -c audiaki -l click -d 'Play a metronome at BPM while recording (heard, not recorded)' -x
complete -c audiaki -l click-beats -d 'Beats to a bar, accenting the first' -x
complete -c audiaki -l click-gain -d 'How loud the click is, 0.0 to 2.0' -x

complete -c audiaki -l visualize -d 'Render a WAV to a video and exit' -r -a '(__fish_complete_suffix .wav)'
complete -c audiaki -s o -l output -d 'Output file' -r
complete -c audiaki -l style -d 'Visualiser style' -x -a 'bars scope waveform'
complete -c audiaki -l size -d 'Video size' -x -a '480p 720p 1080p 1440p 2160p'
complete -c audiaki -l fps -d 'Video frame rate' -x
complete -c audiaki -l bars -d 'Number of spectrum bars' -x

complete -c audiaki -l tune -d 'Show the pitch of what is being played, then exit'
complete -c audiaki -l a4 -d 'Tuner reference pitch in Hz' -x

complete -c audiaki -l info -d 'Report levels and clipping for one or more WAVs and exit' -r -a '(__fish_complete_suffix .wav)'
complete -c audiaki -l play -d 'Play a WAV through the output and exit' -r -a '(__fish_complete_suffix .wav)'
complete -c audiaki -l json -d 'Machine readable --list, --probe and --info'
complete -c audiaki -s y -l force -d 'Overwrite the output file if it exists'
complete -c audiaki -s q -l quiet -d 'Errors only'
complete -c audiaki -s v -l verbose -d 'Report device negotiation details'
complete -c audiaki -s l -l list -d 'List capture devices and exit'
complete -c audiaki -s P -l probe -d 'Show what a device supports and exit'
complete -c audiaki -s h -l help -d 'Show usage and exit'
complete -c audiaki -s V -l version -d 'Show the version and exit'
