#!/usr/bin/env python
# -*- coding: utf-8 -*-
#
#############################################################################
##
## Copyright (C) 2019 The Qt Company Ltd.
## Contact: https://www.qt.io/licensing/
##
## This file is part of the test suite of Qt for Python.
##
## $QT_BEGIN_LICENSE:GPL-EXCEPT$
## Commercial License Usage
## Licensees holding valid commercial Qt licenses may use this file in
## accordance with the commercial license agreement provided with the
## Software or, alternatively, in accordance with the terms contained in
## a written agreement between you and The Qt Company. For licensing terms
## and conditions see https://www.qt.io/terms-conditions. For further
## information use the contact form at https://www.qt.io/contact-us.
##
## GNU General Public License Usage
## Alternatively, this file may be used under the terms of the GNU
## General Public License version 3 as published by the Free Software
## Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
## included in the packaging of this file. Please review the following
## information to ensure the GNU General Public License requirements will
## be met: https://www.gnu.org/licenses/gpl-3.0.html.
##
## $QT_END_LICENSE$
##
#############################################################################

'''Test cases for renaming using target-lang-name attribute.'''

import os
import re
import sys
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from shiboken_paths import init_paths
init_paths()

from sample import RenamedValue, RenamedUser

from shiboken6 import Shiboken
_init_pyside_extension()   # trigger bootstrap

from shibokensupport.signature import get_signature


class RenamingTest(unittest.TestCase):
    def test(self):
        '''Tests whether the C++ class ToBeRenamedValue renamed via attribute
           target-lang-name to RenamedValue shows up in consuming function
           signature strings correctly.
        '''
        renamed_value = RenamedValue()
        self.assertEqual(str(type(renamed_value)),
                         "<class 'sample.RenamedValue'>")
        rename_user = RenamedUser()
        rename_user.useRenamedValue(renamed_value)
        actual_signature = str(get_signature(rename_user.useRenamedValue))
        self.assertTrue(re.match(r"^\(self,\s*?v:\s*?sample.RenamedValue\)\s*?->\s*?None$",
                                 actual_signature))



if __name__ == '__main__':
    unittest.main()
