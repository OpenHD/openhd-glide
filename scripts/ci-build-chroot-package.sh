#!/usr/bin/env bash
set -euo pipefail

chmod 1777 /tmp || true
export DEBIAN_FRONTEND=noninteractive
mkdir -p /out/apt-archives/partial
cat >/etc/apt/apt.conf.d/99openhd-ci-cache-dir <<'APT_CONF'
Dir::Cache::archives "/out/apt-archives";
APT::Install-Recommends "false";
APT::Install-Suggests "false";
APT_CONF
apt-get clean

if [ -d /etc/apt/sources.list.d ]; then
  sudo sed -i 's/^#deb /deb /' /etc/apt/sources.list.d/*.list 2>/dev/null || true
fi

target_release="$(cat flavor.txt 2>/dev/null || true)"
radxa_repo_suite="$(cat radxa_repo_suite.txt 2>/dev/null || printf '%s' "${target_release}")"
install_openhd_cloudsmith_key() {
  mkdir -p /etc/apt/trusted.gpg.d /usr/share/keyrings
  key_tmp="$(mktemp)"
  if command -v curl >/dev/null 2>&1; then
    curl -1fsSL "https://dl.cloudsmith.io/public/openhd/release/gpg.8F544FDF656561E4.key" -o "${key_tmp}"
  elif command -v wget >/dev/null 2>&1; then
    wget -qO "${key_tmp}" "https://dl.cloudsmith.io/public/openhd/release/gpg.8F544FDF656561E4.key"
  else
    echo "curl or wget is required to import the OpenHD Cloudsmith key" >&2
    return 1
  fi
  if command -v gpg >/dev/null 2>&1; then
    gpg --dearmor <"${key_tmp}" >/usr/share/keyrings/openhd-release-archive-keyring.gpg
    cp /usr/share/keyrings/openhd-release-archive-keyring.gpg /etc/apt/trusted.gpg.d/openhd-release.gpg
  else
    cp "${key_tmp}" /usr/share/keyrings/openhd-release-archive-keyring.asc
    cp "${key_tmp}" /etc/apt/trusted.gpg.d/openhd-release.asc
  fi
  rm -f "${key_tmp}"
  chmod 644 /usr/share/keyrings/openhd-release-archive-keyring.* 2>/dev/null || true
  chmod 644 /etc/apt/trusted.gpg.d/openhd-release.* 2>/dev/null || true
}

install_radxa_archive_keyring() {
  mkdir -p /etc/apt/trusted.gpg.d /usr/share/keyrings
  keyring_tmp="$(mktemp)"
  version_tmp="$(mktemp)"
  if command -v curl >/dev/null 2>&1; then
    curl -1fsSL "https://github.com/radxa-pkg/radxa-archive-keyring/releases/latest/download/VERSION" -o "${version_tmp}"
    version="$(cat "${version_tmp}")"
    curl -1fsSL "https://github.com/radxa-pkg/radxa-archive-keyring/releases/latest/download/radxa-archive-keyring_${version}_all.deb" -o "${keyring_tmp}"
  elif command -v wget >/dev/null 2>&1; then
    wget -qO "${version_tmp}" "https://github.com/radxa-pkg/radxa-archive-keyring/releases/latest/download/VERSION"
    version="$(cat "${version_tmp}")"
    wget -qO "${keyring_tmp}" "https://github.com/radxa-pkg/radxa-archive-keyring/releases/latest/download/radxa-archive-keyring_${version}_all.deb"
  else
    echo "curl or wget is required to install the Radxa archive keyring" >&2
    return 1
  fi
  dpkg -i "${keyring_tmp}"
  for keyring in /usr/share/keyrings/radxa-archive-keyring*.gpg; do
    [ -f "${keyring}" ] && cp "${keyring}" "/etc/apt/trusted.gpg.d/$(basename "${keyring}")"
  done
  chmod 644 /etc/apt/trusted.gpg.d/radxa-archive-keyring*.gpg 2>/dev/null || true
  rm -f "${keyring_tmp}" "${version_tmp}"
}

install_radxa_archive_keyring

if [ "${target_release}" = "bookworm" ]; then
  for source_file in /etc/apt/sources.list /etc/apt/sources.list.d/*.list; do
    if [ -f "${source_file}" ] && grep -q 'radxa-repo.github.io/bullseye' "${source_file}"; then
      sudo sed -i -E \
        -e "s|https://radxa-repo.github.io/bullseye/?|https://radxa-repo.github.io/${radxa_repo_suite}/|g" \
        -e "s|http://radxa-repo.github.io/bullseye/?|https://radxa-repo.github.io/${radxa_repo_suite}/|g" \
        -e "s|rockchip-bullseye|${radxa_repo_suite}|g" \
        -e "s| bullseye | ${radxa_repo_suite} |g" \
        "${source_file}"
    fi
    if [ -f "${source_file}" ] && grep -q 'radxa-repo.github.io/.*bookworm' "${source_file}"; then
      sudo sed -i -E \
        -e 's|\[signed-by=[^]]+\]|[signed-by=/usr/share/keyrings/radxa-archive-keyring.gpg]|g' \
        -e '\|radxa-repo.github.io/.*bookworm|{ /\[signed-by=/! s|^deb |deb [signed-by=/usr/share/keyrings/radxa-archive-keyring.gpg] | }' \
        "${source_file}"
    fi
  done
  for source_file in /etc/apt/sources.list.d/*.sources; do
    if [ -f "${source_file}" ] && grep -q 'radxa-repo.github.io/bullseye' "${source_file}"; then
      sudo sed -i -E \
        -e "s|https://radxa-repo.github.io/bullseye/?|https://radxa-repo.github.io/${radxa_repo_suite}/|g" \
        -e "s|http://radxa-repo.github.io/bullseye/?|https://radxa-repo.github.io/${radxa_repo_suite}/|g" \
        -e "s|rockchip-bullseye|${radxa_repo_suite}|g" \
        -e "s|Suites: bullseye|Suites: ${radxa_repo_suite}|g" \
        "${source_file}"
    fi
    if [ -f "${source_file}" ] && grep -q 'radxa-repo.github.io/.*bookworm' "${source_file}"; then
      sudo sed -i -E \
        -e 's|^Signed-By:.*$|Signed-By: /usr/share/keyrings/radxa-archive-keyring.gpg|g' \
        "${source_file}"
      if ! grep -q '^Signed-By:' "${source_file}"; then
        printf '%s\n' 'Signed-By: /usr/share/keyrings/radxa-archive-keyring.gpg' | sudo tee -a "${source_file}" >/dev/null
      fi
    fi
  done
  sudo tee /etc/apt/sources.list.d/70-radxa-${radxa_repo_suite}.list >/dev/null <<APT_SOURCES
deb [signed-by=/usr/share/keyrings/radxa-archive-keyring.gpg] https://radxa-repo.github.io/${radxa_repo_suite}/ ${radxa_repo_suite} main
APT_SOURCES
elif [ "${target_release}" = "bullseye" ]; then
  for source_file in /etc/apt/sources.list /etc/apt/sources.list.d/*.list; do
    [ -f "${source_file}" ] && sudo sed -i '\|bullseye-backports|d' "${source_file}"
  done
  for source_file in /etc/apt/sources.list.d/*.sources; do
    [ -f "${source_file}" ] && grep -q 'bullseye-backports' "${source_file}" && sudo rm -f "${source_file}"
  done
  install_openhd_cloudsmith_key
fi

apt-get update --fix-missing
graphics_dev_packages=(
  libdrm-dev
  libgbm-dev
  libgles2-mesa-dev
  libegl1-mesa-dev
)
if apt-cache policy libdrm2 libgbm1 | awk '/Installed:|Candidate:/ && /~bpo/ { found = 1 } END { exit(found ? 0 : 1) }'; then
  apt-get install -y --no-install-recommends -t bookworm-backports "${graphics_dev_packages[@]}"
else
  apt-get install -y --no-install-recommends "${graphics_dev_packages[@]}"
fi

apt-get remove -y gstreamer1.0-plugins-rtp || true

install_exact_runtime_for_dev_package() {
  local dev_package="$1"
  local runtime_package="$2"
  local dev_version
  local runtime_version

  dev_version="$(apt-cache policy "${dev_package}" | awk '/Candidate:/ { print $2; exit }')"
  if [ -z "${dev_version}" ] || [ "${dev_version}" = "(none)" ]; then
    return 0
  fi

  runtime_version="$(
    apt-cache show "${dev_package}=${dev_version}" 2>/dev/null \
      | awk -v runtime="${runtime_package}" '
          $1 == "Depends:" {
            for (i = 2; i <= NF; ++i) {
              if ($i == runtime && $(i + 1) == "(=") {
                gsub(/[),]/, "", $(i + 2))
                print $(i + 2)
                exit
              }
            }
          }'
  )"
  if [ -n "${runtime_version}" ]; then
    apt-get install -y --no-install-recommends --allow-downgrades "${runtime_package}=${runtime_version}"
  fi
}

install_exact_runtime_for_dev_package libgstreamer1.0-dev libgstreamer1.0-0
install_exact_runtime_for_dev_package libgstreamer-plugins-base1.0-dev libgstreamer-plugins-base1.0-0

apt_package_available() {
  local package_name="$1"
  local candidate
  candidate="$(apt-cache policy "${package_name}" | awk '/Candidate:/ { print $2; exit }')"
  [ -n "${candidate}" ] && [ "${candidate}" != "(none)" ]
}

install_radxa_bullseye_rkmpp_debs() {
  local deb_dir="/tmp/radxa-rkmpp-debs"
  local mpp_dev_deb="${deb_dir}/librockchip-mpp-dev_1.5.0-1_arm64.deb"
  rm -rf "${deb_dir}"
  mkdir -p "${deb_dir}"
  download_deb() {
    local url="$1"
    local output="${deb_dir}/${url##*/}"
    if command -v curl >/dev/null 2>&1; then
      curl -1fsSL "${url}" -o "${output}"
    elif command -v wget >/dev/null 2>&1; then
      wget -qO "${output}" "${url}"
    else
      echo "curl or wget is required to download Radxa RKMPP packages" >&2
      return 1
    fi
  }

  download_deb "https://radxa-repo.github.io/bullseye/pool/main/m/mpp/librockchip-mpp1_1.5.0-1_arm64.deb"
  download_deb "https://radxa-repo.github.io/bullseye/pool/main/m/mpp/librockchip-vpu0_1.5.0-1_arm64.deb"
  download_deb "https://radxa-repo.github.io/bullseye/pool/main/m/mpp/librockchip-mpp-dev_1.5.0-1_arm64.deb"
  download_deb "https://radxa-repo.github.io/bullseye/pool/main/libr/librga/librga2_2.2.0-1_arm64.deb"
  download_deb "https://radxa-repo.github.io/bullseye/pool/main/libr/librga/librga-dev_2.2.0-1_arm64.deb"
  download_deb "https://radxa-repo.github.io/bullseye/pool/main/g/gstreamer1.0-rockchip/gstreamer1.0-rockchip1_1.14-4_arm64.deb"
  if [ -e /usr/include/rockchip/rk_mpi.h ]; then
    rm -f "${mpp_dev_deb}"
  elif dpkg -S /usr/lib/aarch64-linux-gnu/pkgconfig/rockchip_mpp.pc >/dev/null 2>&1; then
    rm -rf /tmp/radxa-mpp-dev-extract
    dpkg-deb -x "${mpp_dev_deb}" /tmp/radxa-mpp-dev-extract
    mkdir -p /usr/include/rockchip
    cp -a /tmp/radxa-mpp-dev-extract/usr/include/rockchip/. /usr/include/rockchip/
    rm -f "${mpp_dev_deb}"
  fi
  apt-get install -y --no-install-recommends "${deb_dir}"/*.deb
  if [ ! -e /usr/include/rockchip/rk_mpi.h ]; then
    echo "[openhd-glide-ci] RKMPP header is missing after installing Radxa packages: /usr/include/rockchip/rk_mpi.h" >&2
    exit 1
  fi
}

apt-get install -y --no-install-recommends \
  build-essential \
  ca-certificates \
  git \
  python3-pip \
  pkg-config \
  zlib1g-dev \
  libfreetype-dev \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly \
  gstreamer1.0-libav

require_rkmpp="$(cat require_rkmpp.txt)"
if [ "${require_rkmpp}" = "1" ]; then
  if [ "${target_release}" = "bullseye" ]; then
    install_radxa_bullseye_rkmpp_debs
  else
    for package_name in librockchip-mpp-dev librga-dev gstreamer1.0-rockchip1; do
      if ! apt_package_available "${package_name}"; then
        echo "[openhd-glide-ci] RKMPP is required, but ${package_name} is unavailable. Check that the Radxa repository is enabled." >&2
        exit 1
      fi
    done
    apt-get install -y --no-install-recommends librockchip-mpp-dev librga-dev gstreamer1.0-rockchip1
  fi
fi

apt-get install -y --no-install-recommends cmake || true
cmake_ge_320() {
  if ! command -v cmake >/dev/null 2>&1; then
    return 1
  fi
  version="$(cmake --version | awk 'NR==1{print $3}')"
  dpkg --compare-versions "$version" ge "3.20"
}
if ! cmake_ge_320; then
  pip3 install --upgrade cmake || pip3 install --upgrade cmake --break-system-packages
fi
cmake --version

package_suffix="$(cat package_suffix.txt)"
package_version="$(cat package_version.txt)"
package_arch="$(cat package_arch.txt)"
extra_debian_depends="$(cat extra_debian_depends.txt)"

rm -rf /usr/share/doc/* /usr/share/man/* /var/cache/man/* /var/lib/apt/lists/*
apt-get clean

build_dir="/mnt/openhd-glide-build/build-package-${package_suffix}"
fetchcontent_dir="/mnt/openhd-glide-build/fetchcontent-${package_suffix}"
mkdir -p /mnt/openhd-glide-build
rm -rf "${build_dir}" "${fetchcontent_dir}"
echo "[openhd-glide-ci] build filesystem:"
df -h / /out /mnt/openhd-glide-build || true
build_jobs="$(nproc)"
if [ "${package_suffix}" = "radxa-cubie" ]; then
  build_jobs=1
fi

cmake_args=(
  -S .
  -B "${build_dir}"
  -DFETCHCONTENT_BASE_DIR="${fetchcontent_dir}"
  -DCMAKE_BUILD_TYPE=Release
  -DOPENHD_GLIDE_DEVICE_KMS=ON
  -DOPENHD_GLIDE_REQUIRE_KMS_GBM=ON
  -DOPENHD_GLIDE_REQUIRE_GSTREAMER=ON
  -DOPENHD_GLIDE_PACKAGE_SUFFIX="-${package_suffix}"
  -DOPENHD_GLIDE_PACKAGE_VERSION="${package_version}"
  -DOPENHD_GLIDE_PACKAGE_ARCHITECTURE="${package_arch}"
  -DOPENHD_GLIDE_EXTRA_DEBIAN_DEPENDS="${extra_debian_depends}"
)
if [ "${require_rkmpp}" = "1" ]; then
  cmake_args+=(-DOPENHD_GLIDE_REQUIRE_RKMPP=ON)
fi
cmake "${cmake_args[@]}"
cmake --build "${build_dir}" -j"${build_jobs}"
cmake --build "${build_dir}" --target package

package_file="$(find "${build_dir}" -maxdepth 1 -name '*.deb' -print -quit)"
dpkg-deb -f "${package_file}" Depends
for required in \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly \
  gstreamer1.0-libav; do
  dpkg-deb -f "${package_file}" Depends | grep -q "${required}"
done
if [ "${require_rkmpp}" = "1" ]; then
  for required in \
    gstreamer1.0-rockchip1 \
    librockchip-mpp1 \
    librga2; do
    dpkg-deb -f "${package_file}" Depends | grep -q "${required}"
  done
fi

rm -rf /out/apt-archives
cp "${build_dir}"/*.deb /out/
rm -rf "${build_dir}"
