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
