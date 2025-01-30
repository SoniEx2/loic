project('loic', 'c')

gnome = import('gnome')

loic_resources = gnome.compile_resources('loic_resources',
  'loic.gresource.xml', source_dir: meson.current_source_dir())

gtkdep = dependency('gtk4')
giodep = dependency('gio-2.0')

executable('loic',
  'main.c',
  'loic-app.c',
  'loic-app-window.c',
  loic_resources,
  dependencies: [gtkdep, giodep])
