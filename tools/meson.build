pulse_simple = dependency('libpulse-simple')

test_audio_pipeline = executable('test_audio_pipeline',
  'test_audio_pipeline.cpp',
  dependencies: [asr_provider_dep, pulse_simple],
  install: false)

test_audio_file = executable('test_audio_file',
  'test_audio_file.cpp',
  dependencies: [asr_provider_dep],
  install: false)

test_uinput_caps = executable('test_uinput_caps',
  'test_uinput_caps.cpp',
  install: false)

# Issue #8: benchmark harness runner (tools/benchmark/README.md).
bench_asr = executable('bench_asr',
  'benchmark/bench_asr.cpp',
  dependencies: [asr_provider_dep],
  install: false)

# Issue #5: first-run model helper. Installed as nextless-get-models so the
# README path from `meson install` to working dictation needs no hand-written
# curl (PKGBUILD's meson install picks this up automatically).
install_data('get_models.sh',
  install_dir: get_option('bindir'),
  install_mode: 'rwxr-xr-x',
  rename: 'nextless-get-models')
