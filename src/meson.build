revision_tag = vcs_tag(
  input: 'version.h.in',
  output: 'version.h',
)
bijiben_deps += declare_dependency(sources: revision_tag)

libsrc = files(
  'editor/biji-editor-selection.c',
  'editor/biji-webkit-editor.c',
  'items/bjb-item.c',
  'items/bjb-tag.c',
  'items/bjb-note.c',
  'items/bjb-tag-store.c',
  'items/bjb-plain-note.c',
  'items/bjb-xml-note.c',
  'providers/bjb-provider.c',
  'providers/bjb-memo-provider.c',
  'providers/bjb-local-provider.c',
  'providers/bjb-nc-provider.c',
  'biji-date-time.c',
  'biji-date-time.c',
  'biji-string.c',
  'bjb-dex.c',
  'bjb-utils.c',
  'bjb-log.c',
  'bjb-application.c',
  'bjb-editor-toolbar.c',
  'bjb-import-dialog.c',
  'bjb-list-view-row.c',
  'bjb-manager.c',
  'bjb-note-list.c',
  'bjb-note-view.c',
  'bjb-side-view.c',
  'bjb-notebooks-dialog.c',
  'bjb-settings.c',
  'bjb-settings-dialog.c',
  'bjb-share.c',
  'bjb-window.c',
)

sources = files(
  'bjb-main.c',
)

sources += gnome.compile_resources(
  'bjb-resources',
  gresource,
  c_name: 'bjb',
  source_dir: data_dir,
  dependencies: resource_data,
  export: true
)

libbijiben = static_library(
  'bijiben', libsrc,
  include_directories: [top_inc, src_inc],
  dependencies: bijiben_deps,
)

executable(
  bijiben_name,
  sources,
  include_directories: [top_inc, src_inc],
  dependencies: bijiben_deps,
  link_with: libbijiben,
  install: true,
  install_dir: bijiben_bindir
)

sources = files('bijiben-shell-search-provider.c')

sources += gnome.gdbus_codegen(
  'bijiben-shell-search-provider-generated',
  search_provider_dbus_iface,
  interface_prefix: 'org.gnome.',
  namespace: 'Bijiben'
)

executable(
  'bijiben-shell-search-provider',
  sources,
  include_directories: [top_inc, src_inc],
  dependencies: bijiben_deps,
  install: true,
  install_dir: bijiben_libexecdir
)
