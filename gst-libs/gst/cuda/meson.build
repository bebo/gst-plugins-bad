cuda_sources = [
  'gstcudaloader.c',
  'gstcudautils.c',
  'gstcudacontext.c',
  'gstcudamemory.c',
  'gstcudabufferpool.c',
  'gstcudabasefilter.c',
]

cuda_headers = [
  'gstcuda.h',
  'gstcuda_fwd.h',
  'cuda-prelude.h',
  'gstcudautils.h',
  'gstcudacontext.h',
  'gstcudamemory.h',
  'gstcudabasefilter.h',
]

cuda_dep = dependency('', required : false)
cuda_header_dep = dependency('', required : false)
gstcuda_dep = dependency('', required : false)
cuda_incdir = ''

# FIXME: allow cuda without opengl
if not gstgl_dep.found()
  subdir_done()
endif

# CUDA dependency
if host_machine.system() == 'windows'
  # On windows, CUDA_PATH env will be set by installer
  cuda_root = run_command(python3, '-c', 'import os; print(os.environ.get("CUDA_PATH"))').stdout().strip()
  if cuda_root != '' and cuda_root != 'None'
    cuda_incdir = join_paths (cuda_root, 'include')
    if cc.has_header('cuda.h', args: '-I' + cuda_incdir)
      cuda_header_dep = declare_dependency(include_directories: include_directories(cuda_incdir))
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
      if cuda_dep.found()
        cuda_ver = v
      endif
    endif
  endforeach

  if cuda_dep.found()
    if cc.has_header('cuda.h', dependencies: cuda_dep)
      cuda_header_dep = cuda_dep.partial_dependency(compile_args : true, includes : true)
    endif
  endif
endif

if cuda_header_dep.found()
  gstcuda = library('gstcuda-' + api_version,
    cuda_sources,
    c_args : gst_plugins_bad_args + ['-DBUILDING_GST_CUDA'],
    include_directories : [configinc, libsinc],
    version : libversion,
    soversion : soversion,
    install : true,
    dependencies : [gstbase_dep, gstvideo_dep, gstgl_dep, gmodule_dep, cuda_header_dep],
  )

  gstcuda_dep = declare_dependency(link_with: gstcuda,
    include_directories : [libsinc],
    dependencies : [gstbase_dep, gstvideo_dep, gstgl_dep, gmodule_dep, cuda_header_dep])

  install_headers(cuda_headers, subdir : 'gstreamer-1.0/gst/cuda')
endif
