#!/bin/sh
# Force OpenGL ES 3.0 on Mesa Panfrost / Mali-T720 through Android properties.
#
# Mesa reads its env options from properties on Android
# (src/util/os_misc.c: MESA_FOO_BAR -> debug.mesa.foo.bar), so no rebuild is
# needed. T720 (Midgard v4) has one render target, so this is NOT conformant
# ES 3.0: glDrawBuffers with more than one buffer fails (GL_MAX_DRAW_BUFFERS=1).
#
#   tests/gles3-override.sh test   push + run tests/gles3test in a shell process only
#   tests/gles3-override.sh on     system-wide: ES 3.0 for all 64-bit apps (reboot)
#   tests/gles3-override.sh off    undo "on" (reboot)
#
# on/off need adb root + remount (they edit /vendor/build.prop).
set -eu

ADB=${ADB:-adb}
TOP=$(cd "$(dirname "$0")/.." && pwd)

case "${1:-test}" in
test)
    BIN=$TOP/out/tests/gles3test
    [ -f "$BIN" ] || { echo "build it first: ./build.sh tests"; exit 1; }
    $ADB push "$BIN" /data/local/tmp/gles3test >/dev/null
    $ADB shell chmod 755 /data/local/tmp/gles3test
    echo "== ES 2.0 (no override)"
    $ADB shell /data/local/tmp/gles3test || true
    echo
    echo "== ES 3.0 (MESA_GLES_VERSION_OVERRIDE=3.0, env only)"
    $ADB shell MESA_GLES_VERSION_OVERRIDE=3.0 /data/local/tmp/gles3test || true
    ;;
on)
    # Mesa looks up debug.mesa.*, then vendor.mesa.*, then mesa.* (os_misc.c),
    # but builds the key in a PROP_NAME_MAX (32) buffer: the debug./vendor.
    # forms of this name are 32/33 chars and get truncated. Only the bare
    # mesa.* key works. Shell can't setprop it (SELinux default_prop), init can.
    # Apps pick ES 3 paths from ro.opengles.version (196608 = 0x30000 = 3.0).
    $ADB root >/dev/null
    $ADB remount >/dev/null
    $ADB shell "sed -i -e '/^mesa.gles.version.override=/d' \\
        -e 's/^ro.opengles.version=.*/ro.opengles.version=196608/' /vendor/build.prop && \\
        echo mesa.gles.version.override=3.0 >> /vendor/build.prop"
    echo "ES 3.0 override on. Reboot: $ADB reboot"
    ;;
off)
    $ADB root >/dev/null
    $ADB remount >/dev/null
    $ADB shell "sed -i -e '/^mesa.gles.version.override=/d' \\
        -e 's/^ro.opengles.version=.*/ro.opengles.version=131072/' /vendor/build.prop"
    echo "ES 3.0 override off. Reboot: $ADB reboot"
    ;;
*)
    echo "usage: $0 test|on|off"; exit 1 ;;
esac
