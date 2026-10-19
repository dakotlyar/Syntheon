#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Captures the files SynBench's Linux system-info parsers read into a fixture
# tree, for tests/unit/tst_sysinfo_linux.cpp:
#
#   tests/fixtures/capture-linux.sh tests/fixtures/linux/<machine>
#
# Paths are committed with % : * ? " < > | escaped as %XX (in names and in
# symlink targets), because a Windows checkout can't hold them;
# tests/unit/fixture_tree.h decodes them back.
#
# Runs as a normal user (no root). Only an explicit allowlist is copied, and
# nothing identifying: no serial numbers, UUIDs, MAC addresses, asset tags or
# battery manufacturer/model strings (P23). The SMBIOS tables are root-only
# and full of serials, so they're never captured here.
set -euo pipefail

final=${1:?usage: capture-linux.sh <output dir>}
mkdir -p "$final"
final=$(cd "$final" && pwd)
# Capture into a scratch tree with the real names, then copy it into the
# output with Windows-invalid characters escaped (see encode_tree below).
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

copy() {  # copy <absolute path>, if readable, into the tree
    local src=$1
    [[ -r $src && -f $src ]] || return 0
    mkdir -p "$out$(dirname "$src")"
    cat "$src" > "$out$src" 2>/dev/null || rm -f "$out$src"
}

link() {  # recreate symlink <absolute path> with its (relative) target
    local src=$1
    [[ -L $src ]] || return 0
    mkdir -p "$out$(dirname "$src")"
    ln -sfn "$(readlink "$src")" "$out$src"
}

copy /proc/cpuinfo
copy /proc/meminfo
copy /etc/os-release
for f in sys_vendor product_name product_family product_version bios_vendor bios_version bios_date \
         board_vendor board_name chassis_type; do
    copy /sys/class/dmi/id/$f
done
copy /proc/device-tree/model

for f in online present possible; do copy /sys/devices/system/cpu/$f; done
for f in /sys/devices/cpu_core/cpus /sys/devices/cpu_atom/cpus; do copy $f; done
for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
    for f in topology/core_id topology/physical_package_id topology/die_id topology/cluster_id \
             topology/thread_siblings_list topology/core_cpus_list cpu_capacity acpi_cppc/highest_perf \
             cpufreq/cpuinfo_max_freq cpufreq/cpuinfo_min_freq cpufreq/base_frequency \
             cpufreq/scaling_cur_freq thermal_throttle/core_throttle_count \
             thermal_throttle/package_throttle_count regs/identification/midr_el1; do
        copy "$cpu/$f"
    done
    for idx in "$cpu"/cache/index*; do
        [[ -d $idx ]] || continue
        for f in level type size coherency_line_size shared_cpu_list; do copy "$idx/$f"; done
    done
done

# Temperature sensors and the power profile, for the CPU test's telemetry
# (Phase 2 §8.1): each hwmon's class link, name and temp*_input / temp*_label.
for hw in /sys/class/hwmon/hwmon[0-9]*; do
    link "$hw"
    real=$(readlink -f "$hw")
    copy "$real/name"
    for f in "$real"/temp[0-9]*_input "$real"/temp[0-9]*_label; do copy "$f"; done
done
copy /sys/firmware/acpi/platform_profile
copy /sys/firmware/acpi/platform_profile_choices

# GPUs and NPUs: PCI ids, VRAM size and the driver link.
for dev in /sys/class/drm/card[0-9]* /sys/class/accel/accel[0-9]*; do
    [[ $dev =~ card[0-9]+$ || $dev =~ accel[0-9]+$ ]] || continue
    card=$(readlink -f "$dev")          # .../<pci device>/drm/card1
    real=$(readlink -f "$dev/device")   # .../<pci device>
    link "$dev"
    mkdir -p "$out$card"
    link "$card/device"
    for f in vendor device mem_info_vram_total; do copy "$real/$f"; done
    link "$real/driver"
    driver=$(basename "$(readlink -f "$real/driver")")
    mkdir -p "$out/sys/bus/pci/drivers/$driver"
    copy /sys/module/$driver/version
done

# Power supplies (no serial_number / manufacturer / model_name).
for ps in /sys/class/power_supply/*; do
    for f in type scope online present status capacity energy_full energy_full_design energy_now \
             charge_full charge_full_design charge_now voltage_min_design cycle_count; do
        copy "$ps/$f"
    done
done

# Block devices behind mounted filesystems: model, rotational, partition flag.
for majmin in $(lsblk -rno MAJ:MIN,TYPE | awk '$2=="disk"||$2=="part"{print $1}'); do
    entry=/sys/dev/block/$majmin
    [[ -L $entry ]] || continue
    link "$entry"
    real=$(readlink -f "$entry")
    copy "$real/partition"
    disk=$real
    [[ -f $real/partition ]] && disk=$(dirname "$real")
    for f in device/model queue/rotational; do copy "$disk/$f"; done
done
enc() {  # escape the characters Windows forbids in path components
    local s=$1 r="" c i
    for ((i = 0; i < ${#s}; i++)); do
        c=${s:i:1}
        case $c in
            '%' | ':' | '*' | '?' | '"' | '<' | '>' | '|') r+=$(printf '%%%02X' "'$c") ;;
            *) r+=$c ;;
        esac
    done
    printf '%s' "$r"
}

encode_tree() {  # encode_tree <src> <dst>
    local src=$1 dst=$2 rel encoded
    (cd "$src" && find . -mindepth 1 \( -type d -o -type f -o -type l \) -print) | sort | while IFS= read -r rel; do
        rel=${rel#./}
        encoded=$(enc "$rel")
        if [[ -L $src/$rel ]]; then
            mkdir -p "$(dirname "$dst/$encoded")"
            ln -sfn "$(enc "$(readlink "$src/$rel")")" "$dst/$encoded"
        elif [[ -d $src/$rel ]]; then
            mkdir -p "$dst/$encoded"
        else
            mkdir -p "$(dirname "$dst/$encoded")"
            cp "$src/$rel" "$dst/$encoded"
        fi
    done
}

encode_tree "$out" "$final"
echo "captured into $final"
