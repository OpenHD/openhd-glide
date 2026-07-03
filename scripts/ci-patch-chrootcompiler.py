#!/usr/bin/env python3
import os
import re
from pathlib import Path


def patch_package_stage() -> None:
    packages_stage = Path("/opt/OpenHD-ChrootCompiler/stages/02-Packages/00-run-chroot.sh")
    packages_text = packages_stage.read_text()
    packages_marker = "openhd_glide_normalize_apt_sources"
    packages_needle = 'echo "_______________________Starting build____________________________"\n'
    packages_patch = packages_needle + r'''
openhd_glide_normalize_apt_sources() {
  install_openhd_cloudsmith_key() {
    sudo mkdir -p /etc/apt/trusted.gpg.d /usr/share/keyrings
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
      gpg --dearmor <"${key_tmp}" | sudo tee /usr/share/keyrings/openhd-release-archive-keyring.gpg >/dev/null
      sudo cp /usr/share/keyrings/openhd-release-archive-keyring.gpg /etc/apt/trusted.gpg.d/openhd-release.gpg
    else
      sudo cp "${key_tmp}" /usr/share/keyrings/openhd-release-archive-keyring.asc
      sudo cp "${key_tmp}" /etc/apt/trusted.gpg.d/openhd-release.asc
    fi
    rm -f "${key_tmp}"
    sudo chmod 644 /usr/share/keyrings/openhd-release-archive-keyring.* 2>/dev/null || true
    sudo chmod 644 /etc/apt/trusted.gpg.d/openhd-release.* 2>/dev/null || true
  }

  install_radxa_archive_keyring() {
    sudo mkdir -p /etc/apt/trusted.gpg.d /usr/share/keyrings
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
    sudo dpkg -i "${keyring_tmp}"
    for keyring in /usr/share/keyrings/radxa-archive-keyring*.gpg; do
      [ -f "${keyring}" ] && sudo cp "${keyring}" "/etc/apt/trusted.gpg.d/$(basename "${keyring}")"
    done
    sudo chmod 644 /etc/apt/trusted.gpg.d/radxa-archive-keyring*.gpg 2>/dev/null || true
    rm -f "${keyring_tmp}" "${version_tmp}"
  }

  install_radxa_archive_keyring

  if [[ "${DISTRO}" == "bookworm" ]]; then
    echo "[apt-preflight] Ensuring Bookworm apt sources..."
    sudo tee /etc/apt/sources.list.d/openhd-bookworm.list >/dev/null <<'APT_SOURCES'
deb http://deb.debian.org/debian bookworm main contrib non-free non-free-firmware
deb http://deb.debian.org/debian bookworm-updates main contrib non-free non-free-firmware
deb http://deb.debian.org/debian bookworm-backports main contrib non-free non-free-firmware
deb http://security.debian.org/debian-security bookworm-security main contrib non-free non-free-firmware
APT_SOURCES
  elif [[ "${DISTRO}" == "bullseye" ]]; then
    echo "[apt-preflight] Normalizing Bullseye apt sources..."
    while IFS= read -r -d '' source_file; do
      if grep -q 'bullseye-backports' "${source_file}"; then
        case "${source_file}" in
          *.sources) sudo rm -f "${source_file}" ;;
          *) sudo sed -i '\|bullseye-backports|d' "${source_file}" ;;
        esac
      fi
    done < <(find /etc/apt -type f \( -name 'sources.list' -o -name '*.list' -o -name '*.sources' \) -print0 2>/dev/null)
    install_openhd_cloudsmith_key
  else
    return 0
  fi

  sudo tee /etc/apt/apt.conf.d/99openhd-ci-minimal-indexes >/dev/null <<'APT_CONF'
Acquire::Languages "none";
Acquire::IndexTargets::deb::DEP-11::DefaultEnabled "false";
Acquire::IndexTargets::deb::DEP-11-icons-small::DefaultEnabled "false";
Acquire::IndexTargets::deb::DEP-11-icons::DefaultEnabled "false";
APT::Install-Recommends "false";
APT::Install-Suggests "false";
APT_CONF
  sudo mkdir -p /out/apt-archives/partial
  sudo tee /etc/apt/apt.conf.d/99openhd-ci-cache-dir >/dev/null <<'APT_CONF'
Dir::Cache::archives "/out/apt-archives";
APT::Install-Recommends "false";
APT::Install-Suggests "false";
APT_CONF
  sudo rm -rf /var/lib/apt/lists/*
  sudo apt-get clean
}
apt() {
  if [[ "${1:-}" == "update" ]]; then
    openhd_glide_normalize_apt_sources
  fi
  command apt "$@"
}
apt-get() {
  if [[ "${1:-}" == "update" ]]; then
    openhd_glide_normalize_apt_sources
  fi
  command apt-get "$@"
}
openhd_glide_normalize_apt_sources
'''
    if packages_marker not in packages_text:
        if packages_needle not in packages_text:
            raise SystemExit("Could not find package-stage build marker in ChrootCompiler")
        packages_stage.write_text(packages_text.replace(packages_needle, packages_patch, 1))

    packages_text = packages_stage.read_text()
    apt_update_marker = "# openhd_glide_patch_apt_update_calls"
    if apt_update_marker not in packages_text:
        packages_text = packages_text.replace("set -e\n", f"set -e\n\n{apt_update_marker}\n", 1)
        apt_update_pattern = re.compile(r"^([ \t]*)((?:sudo\s+)?apt(?:-get)?\s+update(?:\s+.*)?)$", re.MULTILINE)
        packages_text = apt_update_pattern.sub(
            lambda match: f"{match.group(1)}{packages_marker}\n{match.group(1)}{match.group(2)}",
            packages_text,
        )
        packages_stage.write_text(packages_text)

    packages_text = packages_stage.read_text()
    out_mount_marker = "# openhd_glide_bind_out_guard"
    out_link_needle = """HOST=$(cat /opt/additionalFiles/mount.txt)
mkdir /host
mount $HOST /host
INDIR=$(cat /opt/additionalFiles/pwd.txt)
OUTDIR="/host"$INDIR
ln -s $OUTDIR /out
rm -Rf /out/*
"""
    out_link_patch = f"""{out_mount_marker}
if mountpoint -q /out; then
  rm -Rf /out/*
else
  HOST=$(cat /opt/additionalFiles/mount.txt)
  mkdir /host
  mount $HOST /host
  INDIR=$(cat /opt/additionalFiles/pwd.txt)
  OUTDIR="/host"$INDIR
  ln -s $OUTDIR /out
  rm -Rf /out/*
fi
"""
    if out_mount_marker not in packages_text:
        if out_link_needle not in packages_text:
            raise SystemExit("Could not find package-stage /out symlink block")
        packages_stage.write_text(packages_text.replace(out_link_needle, out_link_patch, 1))


