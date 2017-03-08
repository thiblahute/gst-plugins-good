wavpack_sources = [
  'gstwavpack.c',
  'gstwavpackcommon.c',
  'gstwavpackdec.c',
  'gstwavpackenc.c',
  'gstwavpackstreamreader.c',
]

wavpack_comp = component('wavpack')
wavpack_dep = dependency('wavpack', version : '>= 4.60.0', required : false,
  components: wavpack_comp)

gstwavpack = library('gstwavpack',
  wavpack_sources,
  c_args : gst_plugins_good_args,
  link_args : noseh_link_args,
  include_directories : [configinc, libsinc],
  dependencies : [gstbase_dep, gstaudio_dep, wavpack_comp.dependencies()],
  install : true,
  install_dir : plugins_install_dir,
  components: wavpack_comp
)
