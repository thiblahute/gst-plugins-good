jack_sources = [
  'gstjackaudioclient.c',
  'gstjackaudiosink.c',
  'gstjackaudiosrc.c',
  'gstjack.c',
  'gstjackutil.c',
]


libjack_comp = component('jack')
libaa_dep = dependency('jack', version : '>= 1.9.7', required : false, components: libjack_comp)

gstjack = library('gstjack',
  jack_sources,
  c_args : gst_plugins_good_args + ['-DHAVE_JACK_1_9_7'],
  include_directories : [configinc, libsinc],
  dependencies : [gst_dep, gstbase_dep, gstaudio_dep, libjack_comp.dependencies()],
  install : true,
  install_dir : plugins_install_dir,
  components: libjack_comp
)