def patch_common_chroot_mount() -> None:
    package_suffix = Path("/opt/OpenHD-ChrootCompiler/additionalFiles/package_suffix.txt").read_text().strip()
    workspace = Path(os.environ["GITHUB_WORKSPACE"])
    host_out = workspace / f"chroot-out-{package_suffix}"
    host_out.mkdir(parents=True, exist_ok=True)

    common_sh = Path("/opt/OpenHD-ChrootCompiler/scripts/common.sh")
    common_text = common_sh.read_text()
    common_marker = "# openhd_glide_bind_workspace_build"
    if common_marker in common_text:
        return

    mnt_dir = "$" + "{MNT_DIR}"
    common_needle = (
        '    cp -r "${STAGE_DIR}/../../additionalFiles" "${MNT_DIR}/opt"\n'
        '    capsh --drop=cap_setfcap "--chroot=${MNT_DIR}/" -- "$@"\n\n'
        '    umount -l "${MNT_DIR}/etc/resolv.conf"\n'
    )
    common_patch = (
        f"    {common_marker}\n"
        + f'    mkdir -p {host_out.as_posix()} "{mnt_dir}/mnt/openhd-glide-build"\n'
        + f'    mountpoint -q "{mnt_dir}/mnt/openhd-glide-build" || mount --bind {host_out.as_posix()} "{mnt_dir}/mnt/openhd-glide-build"\n'
        + '    cp -r "${STAGE_DIR}/../../additionalFiles" "${MNT_DIR}/opt"\n'
        + '    capsh --drop=cap_setfcap "--chroot=${MNT_DIR}/" -- "$@"\n\n'
        + '    if mountpoint -q "${MNT_DIR}/mnt/openhd-glide-build"; then\n'
        + '        umount -l "${MNT_DIR}/mnt/openhd-glide-build"\n'
        + '    fi\n\n'
        + '    umount -l "${MNT_DIR}/etc/resolv.conf"\n'
    )
    if common_needle not in common_text:
        raise SystemExit("Could not find ChrootCompiler chroot entry block")
    common_sh.write_text(common_text.replace(common_needle, common_patch, 1))


