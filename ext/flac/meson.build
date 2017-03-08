flac_sources = [
  'gstflac.c',
  'gstflacdec.c',
  'gstflacenc.c',
  'gstflactag.c',
]

flac_comp = component('flac')
flac_dep = dependency('flac', version : '>=1.1.4', required : false,
  components: flac_comp)
gstflac = library('gstflac',
  flac_sources,
  c_args : gst_plugins_good_args + ['-DGST_USE_UNSTABLE_API'],
  link_args : noseh_link_args,
  include_directories : [configinc, libsinc],
  dependencies : [gstbase_dep, gsttag_dep, gstaudio_dep, flac_comp.dependencies()],
  install : true,
  install_dir : plugins_install_dir,
  components: flac_comp
)
