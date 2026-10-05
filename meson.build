project('fcitx5-nextless', 'cpp',
  version: '0.2.0',
  default_options: ['cpp_std=c++20', 'warning_level=3', 'werror=true'])

add_project_arguments('-Wno-variadic-macros', language: 'cpp')
add_project_arguments('-DNEXTLESS_DIAGNOSTICS_ENABLED=' +
  (get_option('diagnostic_logging') ? '1' : '0'), language: 'cpp')

# 提示音装在 datadir 下, 编译期把绝对路径告诉 adapter。用户在
# ~/.local/share/nextless/sounds/ 放同名文件即可覆盖音色。
sound_install_dir = get_option('datadir') / 'nextless' / 'sounds'
install_data('sounds/activate.wav', 'sounds/deactivate.wav', 'sounds/switch.wav',
  install_dir: sound_install_dir)
add_project_arguments('-DNEXTLESS_PACKAGED_SOUND_DIR="' +
  (get_option('prefix') / sound_install_dir) + '"', language: 'cpp')

subdir('ASR_provider/src')
subdir('tests')
subdir('tools')
subdir('adapter')

install_man('docs/nextless.1')
