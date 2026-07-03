#!/usr/bin/env bash
set -euo pipefail

required_vars=(
  GITHUB_WORKSPACE
  GLIDE_IMAGE_TYPE
  GLIDE_PACKAGE_SUFFIX
  GLIDE_PACKAGE_ARCH
  GLIDE_CLOUDSMITH_DISTRO
  GLIDE_RELEASE
  GLIDE_REQUIRE_RKMPP
  GLIDE_EXTRA_DEBIAN_DEPENDS
  GITHUB_RUN_NUMBER
)
for var_name in "${required_vars[@]}"; do
  if [ -z "${!var_name:-}" ]; then
    echo "Missing required environment variable: ${var_name}" >&2
    exit 2
  fi
done

git clone https://github.com/OpenHD/OpenHD-ChrootCompiler /opt/OpenHD-ChrootCompiler
mkdir -p /opt/OpenHD-ChrootCompiler/additionalFiles
cp -a "${GITHUB_WORKSPACE}/." /opt/OpenHD-ChrootCompiler/additionalFiles/
printf '%s\n' "${CLOUDSMITH_API_KEY:-}" > /opt/OpenHD-ChrootCompiler/additionalFiles/cloudsmith_api_key.txt
echo "standard" > /opt/OpenHD-ChrootCompiler/additionalFiles/custom.txt
echo "${GLIDE_PACKAGE_ARCH}" > /opt/OpenHD-ChrootCompiler/additionalFiles/arch.txt
echo "${GLIDE_CLOUDSMITH_DISTRO}" > /opt/OpenHD-ChrootCompiler/additionalFiles/distro.txt
echo "${GLIDE_RELEASE}" > /opt/OpenHD-ChrootCompiler/additionalFiles/flavor.txt
echo "${GITHUB_REF_NAME:-}" > /opt/OpenHD-ChrootCompiler/additionalFiles/repo.txt
cp "${GITHUB_WORKSPACE}/scripts/ci-build-chroot-package.sh" /opt/OpenHD-ChrootCompiler/additionalFiles/build_chroot.sh
chmod +x /opt/OpenHD-ChrootCompiler/additionalFiles/build_chroot.sh
echo "${GLIDE_PACKAGE_SUFFIX}" > /opt/OpenHD-ChrootCompiler/additionalFiles/package_suffix.txt
echo "${GLIDE_PACKAGE_ARCH}" > /opt/OpenHD-ChrootCompiler/additionalFiles/package_arch.txt
echo "${GLIDE_REQUIRE_RKMPP}" > /opt/OpenHD-ChrootCompiler/additionalFiles/require_rkmpp.txt
echo "${GLIDE_EXTRA_DEBIAN_DEPENDS}" > /opt/OpenHD-ChrootCompiler/additionalFiles/extra_debian_depends.txt
echo "0.1.0.${GITHUB_RUN_NUMBER}" > /opt/OpenHD-ChrootCompiler/additionalFiles/package_version.txt

cd /opt/OpenHD-ChrootCompiler/
sudo apt update
sudo bash install_dep.sh
sudo apt install -y rclone
python3 "${GITHUB_WORKSPACE}/scripts/ci-patch-chrootcompiler.py"
sudo --preserve-env=OPENHD_RCLONE_CONFIG_GDRIVE bash build.sh "${GLIDE_IMAGE_TYPE}" "${API_KEY:-}" debian "${GLIDE_RELEASE}"
