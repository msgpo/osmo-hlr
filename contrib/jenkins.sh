#!/bin/sh
# jenkins build helper script for osmo-hlr.  This is how we build on jenkins.osmocom.org

if ! [ -x "$(command -v osmo-build-dep.sh)" ]; then
	echo "Error: We need to have scripts/osmo-deps.sh from http://git.osmocom.org/osmo-ci/ in PATH !"
	exit 2
fi


set -ex

base="$PWD"
deps="$base/deps"
inst="$deps/install"
export deps inst

osmo-clean-workspace.sh

mkdir "$deps" || true

verify_value_string_arrays_are_terminated.py $(find . -name "*.[hc]")

export PKG_CONFIG_PATH="$inst/lib/pkgconfig:$PKG_CONFIG_PATH"
export LD_LIBRARY_PATH="$inst/lib"

osmo-build-dep.sh libosmocore "" ac_cv_path_DOXYGEN=false
osmo-build-dep.sh libosmo-abis

set +x
echo
echo
echo
echo " =============================== osmo-hlr ==============================="
echo
set -x

cd "$base"
autoreconf --install --force
./configure --enable-external-tests
$MAKE $PARALLEL_MAKE
if [ "x$label" != "xFreeBSD_amd64" ]; then
    $MAKE check || cat-testlogs.sh
    $MAKE distcheck || cat-testlogs.sh
fi

osmo-clean-workspace.sh
