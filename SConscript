# RT-Thread building script for component

from building import *

cwd = GetCurrentDir()
src = Glob('*.c') + Glob('*.cpp') 
CPPPATH = [cwd]
CPPDEFINES = ['LFS_CONFIG=lfs_config.h']

group = DefineGroup('littlefs', src, depend = ['PKG_USING_LITTLEFS', 'RT_USING_DFS'], CPPPATH = CPPPATH, CPPDEFINES = CPPDEFINES)

Return('group')
