#!/usr/bin/env bash
#
# Installs asuna into a prefix, defaulting to ~/.local - no root, no packaging.
#
# The layout is chosen to match how the binary looks for its own data. Both
# searches walk up from the executable (see src/paths.cpp and Dialogue::
# defaultPath), so `<prefix>/bin/asuna` finds `<prefix>/share/asuna/models` and
# `<prefix>/share/asuna/dialogue.zh.json` with nothing configured and no
# environment variables set. That is what makes an autostarted daemon, whose
# working directory is the user's home, find its own model.
#
#   ./install.sh                 build if needed, install to ~/.local
#   ./install.sh --prefix /opt/asuna
#   ./install.sh --link          symlink models/ instead of copying 39 MB
#   ./install.sh --uninstall
#
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
prefix="${HOME}/.local"
link_models=0
uninstall=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix) prefix="${2:?--prefix needs a directory}"; shift 2 ;;
        --link) link_models=1; shift ;;
        --uninstall) uninstall=1; shift ;;
        -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; exit 0 ;;
        *) echo "install.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

bindir="${prefix}/bin"
sharedir="${prefix}/share/asuna"

if [[ $uninstall -eq 1 ]]; then
    if [[ -x "${bindir}/asuna" ]]; then
        "${bindir}/asuna" exit >/dev/null 2>&1 || true
        "${bindir}/asuna" autostart disable >/dev/null 2>&1 || true
    fi
    rm -f  "${bindir}/asuna"
    # -r, because --link leaves a symlink and a plain install leaves 39 MB of
    # outfits; both live under share/asuna and neither is anything but ours.
    rm -rf "${sharedir}"
    echo "asuna: removed ${bindir}/asuna and ${sharedir}"
    exit 0
fi

# --- build ------------------------------------------------------------------
if [[ ! -x "${here}/build/asuna" ]] || [[ -n "$(find "${here}/src" -newer "${here}/build/asuna" -name '*.[ch]pp' -print -quit)" ]]; then
    echo "asuna: building"
    cmake -S "${here}" -B "${here}/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "${here}/build" -j"$(nproc)"
fi

if [[ ! -d "${here}/models" ]]; then
    echo "asuna: no models/ directory - run tools/fetch_models.py --all first" >&2
    exit 1
fi

# --- install ----------------------------------------------------------------
# A running daemon is still holding the old inode, which is harmless but means
# it is still the old build until it is restarted. Noticed here rather than
# left for the user to be puzzled by.
was_running=0
if "${here}/build/asuna" ping >/dev/null 2>&1; then was_running=1; fi

mkdir -p "${bindir}" "${sharedir}"
install -m 755 "${here}/build/asuna" "${bindir}/asuna"
install -m 644 "${here}/data/dialogue.zh.json" "${sharedir}/dialogue.zh.json"
# The extension helper and the prompt window it opens. Installed always and
# started never: they do nothing until [ext] enabled is set and `asuna ext
# start` is run, and the binary looks for the helper here (paths::extHelper),
# which then imports the implementation package beside both wrappers.
install -m 755 "${here}/tools/asuna-ext.py" "${sharedir}/asuna-ext.py"
install -m 755 "${here}/tools/asuna-prompt.py" "${sharedir}/asuna-prompt.py"
rm -rf "${sharedir}/chat"
install -d "${sharedir}/chat"
install -m 644 "${here}"/tools/chat/*.py "${sharedir}/chat/"

rm -rf "${sharedir}/models"
if [[ $link_models -eq 1 ]]; then
    ln -s "${here}/models" "${sharedir}/models"
    echo "asuna: models symlinked from ${here}/models"
else
    cp -r "${here}/models" "${sharedir}/models"
fi

echo "asuna: installed ${bindir}/asuna"
echo "       data in   ${sharedir}"

case ":${PATH}:" in
    *":${bindir}:"*) ;;
    *) echo "       note: ${bindir} is not on your PATH" ;;
esac

if [[ $was_running -eq 1 ]]; then
    echo "       a daemon is running the old build - \`asuna restart\` picks this one up"
fi
echo
echo "Next:  asuna start        and  asuna autostart enable"
echo "       asuna config init  writes a commented file with every setting in it"
