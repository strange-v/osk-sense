# Sourced by the native test scripts: sets rfm69_dir to the RFM69 driver
# sources at the commit node and gateway firmware pin, fetched once into a
# cache on the Linux filesystem. RFM69_DIR overrides it with a local checkout.

rfm69_repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
rfm69_pin() {
    sed -n 's|.*github\.com/strange-v/RFM69\.git#\([0-9a-f]\{40\}\).*|\1|p' "$1"
}
rfm69_commit="$(rfm69_pin "${rfm69_repo_dir}/node/platformio.ini")"
if [[ -z "${rfm69_commit}" ||
      "${rfm69_commit}" != "$(rfm69_pin "${rfm69_repo_dir}/gateway/platformio.ini")" ]]; then
    echo "node and gateway platformio.ini must pin the same RFM69 commit" >&2
    exit 1
fi

if [[ -n "${RFM69_DIR:-}" ]]; then
    rfm69_dir="${RFM69_DIR}"
else
    rfm69_dir="${XDG_CACHE_HOME:-${HOME}/.cache}/osk-sense-native/RFM69-${rfm69_commit}"
    if [[ ! -f "${rfm69_dir}/RFM69.cpp" ]]; then
        rm -rf "${rfm69_dir}.part"
        git init -q "${rfm69_dir}.part"
        git -C "${rfm69_dir}.part" fetch -q --depth 1 \
            https://github.com/strange-v/RFM69.git "${rfm69_commit}"
        git -C "${rfm69_dir}.part" checkout -q FETCH_HEAD
        mv "${rfm69_dir}.part" "${rfm69_dir}"
    fi
fi
