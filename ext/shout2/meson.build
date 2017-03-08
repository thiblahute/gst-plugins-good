shout2_sources = ['gstshout2.c']

shout2_comp = component('shout2')
shout2_dep = dependency('shout', version : '>= 2.0', required : false,
  components: shout2_comp)
gstshout2 = library('gstshout2',
  shout2_sources,
  c_args : gst_plugins_good_args,
  include_directories : [configinc, libsinc],
  dependencies : [gstbase_dep, shout2_comp.dependencies()],
  install : true,
  install_dir : plugins_install_dir,
  components: shout2_comp
)
