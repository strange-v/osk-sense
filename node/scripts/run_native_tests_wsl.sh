#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
node_dir="$(cd "${script_dir}/.." && pwd)"
cd "${node_dir}"

source ../shared/scripts/native_unity.sh

flags=(-std=c++17 -Ilib/NodeCore/include -I../shared/RadioProtocol/include -I"${unity_dir}")
if [[ "${1:-}" == "--sanitize" ]]; then
    flags+=(-Wall -Wextra -Werror -g -fsanitize=address,undefined)
fi

output="/tmp/radiosensors_test_node_storage"
g++ "${flags[@]}" \
    test/test_node_storage/test_main.cpp "${unity_dir}/unity.c" -o "${output}"
"${output}"

output="/tmp/radiosensors_test_security_pairing"
g++ "${flags[@]}" test/test_security_pairing/test_main.cpp \
    ../shared/RadioProtocol/src/RadioCrypto.cpp \
    ../shared/RadioProtocol/src/RadioSecurity.cpp \
    ../shared/RadioProtocol/src/RadioSecurityFrames.cpp \
    "${unity_dir}/unity.c" -o "${output}"
"${output}"
