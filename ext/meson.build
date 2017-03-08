subdir('aalib', components: [plugins_component])
subdir('cairo', components: [plugins_component])
subdir('flac', components: [plugins_component])
subdir('gdk_pixbuf', components: [plugins_component])
subdir('jack', components: [plugins_component])
subdir('jpeg', components: [plugins_component])
subdir('lame', components: [plugins_component])
subdir('libcaca', components: [plugins_component])
# FIXME: dv plugin fails to link with msvc, wants pthread.lib
if cc.get_id() != 'msvc'
  subdir('dv', components: [plugins_component])
endif

subdir('libpng', components: [plugins_component])
subdir('raw1394', components: [plugins_component])
subdir('pulse', components: [plugins_component])
subdir('shout2', components: [plugins_component])
subdir('soup', components: [plugins_component])
subdir('speex', components: [plugins_component])

# FIXME: taglib fails to compile and link with msvc
if cc.get_id() == 'msvc'
  message('Building with MSVC, disabling taglib support. Patches welcome!')
else
  subdir('taglib', components: [plugins_component])
endif
subdir('twolame',components: [plugins_component])
subdir('vpx', components: [plugins_component])
subdir('wavpack', components: [plugins_component])
