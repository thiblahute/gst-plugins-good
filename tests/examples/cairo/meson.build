executable('cairo_overlay', 'cairo_overlay.c',
  dependencies: [gstvideo_dep, gst_dep, cairo_comp.dependencies(), libm],
  c_args : gst_plugins_good_args,
  include_directories : [configinc],
  install: false,
  components: cairo_comp)
