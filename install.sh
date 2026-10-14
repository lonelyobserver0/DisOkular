#!/usr/bin/env bash
# Compila e installa DisOkular.
#
#   ./install.sh                  build Release, installa in ~/.local (senza sudo)
#   ./install.sh --system         installa in /usr/local (con sudo)
#   ./install.sh --set-default    in più, lo rende il programma predefinito per PDF, Markdown, LaTeX
#   ./install.sh --uninstall      rimuove i file installati dall'ultima installazione
#   ./install.sh --help           tutte le opzioni
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

readonly SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

prefix="$HOME/.local"
build_type="Release"
build_dir="$SRC_DIR/build-release"
jobs=""
clean=0
uninstall=0
check_deps=1
use_sudo=0
set_default=0

readonly DESKTOP_ID="disokular.desktop"
readonly MIME_TYPES=(application/pdf text/markdown text/x-markdown text/x-tex)
readonly CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}"
readonly MIMEAPPS="$CONFIG_DIR/mimeapps.list"
# Le associazioni sostituite da --set-default, per ripristinarle alla disinstallazione.
readonly PREVIOUS_DEFAULTS="$CONFIG_DIR/disokular/previous-defaults"

# ── Output ────────────────────────────────────────────────────────────────────

if [[ -t 1 ]]; then
    bold=$'\e[1m'; red=$'\e[31m'; yellow=$'\e[33m'; green=$'\e[32m'; reset=$'\e[0m'
else
    bold=""; red=""; yellow=""; green=""; reset=""
fi

step() { printf '%s==>%s %s\n' "$bold$green" "$reset" "$*"; }
warn() { printf '%sattenzione:%s %s\n' "$bold$yellow" "$reset" "$*" >&2; }
die()  { printf '%serrore:%s %s\n' "$bold$red" "$reset" "$*" >&2; exit 1; }

usage() {
    cat <<EOF
Uso: ${0##*/} [opzioni]

Compila DisOkular e lo installa.

Opzioni:
  -p, --prefix DIR      cartella di installazione (predefinita: ~/.local)
  -s, --system          installa in /usr/local, usando sudo per la copia
  -t, --build-type TIPO Release, Debug, RelWithDebInfo o MinSizeRel (predefinito: Release)
  -b, --build-dir DIR   cartella di build (predefinita: build-release)
  -j, --jobs N          compilazioni in parallelo (predefinito: tutti i core)
  -c, --clean           cancella la cartella di build prima di compilare
  -d, --set-default     rendi DisOkular il programma predefinito per PDF, Markdown e LaTeX
                        (solo per il tuo utente, in ~/.config/mimeapps.list)
  -u, --uninstall       rimuove i file installati (usa la stessa cartella di build)
                        e le associazioni create da --set-default
      --no-deps-check   salta il controllo delle dipendenze
  -h, --help            mostra questo aiuto

Esempi:
  ${0##*/}                       # ~/.local/bin/disokular
  ${0##*/} --system              # /usr/local/bin/disokular
  ${0##*/} --clean -t Debug -p /tmp/disokular
  ${0##*/} --set-default         # installa e apri PDF/MD/TEX con DisOkular
  ${0##*/} --uninstall
EOF
}

# ── Argomenti ─────────────────────────────────────────────────────────────────

# Un'opzione con valore non deve restare senza: "--prefix" in fondo è un errore.
need_value() {
    [[ $# -ge 2 && -n "$2" && "$2" != -* ]] || die "l'opzione $1 richiede un valore (vedi --help)"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--prefix)     need_value "$@"; prefix="$2"; shift 2 ;;
        --prefix=*)      prefix="${1#*=}"; shift ;;
        -s|--system)     prefix="/usr/local"; use_sudo=1; shift ;;
        -t|--build-type) need_value "$@"; build_type="$2"; shift 2 ;;
        --build-type=*)  build_type="${1#*=}"; shift ;;
        -b|--build-dir)  need_value "$@"; build_dir="$2"; shift 2 ;;
        --build-dir=*)   build_dir="${1#*=}"; shift ;;
        -j|--jobs)       need_value "$@"; jobs="$2"; shift 2 ;;
        --jobs=*)        jobs="${1#*=}"; shift ;;
        -c|--clean)      clean=1; shift ;;
        -u|--uninstall)  uninstall=1; shift ;;
        -d|--set-default) set_default=1; shift ;;
        --no-deps-check) check_deps=0; shift ;;
        -h|--help)       usage; exit 0 ;;
        *)               die "opzione sconosciuta: $1 (vedi --help)" ;;
    esac
done

case "$build_type" in
    Release|Debug|RelWithDebInfo|MinSizeRel) ;;
    *) die "tipo di build non valido: $build_type" ;;
