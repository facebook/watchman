#!/bin/bash
set -x
uname -a

# Speculatively fulfil any node deps before we turn on hard errors
cd node
npm install
cd ..

set -e
PATH=$PWD:$PATH

case $(uname) in
  Darwin)
    eval "$(pyenv init -)"
    ;;
  Linux)
    if [ "${ENABLE_ASAN-0}" -eq 1 ]; then
      # Some versions of gcc-5 appear to have a bug where -fsanitize-address
      # doesn't automatically enable the gold linker.
      LDFLAGS="$LDFLAGS -fuse-ld=gold"
      export LDFLAGS
    fi
    ;;
esac

INST_TEST=/tmp/install-test
if test "$CIRCLE" = "true" ; then
  INST_TEST="$CIRCLE_ARTIFACTS/install-test"
fi

rm -rf /tmp/watchman* $INST_TEST
rm -rf /var/tmp/watchmantest*
TMP=/var/tmp
TMPDIR=$TMP
export TMPDIR TMP

set +e

GETDEPS="build/fbcode_builder/getdeps.py"

"$GETDEPS" build watchman
"$GETDEPS" test watchman

inst_dir=$("$GETDEPS" show-inst-dir watchman)
src_dir=$("$GETDEPS" show-source-dir watchman)

python "${src_dir}/runtests.py" \
  --pybuild-dir="${inst_dir}/../../build/watchman/python/build" \
  --watchman-path="${inst_dir}/bin/watchman"

exit 0

