project('instrew', ['c', 'cpp'],
        default_options: [
            'buildtype=debugoptimized',
            'default_library=static',
            'warning_level=3',
            'c_std=c11',
            'cpp_std=c++17',
            'b_ndebug=if-release',
        ],
        meson_version: '>=0.52')

if host_machine.endian() != 'little'
  error('cannot compile for non-little-endian machine')
endif

add_project_arguments(['-Wmissing-field-initializers',
                       '-Wunused-parameter',
                       '-Wshadow',
                       '-Wpointer-arith',
                       '-Wwrite-strings',
                       '-Winline',
                       '-Wformat-nonliteral',
                       '-Wformat-security',
                       '-Wswitch-default',
                       '-Wundef',
                       '-Wno-cast-align', # we frequently do casts
                       '-Werror=incompatible-pointer-types',
                       '-Werror=implicit-function-declaration'],
                      language: 'c')

add_global_arguments(['-fno-rtti', '-fno-exceptions'], language: 'cpp')

llvm_version = ['>=16', '<19']
# First, attempt to use the shared library.
libllvm = dependency('llvm', version: llvm_version, static: false,
                     method: 'config-tool', include_type: 'system',
                     required: false)
if not libllvm.found()
  # Try static libraries.
  libllvm = dependency('llvm', version: llvm_version, static: true,
                       method: 'config-tool', include_type: 'system',
                       modules: ['x86', 'aarch64', 'riscv', 'analysis', 'passes'])
endif
add_project_arguments(['-DLL_LLVM_MAJOR='+libllvm.version().split('.')[0]], language: 'cpp')

libcrypto = dependency('libcrypto')

rellume = subproject('rellume')
librellume = rellume.get_variable('librellume')

subdir('client')
subdir('server')
subdir('test')
