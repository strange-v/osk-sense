#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
protocol_dir="$(cd "${script_dir}/.." && pwd)"
cd "${protocol_dir}"

source ../shared/scripts/native_unity.sh

common_sources=(
    ../shared/RadioProtocol/src/CommissioningFrames.cpp
    ../shared/RadioProtocol/src/NodeRegistry.cpp
    ../shared/RadioProtocol/src/RegistryPersistence.cpp
    ../shared/RadioProtocol/src/GatewayStorage.cpp
    ../shared/RadioProtocol/src/UserManagement.cpp
    ../shared/RadioProtocol/src/CommandBook.cpp
    ../shared/RadioProtocol/src/RadioCrypto.cpp
    ../shared/RadioProtocol/src/RadioSecurity.cpp
    ../shared/RadioProtocol/src/RadioSecurityFrames.cpp
    ../shared/RadioProtocol/src/PairingTransaction.cpp
    ../shared/RadioProtocol/src/RegistryPairing.cpp
    ../shared/RadioProtocol/src/RegistryRadio.cpp
    ../shared/RadioProtocol/src/GatewayReplay.cpp
    "${unity_dir}/unity.c"
)
common_flags=(
    -std=c++17
    -I../shared/RadioProtocol/include
    -I"${unity_dir}"
)

run_suite() {
    local suite="$1"
    local output="/tmp/radiosensors_${suite}"
    g++ "${common_flags[@]}" "test/${suite}/test_main.cpp" \
        "${common_sources[@]}" -o "${output}"
    "${output}"
}

if [[ "${1:-}" == "--sanitize" ]]; then
    common_flags+=(-Wall -Wextra -Werror -g -fsanitize=address,undefined)
    shift
fi
if (( $# > 0 )); then
    for suite in "$@"; do
        if [[ ! "${suite}" =~ ^test_[a-z_]+$ || ! -f "test/${suite}/test_main.cpp" ]]; then
            echo "Unknown suite: ${suite}" >&2
            exit 2
        fi
        run_suite "${suite}"
    done
    exit 0
fi

run_suite test_radio_protocol
run_suite test_node_registry
run_suite test_commissioning_frames
run_suite test_gateway_storage
run_suite test_command_book
run_suite test_radio_power
run_suite test_radio_aes
run_suite test_radio_security
run_suite test_gateway_replay
run_suite test_security_frames
run_suite test_pairing_transaction
run_suite test_registry_pairing
run_suite test_registry_radio
