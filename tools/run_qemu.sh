#!/bin/sh
set -eu

HEADLESS=0
if [ "${1:-}" = "--headless" ]; then
    HEADLESS=1
    shift
fi
IMAGE=${1:-build/JA-os-v5.img}

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "qemu-system-x86_64 not found. Install qemu-system-x86." >&2
    exit 1
fi
if [ ! -f "$IMAGE" ]; then
    echo "disk image not found: $IMAGE" >&2
    exit 1
fi

find_pair() {
    for code in \
        /usr/share/OVMF/OVMF_CODE_4M.fd \
        /usr/share/OVMF/OVMF_CODE.fd \
        /usr/share/edk2/ovmf/OVMF_CODE.fd \
        /usr/share/edk2/x64/OVMF_CODE.fd \
        /usr/share/qemu/OVMF_CODE.fd
    do
        case "$code" in
            *_CODE_4M.fd) vars=${code%_CODE_4M.fd}_VARS_4M.fd ;;
            *_CODE.fd) vars=${code%_CODE.fd}_VARS.fd ;;
            *) continue ;;
        esac
        if [ -f "$code" ] && [ -f "$vars" ]; then
            printf '%s\n%s\n' "$code" "$vars"
            return 0
        fi
    done
    return 1
}

if [ -n "${OVMF_CODE:-}" ] || [ -n "${OVMF_VARS:-}" ]; then
    if [ -z "${OVMF_CODE:-}" ] || [ -z "${OVMF_VARS:-}" ]; then
        echo "Set both OVMF_CODE and OVMF_VARS, or neither." >&2
        exit 1
    fi
    CODE=$OVMF_CODE
    VARS=$OVMF_VARS
else
    pair=$(find_pair || true)
    if [ -z "$pair" ]; then
        echo "A matching OVMF_CODE/OVMF_VARS pair was not found." >&2
        echo "Install ovmf, or set OVMF_CODE and OVMF_VARS explicitly." >&2
        exit 1
    fi
    CODE=$(printf '%s\n' "$pair" | sed -n '1p')
    VARS=$(printf '%s\n' "$pair" | sed -n '2p')
fi

if [ ! -f "$CODE" ] || [ ! -f "$VARS" ]; then
    echo "OVMF firmware pair is invalid: $CODE / $VARS" >&2
    exit 1
fi

mkdir -p build
RUNTIME_VARS=build/OVMF_VARS.runtime.fd
cp "$VARS" "$RUNTIME_VARS"

set -- \
    -machine q35 \
    -m 256M \
    -drive "if=pflash,format=raw,readonly=on,file=$CODE" \
    -drive "if=pflash,format=raw,file=$RUNTIME_VARS" \
    -drive "format=raw,file=$IMAGE" \
    -monitor none \
    -serial stdio

if [ "$HEADLESS" -eq 1 ]; then
    AUDIO_DRIVER=${JCOS_AUDIO_BACKEND:-none}
else
    case "$(uname -s)" in
        MINGW*|MSYS*) AUDIO_DRIVER=${JCOS_AUDIO_BACKEND:-dsound} ;;
        *) AUDIO_DRIVER=${JCOS_AUDIO_BACKEND:-sdl} ;;
    esac
fi
set -- "$@" \
    -audiodev "driver=$AUDIO_DRIVER,id=jcosaudio" \
    -device "AC97,audiodev=jcosaudio"

if [ "$HEADLESS" -eq 1 ]; then
    set -- "$@" -display none
fi

exec qemu-system-x86_64 "$@"
