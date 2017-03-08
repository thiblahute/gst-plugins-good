soup_sources = [
  'gstsouphttpsrc.c',
  'gstsouphttpclientsink.c',
  'gstsouputils.c',
  'gstsoup.c',
]

soup_args = [
  '-DSOUP_VERSION_MIN_REQUIRED=SOUP_VERSION_2_48',
  '-DSOUP_VERSION_MAX_ALLOWED=SOUP_DEPRECATED_IN_2_48',
]

libsoup_comp = component('soup')
libsoup_dep = dependency('libsoup-2.4', version : '>=2.48', required : false,
  components: libsoup_comp)

gstsouphttpsrc = library('gstsoup',
  soup_sources,
  c_args : gst_plugins_good_args + soup_args,
  link_args : noseh_link_args,
  include_directories : [configinc, libsinc],
  dependencies : [gst_dep, gstbase_dep, gsttag_dep, libsoup_comp.dependencies()],
  install : true,
  install_dir : plugins_install_dir,
  components: libsoup_comp
)
