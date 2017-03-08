jpeg_sources = [
  'gstjpeg.c',
  'gstjpegenc.c',
  'gstjpegdec.c',
]

jpeg_comp = component('jpeg')
jpeg_dep = cc.find_dependency('jpeg',
  libs: 'jpeg',
  required : false,
  components: jpeg_comp
)

gstjpeg = library('gstjpeg',
  jpeg_sources,
  c_args : gst_plugins_good_args,
  link_args : noseh_link_args,
  include_directories : [configinc, libsinc],
  dependencies : [gst_dep, gstbase_dep, gstvideo_dep, jpeg_comp.dependencies(), libm],
  install : true,
  install_dir : plugins_install_dir,
  components: jpeg_comp
)
