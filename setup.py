has_setuptools = False
try:
	from setuptools import setup, Extension
	has_setuptools = True
except ImportError:
	from distutils.core import setup, Extension

import sys
import os
import string
import time

version = '0.1.0'

kwargs = dict()
if has_setuptools:
	kwargs = dict(
			include_package_data = True,
			install_requires = ['setuptools'],
			zip_safe = False)

setup(
	name = 'jackpatch',
	version = version,
	description = 'JACK MIDI and patchbay bindings for Python',
	author = 'Jesse Crossen',
	author_email = 'jesse.crossen@gmail.com',
	url = 'http://github.com/jessecrossen/impulse/jackpatch',
	license = 'WTFPL',
	ext_modules = [
		Extension('jackpatch', 
		  [ 'jackpatch.c' ],
		  libraries=['jack'],
		  include_dirs=['/usr/local/include/'])
	],
	data_files = [ ( 'share/jack-'+version, [ ] ) ],
	platforms = ['linux', 'freebsd'],
	long_description='''jackpatch provides Python bindings for the MIDI and patchbay functionality of the JACK audio connection kit.''',
	classifiers=[ "Development Status :: 3 - Alpha",
            "Topic :: Multimedia :: Sound/Audio :: MIDI" ],
	package_dir = {'': '.',},
	**kwargs
)
