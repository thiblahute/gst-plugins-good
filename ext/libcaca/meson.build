libcaca_comp = component('caca')
libcaca_dep = dependency('caca', required : false, components: libcaca_comp)

library('gstcacasink', 'gstcacasink.c',
  c_args : gst_plugins_good_args,
  link_args : noseh_link_args,
  include_directories : [configinc],
  dependencies : [gstvideo_dep, gstbase_dep, libcaca_dep],
  install : true,
  install_dir : plugins_install_dir,
  components: libcaca_comp)
