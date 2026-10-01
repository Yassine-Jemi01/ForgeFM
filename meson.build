project('forgefm', 'c',
  version: '0.2.0',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    'buildtype=debugoptimized',
  ]
)

gtk = dependency('gtk4', version: '>= 4.10')

target = executable('forgefm',
  'src/main.c',
  'src/app.c',
  dependencies: [gtk],
  install: true,
)

install_data('README.md', install_dir: get_option('datadir') / 'forgefm')
