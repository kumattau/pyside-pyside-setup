/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt for Python.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "pyside.h"
#include "pysideinit.h"
#include "pysidecleanup.h"
#include "pysideqapp.h"
#include "pysideqobject.h"
#include "pysideutils.h"
#include "pyside_numpy.h"
#include "pyside_p.h"
#include "signalmanager.h"
#include "pysideclassinfo_p.h"
#include "pysideproperty_p.h"
#include "class_property.h"
#include "pysideproperty.h"
#include "pysidesignal.h"
#include "pysidesignal_p.h"
#include "pysidestaticstrings.h"
#include "pysideslot_p.h"
#include "pysidemetafunction_p.h"
#include "pysidemetafunction.h"
#include "dynamicqmetaobject.h"
#include "feature_select.h"

#include <autodecref.h>
#include <basewrapper.h>
#include <bindingmanager.h>
#include <gilstate.h>
#include <sbkconverter.h>
#include <sbkstring.h>
#include <sbkstaticstrings.h>

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSharedPointer>
#include <QtCore/QStack>
#include <QtCore/QThread>

#include <algorithm>
#include <cstring>
#include <cctype>
#include <typeinfo>

static QStack<PySide::CleanupFunction> cleanupFunctionList;
static void *qobjectNextAddr;

QT_BEGIN_NAMESPACE
extern bool qRegisterResourceData(int, const unsigned char *, const unsigned char *,
        const unsigned char *);
QT_END_NAMESPACE

