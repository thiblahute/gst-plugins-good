# Very much not going to implement all kinds of logic around aalib-config
# or cater for non-standard prefixes.
libaa_comp = component('aalib')
libaa_dep = cc.find_dependency('aa',
  libs: ['aa'],
  headers: ['aalib.h'],
  required : false,
  components: libaa_comp
)

library('gstaasink', 'gstaasink.c',
  c_args : gst_plugins_good_args,
  link_args : noseh_link_args,
  include_directories : [configinc],
  dependencies : [gstvideo_dep, gstbase_dep, libaa_comp.dependencies()],
  install : true,
  install_dir : plugins_install_dir,
  components: libaa_comp
)
