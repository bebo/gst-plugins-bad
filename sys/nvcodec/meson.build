nvdec_sources = [
  'gstnvdec.c',
  'gstcuvidloader.c',
]

nvenc_sources = [
  'gstnvbaseenc.c',
  'gstnvenc.c',
  'gstnvh264enc.c',
  'gstnvh265enc.c',
]

nvcodec_sources = [
  'plugin.c',
]

nvdec_option = get_option('nvdec')
nvenc_option = get_option('nvenc')

use_nvenc_gl = false
extra_c_args = []
have_nvenc = false
have_nvdec = false

if nvdec_option.disabled() and nvenc_option.disabled()
  subdir_done()
endif

if not gstcuda_dep.found()
  if nvdec_option.enabled()
    error('The nvdec plugin was enabled explicitly, but required CUDA library was not found.')
  endif

  if nvenc_option.enabled()
    error('The nvenc plugin was enabled explicitly, but required CUDA library was not found.')
  endif

  subdir_done()
endif

# NVDEC dependency
if not nvdec_option.disabled()
  extra_c_args += ['-DHAVE_NVDEC=1']
  nvcodec_sources += nvdec_sources
  have_nvdec = true
endif

# NVENC dependency
if not nvenc_option.disabled()
  if host_machine.system() == 'windows'
    if cc.has_header('cuda_gl_interop.h', args: '-I' + cuda_incdir)
      use_nvenc_gl = true
    endif
  else
    if cc.has_header('cuda_gl_interop.h', dependencies: cuda_header_dep)
      use_nvenc_gl = true
    endif
  endif

  if use_nvenc_gl
    extra_c_args += ['-DHAVE_NVENC_GST_GL=1']
  endif

  extra_c_args += ['-DHAVE_NVENC=1']
  nvcodec_sources += nvenc_sources
  have_nvenc = true
endif

gstnvcodec = library('gstnvcodec',
  nvcodec_sources,
  c_args : gst_plugins_bad_args + extra_c_args,
  include_directories : [configinc],
  dependencies : [gstbase_dep, gstvideo_dep, gstpbutils_dep, gstcuda_dep],
  install : true,
  install_dir : plugins_install_dir,
)
pkgconfig.generate(gstnvcodec, install_dir : plugins_pkgconfig_install_dir)