esac
if [[ -n "$jobs" && ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
    die "--jobs vuole un numero positivo, non '$jobs'"
fi
[[ -n "$jobs" ]] || jobs="$(nproc 2>/dev/null || echo 4)"

# Percorsi assoluti: CMake li registra nella cache e nel manifest.
[[ "$prefix" = /* ]] || prefix="$PWD/$prefix"
[[ "$build_dir" = /* ]] || build_dir="$PWD/$build_dir"

# Fuori dalla home serve sudo, a meno di avere già i permessi di scrittura.
if [[ $use_sudo -eq 0 && "$prefix" != "$HOME"* ]]; then
    parent="$prefix"
    while [[ ! -e "$parent" ]]; do parent="$(dirname "$parent")"; done
    [[ -w "$parent" ]] || use_sudo=1
fi
as_installer() {
    if [[ $use_sudo -eq 1 && $EUID -ne 0 ]]; then sudo "$@"; else "$@"; fi
}

# Cache del launcher e delle icone: senza aggiornarle il nuovo .desktop può non comparire.
refresh_desktop_caches() {
    local apps="$prefix/share/applications" icons="$prefix/share/icons/hicolor"
    if command -v update-desktop-database >/dev/null && [[ -d "$apps" ]]; then
        as_installer update-desktop-database -q "$apps" || true
    fi
    # Solo se il tema hicolor ha già un indice: crearne uno parziale nasconderebbe le altre icone.
    if command -v gtk-update-icon-cache >/dev/null && [[ -f "$icons/index.theme" ]]; then
        as_installer gtk-update-icon-cache -q -t -f "$icons" || true
    fi
}

# ── Disinstallazione ──────────────────────────────────────────────────────────

if [[ $uninstall -eq 1 ]]; then
    # Il prefisso vero è quello dell'installazione, non quello di questa invocazione.
    if [[ -f "$build_dir/CMakeCache.txt" ]]; then
        cached="$(sed -n 's/^CMAKE_INSTALL_PREFIX:PATH=//p' "$build_dir/CMakeCache.txt")"
        [[ -n "$cached" ]] && prefix="$cached"
    fi
    manifest="$build_dir/install_manifest.txt"
    [[ -f "$manifest" ]] || die "nessun manifest in $manifest: niente da disinstallare con questa cartella di build"
    step "Rimozione dei file elencati in $manifest"
    # CMake non chiude il manifest con un a capo: senza "|| -n" l'ultima riga andrebbe persa.
    while IFS= read -r file || [[ -n "$file" ]]; do
        [[ -n "$file" ]] || continue
        if [[ -e "$file" || -L "$file" ]]; then
            [[ -w "$(dirname "$file")" ]] || use_sudo=1
            as_installer rm -f -- "$file"
            echo "  rimosso $file"
        else
            echo "  già assente $file"
        fi
    done < "$manifest"
    # Dopo un'installazione con sudo il manifest appartiene a root.
    [[ -O "$manifest" ]] || use_sudo=1
    as_installer rm -f -- "$manifest"
    refresh_desktop_caches

    # Solo le righe "tipo=disokular.desktop" scritte da --set-default; il resto non si tocca.
    if [[ -f "$MIMEAPPS" ]] && grep -qE "^[^=]+=${DESKTOP_ID//./\\.}\$" "$MIMEAPPS"; then
        step "Rimozione delle associazioni predefinite da $MIMEAPPS"
        grep -E "^[^=]+=${DESKTOP_ID//./\\.}\$" "$MIMEAPPS" | sed 's/^/  /'
        sed -i -E "/^[^=]+=${DESKTOP_ID//./\\.}\$/d" "$MIMEAPPS"
    fi
    # E si rimettono i programmi che c'erano prima.
    if [[ -f "$PREVIOUS_DEFAULTS" ]] && command -v xdg-mime >/dev/null; then
        step "Ripristino delle associazioni precedenti"
        while IFS='=' read -r mime value || [[ -n "$mime" ]]; do
            [[ -n "$mime" && -n "$value" ]] || continue
            if grep -q '^\[Default Applications\]' "$MIMEAPPS" 2>/dev/null; then
                # La riga torna identica, alternative comprese ("a.desktop;b.desktop;").
                sed -i "/^\[Default Applications\]/a ${mime}=${value}" "$MIMEAPPS"
            else
                xdg-mime default "${value%%;*}" "$mime"
            fi
            echo "  $mime → $value"
        done < "$PREVIOUS_DEFAULTS"
        rm -f -- "$PREVIOUS_DEFAULTS"
        rmdir --ignore-fail-on-non-empty -- "$(dirname "$PREVIOUS_DEFAULTS")" 2>/dev/null || true
    fi
    step "DisOkular disinstallato"
    exit 0
fi

# ── Dipendenze ────────────────────────────────────────────────────────────────

if [[ $check_deps -eq 1 ]]; then
    step "Controllo delle dipendenze"
    missing=()
    for tool in cmake pkg-config; do
        command -v "$tool" >/dev/null || missing+=("$tool")
    done
    command -v c++ >/dev/null || command -v g++ >/dev/null || command -v clang++ >/dev/null \
        || missing+=("compilatore C++ (gcc o clang)")
    if command -v pkg-config >/dev/null; then
        pkg-config --exists md4c-html || missing+=("md4c (libreria md4c-html)")
        pkg-config --exists synctex   || missing+=("libsynctex")
    fi
    if [[ ${#missing[@]} -gt 0 ]]; then
        printf '  mancante: %s\n' "${missing[@]}" >&2
        die "installa le dipendenze mancanti (su Arch: sudo pacman -S --needed cmake ninja gcc pkgconf qt6-base qt6-webengine md4c libsynctex)"
    fi
    # Qt 6 (Widgets, Pdf, WebEngine) lo verifica CMake, con un messaggio più preciso.

    # Servono solo per aprire documenti LaTeX: si avvisa, non si blocca.
    for tool in pdflatex bibtex; do
        command -v "$tool" >/dev/null || warn "$tool non trovato: i documenti LaTeX non si potranno compilare"
    done
    command -v biber >/dev/null || echo "  (biber non installato: servirà solo per i documenti biblatex)"
fi

# ── Build ─────────────────────────────────────────────────────────────────────

if [[ $clean -eq 1 && -d "$build_dir" ]]; then
    step "Pulizia di $build_dir"
    rm -rf -- "$build_dir"
fi

generator=()
# Il generatore non si può cambiare su una cartella già configurata: lo si sceglie solo la prima volta.
if [[ ! -f "$build_dir/CMakeCache.txt" ]] && command -v ninja >/dev/null; then
    generator=(-G Ninja)
fi

step "Configurazione ($build_type, prefisso $prefix)"
cmake -S "$SRC_DIR" -B "$build_dir" "${generator[@]}" \
    -DCMAKE_BUILD_TYPE="$build_type" \
    -DCMAKE_INSTALL_PREFIX="$prefix"

step "Compilazione con $jobs job"
cmake --build "$build_dir" --parallel "$jobs"

# ── Installazione ─────────────────────────────────────────────────────────────

if [[ $use_sudo -eq 1 && $EUID -ne 0 ]]; then
    step "Installazione in $prefix (con sudo)"
else
    step "Installazione in $prefix"
fi
as_installer cmake --install "$build_dir"

refresh_desktop_caches

if [[ $set_default -eq 1 ]]; then
    if command -v xdg-mime >/dev/null; then
        step "DisOkular diventa il programma predefinito per: ${MIME_TYPES[*]}"
        # Si annotano i predefiniti attuali, una volta sola: rilanciare --set-default
        # non deve sovrascrivere il ricordo di com'era prima di DisOkular.
        if [[ ! -f "$PREVIOUS_DEFAULTS" && -f "$MIMEAPPS" ]]; then
            mkdir -p -- "$(dirname "$PREVIOUS_DEFAULTS")"
            for mime in "${MIME_TYPES[@]}"; do
                # Solo la sezione [Default Applications]; il valore intero, con le alternative.
                value="$(sed -n '/^\[Default Applications\]/,/^\[/{s|^'"$mime"'=\(.*\)$|\1|p}' "$MIMEAPPS" | head -n1)"
                [[ -n "$value" && "$value" != "$DESKTOP_ID" ]] && echo "$mime=$value"
            done > "$PREVIOUS_DEFAULTS"
            [[ -s "$PREVIOUS_DEFAULTS" ]] || rm -f -- "$PREVIOUS_DEFAULTS"
        fi
        # Le associazioni sono per utente: niente sudo, anche con --system.
        xdg-mime default "$DESKTOP_ID" "${MIME_TYPES[@]}"
    else
        warn "xdg-mime non trovato (pacchetto xdg-utils): associazioni non impostate"
    fi
fi

bin_dir="$prefix/bin"
step "Fatto: $bin_dir/disokular"
case ":$PATH:" in
    *":$bin_dir:"*) echo "  Avvialo con: disokular [file...]" ;;
    *) warn "$bin_dir non è nel PATH: aggiungilo oppure avvia $bin_dir/disokular" ;;
esac
uninstall_cmd="$0 --uninstall"
[[ "$build_dir" == "$SRC_DIR/build-release" ]] || uninstall_cmd+=" --build-dir $build_dir"
echo "  Per disinstallare: $uninstall_cmd"
case ":${XDG_DATA_DIRS:-/usr/local/share:/usr/share}:$HOME/.local/share:" in
    *":$prefix/share:"*) ;;
    *) warn "$prefix/share non è tra le cartelle dei dati (XDG_DATA_DIRS): il launcher potrebbe non vedere DisOkular" ;;
esac