namespace PySide
{

void init(PyObject *module)
{
    qobjectNextAddr = nullptr;
    Numpy::init();
    ClassInfo::init(module);
    Signal::init(module);
    Slot::init(module);
    Property::init(module);
    ClassProperty::init(module);
    MetaFunction::init(module);
    // Init signal manager, so it will register some meta types used by QVariant.
    SignalManager::instance();
    initQApp();
}

static bool _setProperty(PyObject *qObj, PyObject *name, PyObject *value, bool *accept)
{
    QByteArray propName(Shiboken::String::toCString(name));
    propName[0] = std::toupper(propName[0]);
    propName.prepend("set");

    Shiboken::AutoDecRef propSetter(PyObject_GetAttrString(qObj, propName.constData()));
    if (!propSetter.isNull()) {
        *accept = true;
        Shiboken::AutoDecRef args(PyTuple_Pack(1, value));
        Shiboken::AutoDecRef retval(PyObject_CallObject(propSetter, args));
        if (retval.isNull())
            return false;
    } else {
        PyErr_Clear();
        Shiboken::AutoDecRef attr(PyObject_GenericGetAttr(qObj, name));
        if (PySide::Property::checkType(attr)) {
            *accept = true;
            if (PySide::Property::setValue(reinterpret_cast<PySideProperty *>(attr.object()), qObj, value) < 0)
                return false;
        }
    }
    return true;
}

bool fillQtProperties(PyObject *qObj, const QMetaObject *metaObj, PyObject *kwds)
{

    PyObject *key, *value;
    Py_ssize_t pos = 0;

    while (PyDict_Next(kwds, &pos, &key, &value)) {
        QByteArray propName(Shiboken::String::toCString(key));
        bool accept = false;
        if (metaObj->indexOfProperty(propName) != -1) {
            if (!_setProperty(qObj, key, value, &accept))
                return false;
        } else {
            propName.append("()");
            if (metaObj->indexOfSignal(propName) != -1) {
                accept = true;
                propName.prepend('2');
                if (!PySide::Signal::connect(qObj, propName, value))
                    return false;
            }
        }
        if (!accept) {
            // PYSIDE-1019: Allow any existing attribute in the constructor.
            if (!_setProperty(qObj, key, value, &accept))
                return false;
        }
        if (!accept) {
            PyErr_Format(PyExc_AttributeError, "'%s' is not a Qt property or a signal",
                         propName.constData());
            return false;
        }
    }
    return true;
}

void registerCleanupFunction(CleanupFunction func)
{
    cleanupFunctionList.push(func);
}

void runCleanupFunctions()
{
    while (!cleanupFunctionList.isEmpty()) {
        CleanupFunction f = cleanupFunctionList.pop();
        f();
    }
}

static void destructionVisitor(SbkObject *pyObj, void *data)
{
    auto realData = reinterpret_cast<void **>(data);
    auto pyQApp = reinterpret_cast<SbkObject *>(realData[0]);
    auto pyQObjectType = reinterpret_cast<PyTypeObject *>(realData[1]);

    if (pyObj != pyQApp && PyObject_TypeCheck(pyObj, pyQObjectType)) {
        if (Shiboken::Object::hasOwnership(pyObj) && Shiboken::Object::isValid(pyObj, false)) {
            Shiboken::Object::setValidCpp(pyObj, false);

            Py_BEGIN_ALLOW_THREADS
            Shiboken::callCppDestructor<QObject>(Shiboken::Object::cppPointer(pyObj, pyQObjectType));
            Py_END_ALLOW_THREADS
        }
    }

};

void destroyQCoreApplication()
{
    QCoreApplication *app = QCoreApplication::instance();
    if (!app)
        return;
    SignalManager::instance().clear();

    Shiboken::BindingManager &bm = Shiboken::BindingManager::instance();
    SbkObject *pyQApp = bm.retrieveWrapper(app);
    PyTypeObject *pyQObjectType = Shiboken::Conversions::getPythonTypeObject("QObject*");
    assert(pyQObjectType);

    void *data[2] = {pyQApp, pyQObjectType};
    bm.visitAllPyObjects(&destructionVisitor, &data);

    // in the end destroy app
    // Allow threads because the destructor calls
    // QThreadPool::globalInstance().waitForDone() which may deadlock on the GIL
    // if there is a worker working with python objects.
    Py_BEGIN_ALLOW_THREADS
    delete app;
    Py_END_ALLOW_THREADS
    // PYSIDE-571: make sure to create a singleton deleted qApp.
    Py_DECREF(MakeQAppWrapper(nullptr));
}

std::size_t getSizeOfQObject(PyTypeObject *type)
{
    return retrieveTypeUserData(type)->cppObjSize;
}

void initDynamicMetaObject(PyTypeObject *type, const QMetaObject *base, std::size_t cppObjSize)
{
    //create DynamicMetaObject based on python type
    auto userData = new TypeUserData(reinterpret_cast<PyTypeObject *>(type), base, cppObjSize);
    userData->mo.update();
    Shiboken::ObjectType::setTypeUserData(type, userData, Shiboken::callCppDestructor<TypeUserData>);

    //initialize staticQMetaObject property
    void *metaObjectPtr = const_cast<QMetaObject *>(userData->mo.update());
    static SbkConverter *converter = Shiboken::Conversions::getConverter("QMetaObject");
    if (!converter)
        return;
    Shiboken::AutoDecRef pyMetaObject(Shiboken::Conversions::pointerToPython(converter, metaObjectPtr));
    PyObject_SetAttr(reinterpret_cast<PyObject *>(type),
                     PySide::PyName::qtStaticMetaObject(), pyMetaObject);
}

TypeUserData *retrieveTypeUserData(PyTypeObject *pyTypeObj)
{
    return reinterpret_cast<TypeUserData *>(Shiboken::ObjectType::getTypeUserData(pyTypeObj));
}

TypeUserData *retrieveTypeUserData(PyObject *pyObj)
{
    auto pyTypeObj = PyType_Check(pyObj)
        ? reinterpret_cast<PyTypeObject *>(pyObj) : Py_TYPE(pyObj);
    return retrieveTypeUserData(pyTypeObj);
}

const QMetaObject *retrieveMetaObject(PyTypeObject *pyTypeObj)
{
    TypeUserData *userData = retrieveTypeUserData(pyTypeObj);
    return userData ? userData->mo.update() : nullptr;
}

const QMetaObject *retrieveMetaObject(PyObject *pyObj)
{
    auto pyTypeObj = PyType_Check(pyObj)
        ? reinterpret_cast<PyTypeObject *>(pyObj) : Py_TYPE(pyObj);
    return retrieveMetaObject(pyTypeObj);
}

void initQObjectSubType(PyTypeObject *type, PyObject *args, PyObject * /* kwds */)
{
    PyTypeObject *qObjType = Shiboken::Conversions::getPythonTypeObject("QObject*");
    QByteArray className(Shiboken::String::toCString(PyTuple_GET_ITEM(args, 0)));

    PyObject *bases = PyTuple_GET_ITEM(args, 1);
    int numBases = PyTuple_GET_SIZE(bases);

    TypeUserData *userData = nullptr;

    for (int i = 0; i < numBases; ++i) {
        auto base = reinterpret_cast<PyTypeObject *>(PyTuple_GET_ITEM(bases, i));
        if (PyType_IsSubtype(base, qObjType)) {
            userData = retrieveTypeUserData(base);
            break;
        }
    }
    if (!userData) {
        qWarning("Sub class of QObject not inheriting QObject!? Crash will happen when using %s.", className.constData());
        return;
    }
    // PYSIDE-1463: Don't change feature selection durin subtype initialization.
    // This behavior is observed with PySide 6.
    PySide::Feature::Enable(false);
    initDynamicMetaObject(type, userData->mo.update(), userData->cppObjSize);
    PySide::Feature::Enable(true);
}

void initQApp()
{
    /*
     * qApp will not be initialized when embedding is active.
     * That means that qApp exists already when PySide is initialized.
     * We could solve that by creating a qApp variable, but in embedded
     * mode, we also have the effect that the first assignment to qApp
     * is persistent! Therefore, we can never be sure to have created
     * qApp late enough to get the right type for the instance.
     *
     * I would appreciate very much if someone could explain or even fix
     * this issue. It exists only when a pre-existing application exists.
     */
    if (!qApp)
        Py_DECREF(MakeQAppWrapper(nullptr));

    // PYSIDE-1470: Register a function to destroy an application from shiboken.
    setDestroyQApplication(destroyQCoreApplication);
}

PyObject *getMetaDataFromQObject(QObject *cppSelf, PyObject *self, PyObject *name)
{
    PyObject *attr = PyObject_GenericGetAttr(self, name);
    if (!Shiboken::Object::isValid(reinterpret_cast<SbkObject *>(self), false))
        return attr;

    if (attr && Property::checkType(attr)) {
        PyObject *value = Property::getValue(reinterpret_cast<PySideProperty *>(attr), self);
        Py_DECREF(attr);
        if (!value)
            return nullptr;
        attr = value;
    }

    //mutate native signals to signal instance type
    if (attr && PyObject_TypeCheck(attr, PySideSignalTypeF())) {
        PyObject *signal = reinterpret_cast<PyObject *>(Signal::initialize(reinterpret_cast<PySideSignal *>(attr), name, self));
        PyObject_SetAttr(self, name, reinterpret_cast<PyObject *>(signal));
        return signal;
    }

    //search on metaobject (avoid internal attributes started with '__')
    if (!attr) {
        const char *cname = Shiboken::String::toCString(name);
        uint cnameLen = qstrlen(cname);
        if (std::strncmp("__", cname, 2)) {
            const QMetaObject *metaObject = cppSelf->metaObject();
            //signal
            QList<QMetaMethod> signalList;
            for(int i=0, i_max = metaObject->methodCount(); i < i_max; i++) {
                QMetaMethod method = metaObject->method(i);
                const QByteArray methSig_ = method.methodSignature();
                const char *methSig = methSig_.constData();
                bool methMacth = !std::strncmp(cname, methSig, cnameLen) && methSig[cnameLen] == '(';
                if (methMacth) {
                    if (method.methodType() == QMetaMethod::Signal) {
                        signalList.append(method);
                    } else {
                        PySideMetaFunction *func = MetaFunction::newObject(cppSelf, i);
                        if (func) {
                            PyObject *result = reinterpret_cast<PyObject *>(func);
                            PyObject_SetAttr(self, name, result);
                            return result;
                        }
                    }
                }
            }
            if (!signalList.isEmpty()) {
                PyObject *pySignal = reinterpret_cast<PyObject *>(Signal::newObjectFromMethod(self, signalList));
                PyObject_SetAttr(self, name, pySignal);
                return pySignal;
            }
        }
    }
    return attr;
}

bool inherits(PyTypeObject *objType, const char *class_name)
{
    if (strcmp(objType->tp_name, class_name) == 0)
        return true;

    PyTypeObject *base = objType->tp_base;
    if (base == nullptr)
        return false;

    return inherits(base, class_name);
}

void *nextQObjectMemoryAddr()
{
    return qobjectNextAddr;
}

void setNextQObjectMemoryAddr(void *addr)
{
    qobjectNextAddr = addr;
}

} // namespace PySide

