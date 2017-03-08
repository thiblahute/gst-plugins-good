dv_sources = [
  'gstdv.c',
  'gstdvdec.c',
  'gstdvdemux.c',
  'gstsmptetimecode.c',
  'smpte_test.c',
]

dv_comp = component('dv')
dv_dep = dependency('libdv', version : '>= 0.100', required : false,
  components: dv_comp)

gstdv = library('gstdv',
  dv_sources,
  c_args : gst_plugins_good_args,
  include_directories : [configinc, libsinc],
  dependencies : [gstbase_dep, gsttag_dep, gstaudio_dep, gstvideo_dep, dv_comp.dependencies()],
  install : true,
  install_dir : plugins_install_dir,
  components: dv_comp
)

#executable('smpte_test',
#  'smpte_test.c', 'gstsmptetimecode.c',
#  dependencies : [gstbase_dep, gsttag_dep, gstaudio_dep, gstvideo_dep, dv_comp.dependencies()],
#  install : false)
