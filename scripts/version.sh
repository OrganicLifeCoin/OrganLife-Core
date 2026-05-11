#!/usr/bin/env bash
#
# Shared version parsing helpers for build scripts.

set -euo pipefail

get_client_version_component() {
  local configure_ac="${1:?missing configure.ac path}"
  local component="${2:?missing component name}"

  awk -v component="$component" '
    $0 ~ "^define\\(_" component ",[[:space:]]*[0-9]+\\)" {
      line = $0
      sub(/^define\(_[^,]+,[[:space:]]*/, "", line)
      sub(/\).*/, "", line)
      print line
      exit
    }
  ' "$configure_ac"
}

get_client_marketing_version() {
  local configure_ac="${1:?missing configure.ac path}"
  local major minor revision

  major="$(get_client_version_component "$configure_ac" CLIENT_VERSION_MAJOR)"
  minor="$(get_client_version_component "$configure_ac" CLIENT_VERSION_MINOR)"
  revision="$(get_client_version_component "$configure_ac" CLIENT_VERSION_REVISION)"

  echo "${major}.${minor}.${revision}"
}

get_client_version_full() {
  local configure_ac="${1:?missing configure.ac path}"
  local marketing build

  marketing="$(get_client_marketing_version "$configure_ac")"
  build="$(get_client_version_component "$configure_ac" CLIENT_VERSION_BUILD)"

  echo "${marketing}.${build}"
}

get_client_package_version() {
  local configure_ac="${1:?missing configure.ac path}"
  local version build rc

  version="$(get_client_marketing_version "$configure_ac")"
  build="$(get_client_version_component "$configure_ac" CLIENT_VERSION_BUILD)"
  rc="$(get_client_version_component "$configure_ac" CLIENT_VERSION_RC)"

  if [ -n "$build" ] && [ "$build" != "0" ]; then
    version="${version}.${build}"
  fi
  if [ -n "$rc" ] && [ "$rc" != "0" ]; then
    version="${version}rc${rc}"
  fi

  echo "$version"
}
