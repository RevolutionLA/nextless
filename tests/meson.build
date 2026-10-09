test_asr_registry = executable('test_asr_registry',
  'test_asr_registry.cpp',
  dependencies: [asr_provider_dep])

test_audio_capture_file = executable('test_audio_capture_file',
  'test_audio_capture_file.cpp',
  dependencies: [asr_provider_dep])

test_buffer_detect_cache = executable('test_buffer_detect_cache',
  'test_buffer_detect_cache.cpp',
  dependencies: [asr_provider_dep])

test_config_fallback = executable('test_config_fallback',
  'test_config_fallback.cpp',
  dependencies: [asr_provider_dep])

test_provider_lifetime = executable('test_provider_lifetime',
  'test_provider_lifetime.cpp',
  dependencies: [asr_provider_dep])

test_curl_cancellation = executable('test_curl_cancellation',
  'test_curl_cancellation.cpp',
  dependencies: [asr_provider_dep])

test_audio_capture_stop = executable('test_audio_capture_stop',
  'test_audio_capture_stop.cpp',
  dependencies: [asr_provider_dep])

test_cloud_provider_queue = executable('test_cloud_provider_queue',
  'test_cloud_provider_queue.cpp',
  dependencies: [asr_provider_dep])

test_diagnostic_log = executable('test_diagnostic_log',
  'test_diagnostic_log.cpp',
  dependencies: [asr_provider_dep])

test_no_speech = executable('test_no_speech',
  'test_no_speech.cpp',
  dependencies: [asr_provider_dep])

test_denoise_fallback = executable('test_denoise_fallback',
  'test_denoise_fallback.cpp',
  dependencies: [asr_provider_dep])

test_missing_components = executable('test_missing_components',
  'test_missing_components.cpp',
  include_directories: include_directories('../adapter/src'),
  dependencies: [asr_provider_dep])

test_audio_config_loader = executable('test_audio_config_loader',
  'test_audio_config_loader.cpp',
  dependencies: [asr_provider_dep])

test_capture_dir_sweep = executable('test_capture_dir_sweep',
  'test_capture_dir_sweep.cpp',
  dependencies: [asr_provider_dep])

test_punctuator = executable('test_punctuator',
  'test_punctuator.cpp',
  dependencies: [asr_provider_dep])

test_hotwords = executable('test_hotwords',
  'test_hotwords.cpp',
  dependencies: [asr_provider_dep])

test('asr_registry', test_asr_registry)
test('audio_capture_file', test_audio_capture_file)
test('buffer_detect_cache', test_buffer_detect_cache)
test('config_fallback', test_config_fallback)
test('provider_lifetime', test_provider_lifetime)
test('curl_cancellation', test_curl_cancellation)
test('audio_capture_stop', test_audio_capture_stop)
test('cloud_provider_queue', test_cloud_provider_queue)
test('diagnostic_log', test_diagnostic_log)
test('no_speech', test_no_speech)
test('denoise_fallback', test_denoise_fallback, timeout: 30)
test('missing_components', test_missing_components)
test('audio_config_loader', test_audio_config_loader)
test('capture_dir_sweep', test_capture_dir_sweep, timeout: 30)
test('punctuator', test_punctuator, timeout: 60)
test('hotwords', test_hotwords, timeout: 30)

# Issue #5: get_models.sh contract test — arch/asset-name mapping, install
# layout, idempotency, and "no half-extracted model on failure" (runs
# against fake file:// release trees, no network).
test('get_models', find_program('test_get_models.sh'), timeout: 120)

# Issue #8: CER/WER normaliser + table maths (python, stdlib only).
test('cer_tool', find_program('run_cer_tests.sh'), timeout: 60)

# Issue #43: install-deb.sh digest gate — verifies the installer fails closed
# when the sha256 is missing/unlisted, that a mismatch blocks apt, and that
# only --allow-unverified / ALLOW_UNVERIFIED=1 may proceed. root, apt and the
# network are stubbed on PATH, so it runs in CI like any other test.
test('install_deb', find_program('test_install_deb.sh'), timeout: 60)

# Config-panel JSON writer: round-trips through the same extractors the
# providers use (issue: full settings panel).
test_json_patch = executable('test_json_patch',
  'test_json_patch.cpp',
  dependencies: [asr_provider_dep])
test('json_patch', test_json_patch)
