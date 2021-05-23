project('mcwm', 'c')

mcwm_src = [
  'src/list.c',
  'src/mcwm.c'
]

inc = include_directories('include')

xcb = dependency('xcb')
xcb_util = dependency('xcb-util')
xcb_randr = dependency('xcb-randr')
xcb_keysyms = dependency('xcb-keysyms')
xcb_icccm = dependency('xcb-icccm')

mcwm = executable(
  'mcwm',
  mcwm_src,
  c_args : ['-Wextra', '-Werror', '-std=c99', '-pedantic'],
  include_directories : [inc],
  dependencies: [xcb, xcb_util, xcb_randr, xcb_keysyms, xcb_icccm]
)
