png_sources = [
  'gstpng.c',
  'gstpngenc.c',
  'gstpngdec.c',
]

libpng_comp = component('libpng')
libpng_dep = dependency('libpng', version : '>=1.2', required : false,
  components: libpng_comp)

gstpng = library('gstpng',
  png_sources,
  c_args : gst_plugins_good_args,
  link_args : noseh_link_args,
  include_directories : [configinc, libsinc],
  dependencies : [gstbase_dep, gstvideo_dep, libpng_comp.dependencies()],
  install : true,
  install_dir : plugins_install_dir,
  components: libpng_comp
)
