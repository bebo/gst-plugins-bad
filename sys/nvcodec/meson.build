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
  'gstcudaloader.c'
]

nvdec_option = get_option('nvdec')
nvenc_option = get_option('nvenc')

cuda_dep = dependency('', required : false)
cudart_dep = dependency('', required : false)
nvcuvid_dep = dependency('', required : false)
cuda_libdir = ''
cuda_incdir = ''
use_nvenc_gl = false
extra_c_args = []

if nvdec_option.disabled() and nvenc_option.disabled()
  subdir_done()
endif

# CUDA dependency
if host_machine.system() == 'windows'
  # On windows, CUDA_PATH env will be set by installer
  cuda_root = run_command(python3, '-c', 'import os; print(os.environ.get("CUDA_PATH"))').stdout().strip()
  if cuda_root != '' and cuda_root != 'None'
    arc = ''
    if build_machine.cpu_family() == 'x86_64'
      arc = 'x64'
    else
      arc = 'Win32'
    endif
    cuda_libdir = join_paths (cuda_root, 'lib', arc)
    cuda_incdir = join_paths (cuda_root, 'include')
    cuda_lib = cc.find_library('cuda', dirs: cuda_libdir, required: false)
    cudart_lib = cc.find_library('cudart', dirs: cuda_libdir, required: false)
    if cuda_lib.found()
      cuda_header_found = cc.has_header('cuda.h', args: '-I' + cuda_incdir)
      cuda_lib_found = cc.has_function('cuInit', dependencies: cuda_lib)
      if cuda_header_found and cuda_lib_found
        cuda_dep = declare_dependency(include_directories: include_directories(cuda_incdir),
                                      dependencies: cuda_lib)
      endif
    endif

    if cudart_lib.found()
      cudart_header_found = cc.has_header('cuda_runtime_api.h', args: '-I' + cuda_incdir)
      cudart_lib_found = cc.has_function('cudaGetErrorString', dependencies: cudart_lib)
      if cudart_header_found and cudart_lib_found
        cudart_dep = declare_dependency(dependencies: cudart_lib)
      endif
    endif
  endif
else
  cuda_versions = [
    '10.1',
    '10.0',
    '9.2',
    '9.1',
    '9.0',
    '8.0',
    '7.5',
    '7.0',
    '6.5',
  ]
  cuda_ver = ''

  # FIXME: use break syntax when we use meson >= '0.49'
  foreach v : cuda_versions
    if cuda_ver == ''
      cuda_dep = dependency('cuda-' + v, required: false)
      cudart_dep = dependency('cudart-' + v, required: false)
      if cuda_dep.found() and cudart_dep.found()
        cuda_ver = v
      endif
    endif
  endforeach

  if cuda_dep.found()
    cuda_header_found = cc.has_header('cuda.h', dependencies: cuda_dep)
    cuda_lib_found = cc.has_function('cuInit', dependencies: cuda_dep)
    if not cuda_header_found or not cuda_lib_found
      message ('Missing required header and/or function in cuda dependency')
      cuda_dep = dependency('', required : false)
    endif
  endif

  if cudart_dep.found()
    cudart_header_found = cc.has_header('cuda_runtime_api.h', dependencies: cudart_dep)
    cudart_lib_found = cc.has_function('cudaGetErrorString', dependencies: cudart_dep)
    if not cudart_header_found or not cudart_lib_found
      message ('Missing required header and/or function in cudart dependency')
      cudart_dep = dependency('', required : false)
    endif
  endif
endif

if not cuda_dep.found() or not cudart_dep.found()
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
  nvcuvid_incdirs = []
  if host_machine.system() == 'windows'
    cuda_libdirs = [cuda_libdir]
    # NOTE: Newer CUDA toolkit versions do not ship with the nvcuvid library, and
    # you must get it from the Nvidia Video Codec SDK. The SDK ships as a zip
    # file, so there's no installer and you have to set this env var yourself.
    video_sdk_root = run_command(python3, '-c', 'import os; print(os.environ.get("NVIDIA_VIDEO_CODEC_SDK_PATH"))').stdout().strip()
    if video_sdk_root != '' and video_sdk_root != 'None'
      cuda_libdirs += [join_paths(video_sdk_root, 'Samples', 'NvCodec', 'Lib', arc)]
      nvcuvid_incdirs = include_directories(join_paths(video_sdk_root, 'Samples', 'NvCodec', 'NvDecoder'))
    endif
    nvcuvid_lib = cc.find_library('nvcuvid', dirs: cuda_libdirs, required: nvdec_option)
  else
    nvcuvid_lib = cc.find_library('nvcuvid', required: nvdec_option)
  endif

  if nvcuvid_lib.found() and cc.has_function('cuvidCtxLock', dependencies: nvcuvid_lib)
    nvcuvid_dep = declare_dependency(dependencies: nvcuvid_lib,
                                    include_directories: nvcuvid_incdirs)

    extra_c_args += ['-DHAVE_NVDEC=1']
    nvcodec_sources += nvdec_sources
  endif

  if nvdec_option.enabled() and not nvcuvid_dep.found()
    error('The nvdec plugin was enabled explicitly, but required nvcuvid library was not found.')
  endif
endif

# NVENC dependency
if not nvenc_option.disabled()
  if host_machine.system() == 'windows'
    if cc.has_header('cuda_gl_interop.h', args: '-I' + cuda_incdir)
      use_nvenc_gl = true
    endif
  else
    if cc.has_header('cuda_gl_interop.h', dependencies: cuda_dep)
      use_nvenc_gl = true
    endif
  endif

  if use_nvenc_gl
    extra_c_args += ['-DHAVE_NVENC_GST_GL=1']
  endif

  extra_c_args += ['-DHAVE_NVENC=1']
  nvcodec_sources += nvenc_sources
endif

gstnvcodec = library('gstnvcodec',
  nvcodec_sources,
  c_args : gst_plugins_bad_args + extra_c_args,
  include_directories : [configinc],
  dependencies : [gstbase_dep, gstvideo_dep, gstpbutils_dep, gstgl_dep, cuda_dep, cudart_dep, nvcuvid_dep, gmodule_dep],
  install : true,
  install_dir : plugins_install_dir,
)
pkgconfig.generate(gstnvcodec, install_dir : plugins_pkgconfig_install_dir)
