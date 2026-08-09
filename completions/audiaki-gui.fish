# fish completion for audiaki-gui(1)
# SPDX-License-Identifier: MIT

# The window has no --list of its own, so the device names come from the
# recorder next to it. Defined again rather than shared: fish loads one
# completion file per command, and audiaki.fish may never have been read.
function __audiaki_gui_devices
    command -q audiaki; or return
    echo default
    if command -q timeout
        timeout 2 audiaki --list 2>/dev/null | awk 'NR > 1 { print $1 }'
    else
        audiaki --list 2>/dev/null | awk 'NR > 1 { print $1 }'
    end
end

# The window takes no file arguments at all: every value hangs off a flag.
complete -c audiaki-gui -f

complete -c audiaki-gui -s D -l device -d 'Capture device the window opens on' -x -a '(__audiaki_gui_devices)'
complete -c audiaki-gui -s b -l backend -d 'Audio system to talk to' -x -a 'auto pipewire alsa'
complete -c audiaki-gui -s r -l rate -d 'Sample rate in Hz' -x -a '44100 48000 88200 96000 192000'
complete -c audiaki-gui -s c -l channels -d 'Channel count' -x -a '1 2 4 6 8'
complete -c audiaki-gui -s o -l take -d 'Take name prefix' -r
complete -c audiaki-gui -l dir -d 'Folder to keep takes in' -x -a '(__fish_complete_directories)'
complete -c audiaki-gui -l no-dialog -d 'Do not ask where a take should be kept when it stops'
complete -c audiaki-gui -s s -l style -d 'Visualiser style to start on' -x -a 'bars mirror radial scope waterfall tuner'
complete -c audiaki-gui -l preroll -d 'Keep SECS of audio while idle, so takes start earlier' -x
complete -c audiaki-gui -l no-overdub -d 'Do not play the project while recording over it'
complete -c audiaki-gui -l latency -d 'Round-trip latency to place an overdub by' -x

complete -c audiaki-gui -s V -l video -d 'Also render an MP4 of each take'
complete -c audiaki-gui -l video-silent -d 'Render that MP4 without the take\'s audio'
complete -c audiaki-gui -l video-size -d 'Video size' -x -a '480p 720p 1080p 1440p 2160p'
complete -c audiaki-gui -l video-fps -d 'Video frame rate' -x

complete -c audiaki-gui -s M -l monitor -d 'Start with playback monitoring on'
complete -c audiaki-gui -s v -l verbose -d 'Log device negotiation to the terminal'
complete -c audiaki-gui -s h -l help -d 'Show usage and exit'
