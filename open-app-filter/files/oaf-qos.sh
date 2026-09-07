#!/bin/sh

set -u

TC=/sbin/tc
STATE=/tmp/oaf_qos_tc_state
OUR_ROOT_RE='qdisc[[:space:]]+htb[[:space:]]+1:[[:space:]]+root'
SAFE_ROOT_RE='qdisc[[:space:]]+\(fq_codel\|fq\|pfifo_fast\|pfifo\|noqueue\)'

log() {
    logger -t oaf-qos -- "$*" 2>/dev/null || true
}

get_device() {
    local section="$1"
    local output device
    output="$(ubus call network.interface."$section" status 2>/dev/null || true)"
    device="$(printf '%s\n' "$output" | sed -n 's/.*"l3_device"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1)"
    if [ -z "$device" ]; then
        device="$(printf '%s\n' "$output" | sed -n 's/.*"device"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1)"
    fi
    printf '%s' "$device"
}

has_root() {
    "$TC" qdisc show dev "$1" 2>/dev/null | grep -Eq 'root'
}

is_ours() {
    "$TC" qdisc show dev "$1" 2>/dev/null | grep -Eq "$OUR_ROOT_RE"
}

is_safe_default() {
    "$TC" qdisc show dev "$1" 2>/dev/null | grep -Eq "$SAFE_ROOT_RE"
}

remove_ours() {
    local dev="$1"
    if is_ours "$dev"; then
        "$TC" qdisc del dev "$dev" root 2>/dev/null || true
    fi
}

setup_device() {
    local dev="$1"
    local rate_lines="$2"
    local direction="$3"
    local profile class down up rate
    [ -n "$dev" ] || return 0

    if has_root "$dev" && ! is_ours "$dev"; then
        if ! is_safe_default "$dev"; then
            log "refusing to replace non-default qdisc on $dev"
            return 2
        fi
    fi

    remove_ours "$dev"
    if ! "$TC" qdisc add dev "$dev" root handle 1: htb default 4095; then
        log "failed to create HTB root on $dev"
        return 1
    fi
    if ! "$TC" class add dev "$dev" parent 1: classid 1:1 htb rate 100000mbit ceil 100000mbit; then
        log "failed to create OAF root class on $dev"
        remove_ours "$dev"
        return 1
    fi
    if ! "$TC" class add dev "$dev" parent 1:1 classid 1:4095 htb rate 100000mbit ceil 100000mbit; then
        log "failed to create OAF default class on $dev"
        remove_ours "$dev"
        return 1
    fi

    echo "$rate_lines" | while read -r profile down up; do
        [ -n "$profile" ] || continue
        if [ "$direction" = "down" ]; then
            rate="$down"
        else
            rate="$up"
        fi
        [ "${rate:-0}" -gt 0 ] 2>/dev/null || continue
        "$TC" class add dev "$dev" parent 1:1 classid "1:$profile" htb rate "${rate}kbit" ceil "${rate}kbit" burst 64k cburst 64k || exit 1
        "$TC" filter add dev "$dev" parent 1: protocol all pref "$profile" basic match "meta(nfmark mask 0x0fff0000 eq 0x$(printf '%08x' $((profile << 16))))" flowid "1:$profile" || exit 1
    done
    return $?
}

apply() {
    local rules_file="${1:-/tmp/oaf_qos_tc.rules}"
    local rules_data=""
    local lan_dev wan_dev
    [ -f "$rules_file" ] && rules_data="$(cat "$rules_file")"

    if [ -r "$STATE" ] && [ "$(cat "$STATE" 2>/dev/null)" = "${rules_data}" ]; then
        exit 0
    fi

    lan_dev="$(get_device lan)"
    wan_dev="$(get_device wan)"

    if [ -z "$rules_data" ]; then
        remove_ours "$lan_dev"
        remove_ours "$wan_dev"
        printf '%s' "$rules_data" > "$STATE"
        exit 0
    fi

    if [ -z "$lan_dev" ] || [ -z "$wan_dev" ]; then
        log "cannot determine LAN/WAN devices"
        exit 1
    fi

    if ! setup_device "$wan_dev" "$rules_data" up; then
        remove_ours "$wan_dev"
        exit 1
    fi
    if ! setup_device "$lan_dev" "$rules_data" down; then
        remove_ours "$wan_dev"
        remove_ours "$lan_dev"
        exit 1
    fi

    printf '%s' "$rules_data" > "$STATE"
}

case "${1:-}" in
    apply)
        apply "${2:-/tmp/oaf_qos_tc.rules}"
        ;;
    clear)
        remove_ours "$(get_device lan)"
        remove_ours "$(get_device wan)"
        rm -f "$STATE"
        ;;
    *)
        echo "Usage: $0 {apply|clear} [rules-file]" >&2
        exit 2
        ;;
esac
