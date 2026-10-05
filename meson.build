project('fcitx5-nextless', 'cpp',
  version: '0.2.0',
  default_options: ['cpp_std=c++20', 'warning_level=2', 'werror=true'])

# warning_level=2 is -Wall -Wextra (plus -Werror); 3 would add -Wpedantic, and that one is not
# satisfiable while we use fcitx5's own macros: FCITX_CONFIGURATION / FCITX_DECLARE_PRIVATE expand
# to a trailing semicolon at class scope, which GCC's -Wpedantic calls "extra ';'" on the
# fcitx5 releases distros actually package (seen on Ubuntu 24.04, fine on 5.1.19). Marking those
# headers as system includes covers the in-header case, but the macro expands on *our* line, so
# the warning is ours to live with. Everything else stays strict: a new -Wall/-Wextra warning
# fails CI.
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
