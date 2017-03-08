raw1394_comp = component('raw1394')

raw1394_dep = dependency('libraw1394', version: '>= 2.0.0', required: false,
        components: raw1394_comp)
avc1394_dep = dependency('libavc1394', version: '>= 0.5.4', required: false,
        components: raw1394_comp)
iec61883_dep = dependency('libiec61883', version: '>= 1.0.0', required: false,
        components: raw1394_comp)

raw1394_funcs_dep = cc.find_dependency('raw1394_funcs',
  headers_symbols: [['libavc1394/avc1394.h', 'avc1394_send_command'],
                    ['libavc1394/avc1394.h', 'rom1394_free_directory']],
  required : false,
  dependencies: avc1394_dep,
  components: raw1394_comp
)

library('gst1394',
  'gst1394.c', 'gst1394probe.c', 'gstdv1394src.c', 'gsthdv1394src.c', 'gst1394clock.c',
  c_args : gst_plugins_good_args + ['-DHAVE_LIBIEC61883'],
  include_directories : [configinc, libsinc],
  dependencies : [gstvideo_dep, gstbase_dep, raw1394_dep, avc1394_dep, iec61883_dep],
  install : true,
  install_dir : plugins_install_dir,
  components: raw1394_comp
)
