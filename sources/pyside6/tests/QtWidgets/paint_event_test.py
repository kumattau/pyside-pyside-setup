#############################################################################
##
## Copyright (C) 2021 The Qt Company Ltd.
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

'''Test paint event override in python'''

import gc
import os
import sys
import unittest

from textwrap import dedent

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

from PySide6.QtCore import QTimerEvent
from PySide6.QtWidgets import QApplication, QWidget

from helper.usesqapplication import UsesQApplication


class MyWidget(QWidget):
    '''Sample widget'''

    def __init__(self, app=None):
        # Creates a new widget
        if app is None:
            app = QApplication([])

        super().__init__()
        self.app = app
        self.runs = 0
        self.max_runs = 5
        self.paint_event_called = False

    def timerEvent(self, event):
        # Timer event method
        self.runs += 1

        if self.runs == self.max_runs:
            self.app.quit()

        if not isinstance(event, QTimerEvent):
            raise TypeError('Invalid event type. Must be QTimerEvent')

    def paintEvent(self, event):
        # Empty paint event method
        # XXX: should be using super here, but somehow PyQt4
        # complains about paintEvent not present in super
        QWidget.paintEvent(self, event)
        self.paint_event_called = True


class PaintEventOverride(UsesQApplication):
    '''Test case for overriding QWidget.paintEvent'''

    qapplication = True

    def setUp(self):
        # Acquire resources
        super(PaintEventOverride, self).setUp()
        self.widget = MyWidget(self.app)

    def tearDown(self):
        # Release resources
        del self.widget
        # PYSIDE-535: Need to collect garbage in PyPy to trigger deletion
        gc.collect()
        super(PaintEventOverride, self).tearDown()

    def testPaintEvent(self):
        # Test QWidget.paintEvent override
        timer_id = self.widget.startTimer(100)
        self.widget.show()
        self.app.exec()
        self.widget.killTimer(timer_id)
        self.assertTrue(self.widget.paint_event_called)
        self.assertEqual(self.widget.runs, 5)


if __name__ == '__main__':
    unittest.main()
