#############################################################################
##
## Copyright (C) 2016 The Qt Company Ltd.
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

import os
import sys
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

from helper.timedqapplication import TimedQApplication
from PySide6.QtGui import QIcon


class QIconCtorWithNoneTest(TimedQApplication):
    '''Test made by seblin, see Bug #944: http://bugs.pyside.org/show_bug.cgi?id=944'''

    def testQIconCtorWithNone(self):
        icon = QIcon(None)
        pixmap = icon.pixmap(48, 48)
        self.app.exec()


PIX_PATH = os.fspath(Path(__file__).resolve().parents[2]
                     / "doc/tutorials/basictutorial/icons.png")

class QIconAddPixmapTest(TimedQApplication):
    '''PYSIDE-1669: check that addPixmap works'''

    def testQIconSetPixmap(self):
        icon = QIcon()
        icon.addPixmap(PIX_PATH)
        sizes = icon.availableSizes()
        self.assertTrue(sizes)

    def testQIconSetPixmapPathlike(self):
        icon = QIcon()
        pix_path = Path(PIX_PATH)
        icon.addPixmap(pix_path)
        sizes = icon.availableSizes()
        self.assertTrue(sizes)


if __name__ == '__main__':
    unittest.main()
