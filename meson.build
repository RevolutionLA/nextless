project('fcitx5-nextless', 'cpp',
  version: '0.3.1',
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

# 打包默认配置：装进构建时的 sysconfdir/nextless，并把同一个路径编译进
# ASR_provider（NEXTLESS_PACKAGED_CONFIG_DIR），loader 读的就是这次安装写进去的
# 地方。发行版构建（PKGBUILD 和未来的 .deb 显式传 --sysconfdir=/etc，prefix=/usr
# 时 meson 本身也解析成 /etc）→ /etc/nextless；普通源码安装（prefix=/usr/local）
# → /usr/local/etc/nextless。此前 loader 写死 /etc/nextless、而源码安装从没有
# 规则往那里装东西，两边各说各话。现在装与读同源。
sysconfdir = get_option('sysconfdir')
if not sysconfdir.startswith('/')
  sysconfdir = get_option('prefix') / sysconfdir
endif
config_install_dir = sysconfdir / 'nextless'
install_data('config/doubao.json.example',
  'config/qwen.json.example',
  'config/audio.json.example',
  'config/nextless.json.example',
  'config/advanced.json.example',
  'config/hotwords.json.example',
  rename: ['doubao.json', 'qwen.json', 'audio.json', 'nextless.json', 'advanced.json', 'hotwords.json'],
  install_dir: config_install_dir)
add_project_arguments('-DNEXTLESS_PACKAGED_CONFIG_DIR="' + config_install_dir + '"',
  language: 'cpp')

subdir('ASR_provider/src')
subdir('tests')
subdir('tools')
subdir('adapter')

install_man('docs/nextless.1')
