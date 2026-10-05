# include_type: 'system' — the packaged fcitx5 headers are not -Wpedantic-clean on
# the versions distros ship (FCITX_DECLARE_PRIVATE leaves a class-scope semicolon that
# GCC reports as "extra ';'"), and we cannot patch them from here. Treating them as
# system headers keeps -Wall -Wextra -Werror fully on for Nextless' own code while
# any distro's headers pass through untouched. -Wpedantic is deliberately off at the
# project level (root meson.build: warning_level=2) to avoid breaking Ubuntu 24.04.
fcitx5_core = dependency('Fcitx5Core', include_type: 'system')
fcitx5_module = dependency('Fcitx5Module', include_type: 'system')
pulse_simple = dependency('libpulse-simple')
ebur128 = dependency('libebur128')
libcurl = dependency('libcurl')
soxr = dependency('soxr')
speexdsp = dependency('speexdsp')

asr_provider = asr_provider_dep

subdir('src')