def patch_build_script() -> None:
    build_sh = Path("/opt/OpenHD-ChrootCompiler/build.sh")
    text = build_sh.read_text()
    original = text

    imagebuilder_clone_old = "git clone https://github.com/OpenHD/OpenHD-ImageBuilder\n"
    imagebuilder_clone_new = "git clone --branch dev-release --single-branch https://github.com/OpenHD/OpenHD-ImageBuilder\n"
    if imagebuilder_clone_new not in text:
        if imagebuilder_clone_old not in text:
            raise SystemExit("Could not find ImageBuilder clone in build.sh")
        text = text.replace(imagebuilder_clone_old, imagebuilder_clone_new, 1)

    gdrive_script_marker = "# openhd_glide_import_gdrive_helper"
    if gdrive_script_marker not in text:
        gdrive_script_needle = "mv OpenHD-ImageBuilder/stages/01-Baseimage stages/\n"
        gdrive_script_patch = (
            gdrive_script_needle
            + f"{gdrive_script_marker}\n"
            + "cp OpenHD-ImageBuilder/scripts/gdrive.sh scripts/gdrive.sh\n"
        )
        if gdrive_script_needle not in text:
            raise SystemExit("Could not find ImageBuilder base-stage import in build.sh")
        text = text.replace(gdrive_script_needle, gdrive_script_patch, 1)

    image_needle = "mv OpenHD-ImageBuilder/images .\n"
    image_patch = (
        image_needle
        + "for board in radxa-zero3w orangepi-zero3w-lite radxa-cubie rock5a rock5b radxa-cm5; do\n"
        + '  if [ ! -f "images/${board}" ] && [ -f "images/${board}_base" ]; then\n'
        + '    cp "images/${board}_base" "images/${board}"\n'
        + "  fi\n"
        + "done\n"
    )
    if "orangepi-zero3w-lite radxa-cubie rock5a rock5b radxa-cm5" not in text:
        if image_needle not in text:
            raise SystemExit("Could not find ImageBuilder image import in build.sh")
        text = text.replace(image_needle, image_patch, 1)

    if "export BASE_IMAGE_SHA512" not in text:
        text = text.replace("export BASE_IMAGE_SHA256\n", "export BASE_IMAGE_SHA256\nexport BASE_IMAGE_SHA512\n", 1)
    if "export BASE_IMAGE_GDRIVE_URL" not in text:
        text = text.replace(
            "export BASE_IMAGE_URL\n",
            "export BASE_IMAGE_GDRIVE_URL\nexport BASE_IMAGE_GDRIVE_RCLONE_PATH\nexport BASE_IMAGE_URL\n",
            1,
        )

    needle = "mv OpenHD-ImageBuilder/stages/01-Baseimage stages/\n"
    patch = (
        needle
        + "python3 - <<'PATCH_IMAGE_SIZE'\n"
        + "from pathlib import Path\n"
        + "stage = Path('stages/01-Baseimage/01-run.sh')\n"
        + "stage_text = stage.read_text()\n"
        + '''old = """if [[ "${OS}" == radxa-debian-rock5a ]] || [[ "${OS}" == radxa-debian-rock5b ]];then
    WANTEDSIZE="6800000000"
    else
    WANTEDSIZE="${ROOT_IMAGE_SIZE_BYTES:-5632000000}"
    fi"""\n'''
        + '''new = """if [[ "${OS}" == radxa-debian-rock5b ]]; then
    WANTEDSIZE="22000000000"
    elif [[ "${OS}" == radxa-debian-rock5a ]]; then
    WANTEDSIZE="15000000000"
    else
    WANTEDSIZE="${ROOT_IMAGE_SIZE_BYTES:-5632000000}"
    fi"""\n'''
        + "if new not in stage_text:\n"
        + "    if old not in stage_text:\n"
        + "        raise SystemExit('Could not find Rock5 image size block')\n"
        + "    stage.write_text(stage_text.replace(old, new, 1))\n"
        + "PATCH_IMAGE_SIZE\n"
    )
    if "PATCH_IMAGE_SIZE" not in text:
        if needle not in text:
            raise SystemExit("Could not find ImageBuilder stage import in build.sh")
        text = text.replace(needle, patch, 1)

    if text != original:
        build_sh.write_text(text)


def main() -> None:
    patch_package_stage()
    patch_common_chroot_mount()
    patch_build_script()


if __name__ == "__main__":
    main()
