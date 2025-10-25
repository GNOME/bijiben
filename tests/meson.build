tests_inc = [src_inc, top_inc]

env = environment()
env.set('G_TEST_SRCDIR', meson.current_source_dir())
env.set('G_TEST_BUILDDIR', meson.current_build_dir())
env.set('GSETTINGS_SCHEMA_DIR', join_paths(meson.build_root(), 'data'))
env.set('GSETTINGS_BACKEND', 'memory')
env.set('G_DEBUG', 'gc-friendly')

test_items = [
  # 'plain-note',
  'xml-note',
]

foreach item: test_items
  t = executable(
    item,
    item + '.c',
    include_directories: tests_inc,
    link_with: libbijiben,
    dependencies: bijiben_deps,
  )
  test(item, t, env: env)
endforeach
