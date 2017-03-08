lame_comp = component('mp3lame')
lame_dep = cc.find_dependency('mp3lame', required : false,
        libs: ['mp3lame'],
        headers_symbols: [['lame/lame.h', 'lame_init']],
        components: [lame_comp]
)

lame_extra_c_args = []
if lame_comp.enabled()
  if cc.has_header_symbol('lame/lame.h', 'lame_set_VBR_quality')
    lame_extra_c_args += ['-DHAVE_LAME_SET_VBR_QUALITY']
  endif
  if cc.has_header_symbol('lame/lame.h', 'MEDIUM')
    lame_extra_c_args += ['-DGSTLAME_PRESET']
  endif
endif

lame = library('gstlame',
  ['gstlamemp3enc.c', 'plugin.c'],
  c_args : gst_plugins_good_args + lame_extra_c_args,
  include_directories : [configinc, libsinc],
  dependencies : [gstaudio_dep, lame_dep],
  install : true,
  install_dir : plugins_install_dir,
  components: lame_comp
)
