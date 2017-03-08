speex_sources = [
  'gstspeex.c',
  'gstspeexdec.c',
  'gstspeexenc.c',
]

speex_comp = component('speex')
speex_dep = dependency('speex', version : '>=1.1.6', required : false,
  components: speex_comp)

gstspeex = library('gstspeex',
  speex_sources,
  c_args : gst_plugins_good_args + ['-DGST_USE_UNSTABLE_API'],
  link_args : noseh_link_args,
  include_directories : [configinc],
  dependencies : [gstbase_dep, gsttag_dep, gstaudio_dep, speex_comp.dependencies(), libm],
  install : true,
  install_dir : plugins_install_dir,
  components: speex_comp
)