// A QSharedPointer is used with a deletion function to invalidate a pointer
// when the property value is cleared.  This should be a QSharedPointer with
// a void *pointer, but that isn't allowed
typedef char any_t;
Q_DECLARE_METATYPE(QSharedPointer<any_t>);

namespace PySide
{

static void invalidatePtr(any_t *object)
{
    Shiboken::GilState state;

    SbkObject *wrapper = Shiboken::BindingManager::instance().retrieveWrapper(object);
    if (wrapper != nullptr)
        Shiboken::BindingManager::instance().releaseWrapper(wrapper);
}

static const char invalidatePropertyName[] = "_PySideInvalidatePtr";

// PYSIDE-1214, when creating new wrappers for classes inheriting QObject but
// not exposed to Python, try to find the best-matching (most-derived) Qt
// class by walking up the meta objects.
static const char *typeName(const QObject *cppSelf)
{
    const char *typeName = typeid(*cppSelf).name();
    if (!Shiboken::Conversions::getConverter(typeName)) {
        for (auto metaObject = cppSelf->metaObject(); metaObject; metaObject = metaObject->superClass()) {
            const char *name = metaObject->className();
            if (Shiboken::Conversions::getConverter(name)) {
                typeName = name;
                break;
            }
        }
    }
    return typeName;
}

PyTypeObject *getTypeForQObject(const QObject *cppSelf)
{
    // First check if there are any instances of Python implementations
    // inheriting a PySide class.
    auto *existing = Shiboken::BindingManager::instance().retrieveWrapper(cppSelf);
    if (existing != nullptr)
        return reinterpret_cast<PyObject *>(existing)->ob_type;
    // Find the best match (will return a PySide type)
    auto *sbkObjectType = Shiboken::ObjectType::typeForTypeName(typeName(cppSelf));
    if (sbkObjectType != nullptr)
        return reinterpret_cast<PyTypeObject *>(sbkObjectType);
    return nullptr;
}

PyObject *getWrapperForQObject(QObject *cppSelf, PyTypeObject *sbk_type)
{
    PyObject *pyOut = reinterpret_cast<PyObject *>(Shiboken::BindingManager::instance().retrieveWrapper(cppSelf));
    if (pyOut) {
        Py_INCREF(pyOut);
        return pyOut;
    }

    // Setting the property will trigger an QEvent notification, which may call into
    // code that creates the wrapper so only set the property if it isn't already
    // set and check if it's created after the set call
    QVariant existing = cppSelf->property(invalidatePropertyName);
    if (!existing.isValid()) {
        if (cppSelf->thread() == QThread::currentThread()) {
            QSharedPointer<any_t> shared_with_del(reinterpret_cast<any_t *>(cppSelf), invalidatePtr);
            cppSelf->setProperty(invalidatePropertyName, QVariant::fromValue(shared_with_del));
        }
        pyOut = reinterpret_cast<PyObject *>(Shiboken::BindingManager::instance().retrieveWrapper(cppSelf));
        if (pyOut) {
            Py_INCREF(pyOut);
            return pyOut;
        }
    }

    pyOut = Shiboken::Object::newObject(sbk_type, cppSelf, false, false, typeName(cppSelf));

    return pyOut;
}

static QuickRegisterItemFunction quickRegisterItem;

QuickRegisterItemFunction getQuickRegisterItemFunction()
{
    return quickRegisterItem;
}

void setQuickRegisterItemFunction(QuickRegisterItemFunction function)
{
    quickRegisterItem = function;
}

// Inspired by Shiboken::String::toCString;
QString pyStringToQString(PyObject *str)
{
    if (str == Py_None)
        return QString();

    if (PyUnicode_Check(str)) {
        const char *unicodeBuffer = _PepUnicode_AsString(str);
        if (unicodeBuffer)
            return QString::fromUtf8(unicodeBuffer);
    }
    if (PyBytes_Check(str)) {
        const char *asciiBuffer = PyBytes_AS_STRING(str);
        if (asciiBuffer)
            return QString::fromLatin1(asciiBuffer);
    }
    return QString();
}

// PySide-1499: Provide an efficient, correct PathLike interface
QString pyPathToQString(PyObject *path)
{
    // For empty constructors path can be nullptr
    // fallback to an empty QString in that case.
    if (!path)
        return QString();

    // str or bytes pass through
    if (PyUnicode_Check(path) || PyBytes_Check(path))
        return pyStringToQString(path);

    // Let PyOS_FSPath do its work and then fix the result for Windows.
    Shiboken::AutoDecRef strPath(PyOS_FSPath(path));
    if (strPath.isNull())
        return QString();
    return QDir::fromNativeSeparators(pyStringToQString(strPath));
}

static const unsigned char qt_resource_name[] = {
  // qt
  0x0,0x2,
  0x0,0x0,0x7,0x84,
  0x0,0x71,
  0x0,0x74,
    // etc
  0x0,0x3,
  0x0,0x0,0x6c,0xa3,
  0x0,0x65,
  0x0,0x74,0x0,0x63,
    // qt.conf
  0x0,0x7,
  0x8,0x74,0xa6,0xa6,
  0x0,0x71,
  0x0,0x74,0x0,0x2e,0x0,0x63,0x0,0x6f,0x0,0x6e,0x0,0x66
};

static const unsigned char qt_resource_struct[] = {
  // :
  0x0,0x0,0x0,0x0,0x0,0x2,0x0,0x0,0x0,0x1,0x0,0x0,0x0,0x1,
  // :/qt
  0x0,0x0,0x0,0x0,0x0,0x2,0x0,0x0,0x0,0x1,0x0,0x0,0x0,0x2,
  // :/qt/etc
  0x0,0x0,0x0,0xa,0x0,0x2,0x0,0x0,0x0,0x1,0x0,0x0,0x0,0x3,
  // :/qt/etc/qt.conf
  0x0,0x0,0x0,0x16,0x0,0x0,0x0,0x0,0x0,0x1,0x0,0x0,0x0,0x0
};

bool registerInternalQtConf()
{
    // Guard to ensure single registration.
#ifdef PYSIDE_QT_CONF_PREFIX
        static bool registrationAttempted = false;
#else
        static bool registrationAttempted = true;
#endif
    static bool isRegistered = false;
    if (registrationAttempted)
        return isRegistered;
    registrationAttempted = true;

    // Support PyInstaller case when a qt.conf file might be provided next to the generated
    // PyInstaller executable.
    // This will disable the internal qt.conf which points to the PySide6 subdirectory (due to the
    // subdirectory not existing anymore).
#ifndef PYPY_VERSION
    QString executablePath =
    QString::fromWCharArray(Py_GetProgramFullPath());
#else
    // PYSIDE-535: FIXME: Add this function when available.
    QString executablePath = QLatin1String("missing Py_GetProgramFullPath");
#endif // PYPY_VERSION
    QString appDirPath = QFileInfo(executablePath).absolutePath();
    QString maybeQtConfPath = QDir(appDirPath).filePath(QStringLiteral("qt.conf"));
    bool executableQtConfAvailable = QFileInfo::exists(maybeQtConfPath);
    maybeQtConfPath = QDir::toNativeSeparators(maybeQtConfPath);

    // Allow disabling the usage of the internal qt.conf. This is necessary for tests to work,
    // because tests are executed before the package is installed, and thus the Prefix specified
    // in qt.conf would point to a not yet existing location.
    bool disableInternalQtConf =
            qEnvironmentVariableIntValue("PYSIDE_DISABLE_INTERNAL_QT_CONF") > 0;
    if (disableInternalQtConf || executableQtConfAvailable) {
        registrationAttempted = true;
        return false;
    }

    PyObject *pysideModule = PyImport_ImportModule("PySide6");
    if (!pysideModule)
        return false;

    // Querying __file__ should be done only for modules that have finished their initialization.
    // Thus querying for the top-level PySide6 package works for us whenever any Qt-wrapped module
    // is loaded.
    PyObject *pysideInitFilePath = PyObject_GetAttr(pysideModule, Shiboken::PyMagicName::file());
    Py_DECREF(pysideModule);
    if (!pysideInitFilePath)
        return false;

    QString initPath = pyStringToQString(pysideInitFilePath);
    Py_DECREF(pysideInitFilePath);
    if (initPath.isEmpty())
        return false;

    // pysideDir - absolute path to the directory containing the init file, which also contains
    // the rest of the PySide6 modules.
    // prefixPath - absolute path to the directory containing the installed Qt (prefix).
    QDir pysideDir = QFileInfo(QDir::fromNativeSeparators(initPath)).absoluteDir();
    QString setupPrefix;
#ifdef PYSIDE_QT_CONF_PREFIX
    setupPrefix = QStringLiteral(PYSIDE_QT_CONF_PREFIX);
#endif
    const QString prefixPathStr = pysideDir.absoluteFilePath(setupPrefix);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QByteArray prefixPath = prefixPathStr.toLocal8Bit();
#else
    // PYSIDE-972, QSettings used by QtCore uses Latin1
    const QByteArray prefixPath = prefixPathStr.toLatin1();
#endif

    // rccData needs to be static, otherwise when it goes out of scope, the Qt resource system
    // will point to invalid memory.
    static QByteArray rccData = QByteArrayLiteral("[Paths]\nPrefix = ") + prefixPath
#ifdef Q_OS_WIN
            // LibraryExecutables needs to point to Prefix instead of ./bin because we don't
            // currently conform to the Qt default directory layout on Windows. This is necessary
            // for QtWebEngineCore to find the location of QtWebEngineProcess.exe.
            + QByteArray("\nLibraryExecutables = ") + prefixPath
#endif
            ;
    rccData.append('\n');

    // The RCC data structure expects a 4-byte size value representing the actual data.
    int size = rccData.size();

    for (int i = 0; i < 4; ++i) {
        rccData.prepend((size & 0xff));
        size >>= 8;
    }

    const int version = 0x01;
    isRegistered = qRegisterResourceData(version, qt_resource_struct, qt_resource_name,
                                         reinterpret_cast<const unsigned char *>(
                                             rccData.constData()));

    return isRegistered;
}

static PyTypeObject *qobjectType()
{
    static PyTypeObject * const result = Shiboken::Conversions::getPythonTypeObject("QObject*");
    return result;
}

bool isQObjectDerived(PyTypeObject *pyType, bool raiseError)
{
    const bool result = PyType_IsSubtype(pyType, qobjectType());
    if (!result && raiseError) {
        PyErr_Format(PyExc_TypeError, "A type inherited from %s expected, got %s.",
                     qobjectType()->tp_name, pyType->tp_name);
    }
    return result;
}

QObject *convertToQObject(PyObject *object, bool raiseError)
{
    if (object == nullptr) {
        if (raiseError)
            PyErr_Format(PyExc_TypeError, "None passed for QObject");
        return nullptr;
    }

    if (!isQObjectDerived(Py_TYPE(object), raiseError))
        return nullptr;

    auto *sbkObject = reinterpret_cast<SbkObject*>(object);
    auto *ptr = Shiboken::Object::cppPointer(sbkObject, qobjectType());
    if (ptr == nullptr) {
        if (raiseError) {
            PyErr_Format(PyExc_TypeError, "Conversion of %s to QObject failed.",
                         Py_TYPE(object)->tp_name);
        }
        return nullptr;
    }
    return reinterpret_cast<QObject*>(ptr);
}

} //namespace PySide

