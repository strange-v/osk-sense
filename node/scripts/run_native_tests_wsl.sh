#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
node_dir="$(cd "${script_dir}/.." && pwd)"
cd "${node_dir}"

source ../shared/scripts/native_unity.sh

flags=(-std=c++17 -Ilib/NodeCore/include -I../shared/RadioProtocol/include -I"${unity_dir}")
if [[ "${1:-}" == "--sanitize" ]]; then
    flags+=(-Wall -Wextra -Werror -g -fsanitize=address,undefined)
    shift
fi

suites=("$@")
for suite in "${suites[@]}"; do
    case "$suite" in
        test_node_storage|test_security_service|test_pairing_entropy|test_security_pairing) ;;
        *) echo "Unknown suite: $suite" >&2; exit 2 ;;
    esac
done
wanted() {
    if (( ${#suites[@]} == 0 )); then return 0; fi
    for suite in "${suites[@]}"; do [[ "$suite" == "$1" ]] && return 0; done
    return 1
}

if wanted test_node_storage; then
output="/tmp/radiosensors_test_node_storage"
g++ "${flags[@]}" \
    test/test_node_storage/test_main.cpp "${unity_dir}/unity.c" -o "${output}"
"${output}"
fi

if wanted test_security_service; then
output="/tmp/radiosensors_test_security_service"
g++ "${flags[@]}" -Itest/test_security_service/fakes -Ilib/Drivers/include \
    -DNODE_RADIO_MAX_POWER_LEVEL=15 -DNODE_RFM69_FREQUENCY=RF69_868MHZ \
    -DNODE_TICK_MS=250 -DNODE_MIN_TRANSMIT_MILLIVOLTS=2000 \
    test/test_security_service/test_main.cpp \
    lib/NodeCore/src/CommissioningService.cpp lib/NodeCore/src/NodeRadio.cpp \
    ../shared/RadioProtocol/src/RadioCrypto.cpp \
    ../shared/RadioProtocol/src/RadioSecurity.cpp \
    ../shared/RadioProtocol/src/RadioSecurityFrames.cpp \
    "${unity_dir}/unity.c" -o "${output}"
"${output}"
fi

if wanted test_pairing_entropy; then
output="/tmp/radiosensors_test_pairing_entropy"
g++ "${flags[@]}" test/test_pairing_entropy/test_main.cpp \
    ../shared/RadioProtocol/src/RadioCrypto.cpp \
    "${unity_dir}/unity.c" -o "${output}"
"${output}"
fi

if wanted test_security_pairing; then
output="/tmp/radiosensors_test_security_pairing"
g++ "${flags[@]}" test/test_security_pairing/test_main.cpp \
    ../shared/RadioProtocol/src/RadioCrypto.cpp \
    ../shared/RadioProtocol/src/RadioSecurity.cpp \
    ../shared/RadioProtocol/src/RadioSecurityFrames.cpp \
    "${unity_dir}/unity.c" -o "${output}"
"${output}"
fi
