pulse_sources = [
  'plugin.c',
  'pulsesink.c',
  'pulsesrc.c',
  'pulsedeviceprovider.c',
  'pulseutil.c',
]

pulse_comp = component('pulse')
libpulse_dep = dependency('libpulse', version : '>=2.0', required : false,
  components: pulse_comp)

gstpulse = library('gstpulseaudio',
  pulse_sources,
  c_args : gst_plugins_good_args,
  include_directories : [configinc, libsinc],
  dependencies : [gst_dep, gstbase_dep, gstaudio_dep, gstpbutils_dep, pulse_comp.dependencies()],
  install : true,
  install_dir : plugins_install_dir,
  components: pulse_comp
)
