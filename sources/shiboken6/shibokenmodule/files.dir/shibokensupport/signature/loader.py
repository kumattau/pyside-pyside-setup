# This Python file uses the following encoding: utf-8
# It has been edited by fix-complaints.py .

#############################################################################
##
## Copyright (C) 2019 The Qt Company Ltd.
## Contact: https://www.qt.io/licensing/
##
## This file is part of Qt for Python.
##
## $QT_BEGIN_LICENSE:LGPL$
## Commercial License Usage
## Licensees holding valid commercial Qt licenses may use this file in
## accordance with the commercial license agreement provided with the
## Software or, alternatively, in accordance with the terms contained in
## a written agreement between you and The Qt Company. For licensing terms
## and conditions see https://www.qt.io/terms-conditions. For further
## information use the contact form at https://www.qt.io/contact-us.
##
## GNU Lesser General Public License Usage
## Alternatively, this file may be used under the terms of the GNU Lesser
## General Public License version 3 as published by the Free Software
## Foundation and appearing in the file LICENSE.LGPL3 included in the
## packaging of this file. Please review the following information to
## ensure the GNU Lesser General Public License version 3 requirements
## will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
##
## GNU General Public License Usage
## Alternatively, this file may be used under the terms of the GNU
## General Public License version 2.0 or (at your option) the GNU General
## Public license version 3 or any later version approved by the KDE Free
## Qt Foundation. The licenses are as published by the Free Software
## Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
## included in the packaging of this file. Please review the following
## information to ensure the GNU General Public License requirements will
## be met: https://www.gnu.org/licenses/gpl-2.0.html and
## https://www.gnu.org/licenses/gpl-3.0.html.
##
## $QT_END_LICENSE$
##
#############################################################################

"""
loader.py

The loader has to load the signature module completely at startup,
to make sure that the functions are available when needed.
This is meanwhile necessary to make the '__doc__' attribute work correctly.

It does not mean that everything is initialized in advance. Only the modules
are loaded completely after 'import PySide6'.

This version uses both a normal directory, but has also an embedded ZIP file
as a fallback solution. The ZIP file is generated by 'embedding_generator.py'
and embedded into 'signature.cpp' as "embed/signature.inc".

Meanwhile, the ZIP file grew so much, that MSVC had problems
with it's 64k string limit, so we had to break the string up.
See 'zipped_string_sequence' in signature.cpp.
"""

import sys
import os
import traceback
import types


# name used in signature.cpp
def pyside_type_init(type_key, sig_strings):
    return parser.pyside_type_init(type_key, sig_strings)

# name used in signature.cpp
def create_signature(props, key):
    return layout.create_signature(props, key)

# name used in signature.cpp
def seterror_argument(args, func_name, info):
    return errorhandler.seterror_argument(args, func_name, info)

# name used in signature.cpp
def make_helptext(func):
    return errorhandler.make_helptext(func)

# name used in signature.cpp
def finish_import(module):
    return importhandler.finish_import(module)

# name used in signature.cpp
def feature_import(*args, **kwds):
    # don't spend a stack level here for speed and compatibility
    global feature_import
    feature_import = __feature__.feature_import
    return feature_import(*args, **kwds)


import builtins
import signature_bootstrap
from shibokensupport import signature, feature as __feature__
signature.get_signature = signature_bootstrap.get_signature
# PYSIDE-1019: Publish the __feature__ dictionary.
__feature__.pyside_feature_dict = signature_bootstrap.pyside_feature_dict
builtins.__feature_import__ = signature_bootstrap.__feature_import__
del signature_bootstrap


def put_into_package(package, module, override=None):
    # take the last component of the module name
    name = (override if override else module.__spec__.name).rsplit(".", 1)[-1]
    # allow access as {package}.{name}
    if package:
        setattr(package, name, module)
    # put into sys.modules as a package to allow all import options
    fullname = f"{package.__spec__.name}.{name}" if package else name
    module.__spec__.name = fullname
    # publish new dotted name in sys.modules
    sys.modules[fullname] = module


def move_into_pyside_package():
    import shibokensupport
    import PySide6
    try:
        import PySide6.support
    except ModuleNotFoundError:
        # This can happen in the embedding case.
        put_into_package(PySide6, shibokensupport, "support")
    put_into_package(PySide6.support, __feature__, "__feature__")
    put_into_package(PySide6.support, signature)
    put_into_package(PySide6.support.signature, mapping)
    put_into_package(PySide6.support.signature, errorhandler)
    put_into_package(PySide6.support.signature, layout)
    put_into_package(PySide6.support.signature, lib)
    put_into_package(PySide6.support.signature, parser)
    put_into_package(PySide6.support.signature, importhandler)
    put_into_package(PySide6.support.signature.lib, enum_sig)
    put_into_package(PySide6.support.signature.lib, pyi_generator)
    put_into_package(PySide6.support.signature.lib, tool)

from shibokensupport.signature import mapping
from shibokensupport.signature import errorhandler
from shibokensupport.signature import layout
from shibokensupport.signature import lib
from shibokensupport.signature import parser
from shibokensupport.signature import importhandler
from shibokensupport.signature.lib import enum_sig
from shibokensupport.signature.lib import pyi_generator
from shibokensupport.signature.lib import tool

if "PySide6" in sys.modules:
    # We publish everything under "PySide6.support", again.
    move_into_pyside_package()
    # PYSIDE-1502: Make sure that support can be imported.
    try:
        import PySide6.support
    except ModuleNotFoundError as e:
        print("PySide6.support could not be imported. "
              "This is a serious configuration error.", file=sys.stderr)
        raise
    # PYSIDE-1019: Modify `__import__` to be `__feature__` aware.
    # __feature__ is already in sys.modules, so this is actually no import
    if not hasattr(sys, "pypy_version_info"):
        # PYSIDE-535: Cannot enable __feature__ for various reasons.
        import PySide6.support.__feature__
        sys.modules["__feature__"] = PySide6.support.__feature__
        builtins.__orig_import__ = builtins.__import__
        builtins.__import__ = builtins.__feature_import__

# end of file
