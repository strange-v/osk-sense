#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${script_dir}/.."
output=.pio/native-tests/registry-radio-tests
mkdir -p .pio/native-tests
g++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -Itest/native/registry_stubs -Iinclude -I../shared/RadioProtocol/include \
    test/native/test_registry_radio.cpp src/NodeRegistryStore.cpp src/ReplayBoundStorage.cpp \
    ../shared/RadioProtocol/src/{NodeRegistry,RegistryPersistence,RegistryPairing,RegistryRadio}.cpp \
    ../shared/RadioProtocol/src/{PairingTransaction,RadioCrypto,RadioSecurity,RadioSecurityFrames,GatewayReplay}.cpp \
    -o "${output}"
ASAN_OPTIONS=detect_leaks=0 "${output}"
