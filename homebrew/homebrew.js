/* Nubix — websrv Homebrew Launcher entry.

Copyright (C) 2026 Nubix contributors

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation, version 3.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file LICENSE. If not, see
<http://www.gnu.org/licenses/>.

SPDX-License-Identifier: GPL-3.0-only

Install layout (websrv): /data/homebrew/nubix/{eboot.elf, homebrew.js,
sce_sys/icon0.png, assets/}. websrv sets window.workingDir to that folder. */

async function main() {
    const CWD = window.workingDir;
    const ENV = {TMPDIR: '/user/temp'};

    function launch(extraArgs) {
        return {
            path: CWD + '/eboot.elf',
            args: extraArgs,
            cwd: CWD,
            env: ENV
        };
    }

    return {
        mainText: 'Nubix',
        secondaryText: 'Unofficial client for Xbox Cloud Gaming & Remote Play',
        onclick: async () => launch([]),
        options: [{
            text: 'Start',
            onclick: async () => launch([])
        }, {
            text: 'Start (verbose log)',
            onclick: async () => launch(['--verbose'])
        }]
    };
}
