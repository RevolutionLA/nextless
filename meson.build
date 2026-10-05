project('fcitx5-nextless', 'cpp',
  version: '0.2.0',
  default_options: ['cpp_std=c++20', 'warning_level=3', 'werror=true'])

add_project_arguments('-Wno-variadic-macros', language: 'cpp')
add_project_arguments('-DNEXTLESS_DIAGNOSTICS_ENABLED=' +
  (get_option('diagnostic_logging') ? '1' : '0'), language: 'cpp')

subdir('ASR_provider/src')
subdir('tests')
subdir('tools')
subdir('adapter')

install_man('docs/nextless.1')
