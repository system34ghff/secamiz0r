project('secamiz0r', 'c', version: '0.3.0', license: 'GPL-3.0')

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: false)
gavl_dep = dependency('gavl')



shared_library('secamiz0r',
    [
        'secamiz0r.c',
        'third-party/pcg-c-basic/pcg_basic.c'
    ],
    name_prefix: '',
    dependencies: [ m_dep, gavl_dep ],
    include_directories: include_directories('third-party'),
    install: true,
    install_dir: '/'
)
