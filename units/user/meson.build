# SPDX-License-Identifier: LGPL-2.1+

units = [
        'basic.target',
        'bluetooth.target',
        'default.target',
        'exit.target',
        'graphical-session-pre.target',
        'graphical-session.target',
        'paths.target',
        'printer.target',
        'shutdown.target',
        'smartcard.target',
        'sockets.target',
        'sound.target',
        'timers.target',
        'systemd-tmpfiles-clean.timer',
]

meson.add_install_script('sh', '-c',
                         mkdir_p.format(userunitdir))
foreach file : units
        meson.add_install_script(
            'sh', '-c', cp_cmd.format(meson.current_source_dir() + '/' + file, userunitdir))
endforeach

in_units = [
        'systemd-exit.service',
        'systemd-tmpfiles-clean.service',
        'systemd-tmpfiles-setup.service',
]

foreach file : in_units
        gen = configure_file(
                input : file + '.in',
                output : file,
                configuration : substs)
        install_data(gen,
                     install_dir : userunitdir)
endforeach
