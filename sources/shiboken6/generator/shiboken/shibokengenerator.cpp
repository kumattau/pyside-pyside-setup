/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt for Python.
**
** $QT_BEGIN_LICENSE:GPL-EXCEPT$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "shibokengenerator.h"
#include "apiextractorresult.h"
#include "ctypenames.h"
#include <abstractmetabuilder.h>
#include <abstractmetaenum.h>
#include <abstractmetafield.h>
#include <abstractmetafunction.h>
#include <abstractmetalang.h>
#include <abstractmetalang_helpers.h>
#include <usingmember.h>
#include <exception.h>
#include <messages.h>
#include <modifications.h>
#include "overloaddata.h"
#include "propertyspec.h"
#include "pytypenames.h"
#include <reporthandler.h>
#include <textstream.h>
#include <typedatabase.h>
#include <abstractmetabuilder.h>
#include <iostream>

#include <QtCore/QDir>
#include <QtCore/QDebug>
#include <QtCore/QRegularExpression>
#include <limits>
#include <memory>

static const char AVOID_PROTECTED_HACK[] = "avoid-protected-hack";
static const char PARENT_CTOR_HEURISTIC[] = "enable-parent-ctor-heuristic";
static const char RETURN_VALUE_HEURISTIC[] = "enable-return-value-heuristic";
static const char DISABLE_VERBOSE_ERROR_MESSAGES[] = "disable-verbose-error-messages";
static const char USE_ISNULL_AS_NB_NONZERO[] = "use-isnull-as-nb_nonzero";
static const char USE_OPERATOR_BOOL_AS_NB_NONZERO[] = "use-operator-bool-as-nb_nonzero";
static const char WRAPPER_DIAGNOSTICS[] = "wrapper-diagnostics";
static const char NO_IMPLICIT_CONVERSIONS[] = "no-implicit-conversions";

const char *CPP_ARG = "cppArg";
const char *CPP_ARG_REMOVED = "removed_cppArg";
const char *CPP_RETURN_VAR = "cppResult";
const char *CPP_SELF_VAR = "cppSelf";
const char *NULL_PTR = "nullptr";
const char *PYTHON_ARG = "pyArg";
const char *PYTHON_ARGS = "pyArgs";
const char *PYTHON_OVERRIDE_VAR = "pyOverride";
const char *PYTHON_RETURN_VAR = "pyResult";
const char *PYTHON_TO_CPP_VAR = "pythonToCpp";
const char *SMART_POINTER_GETTER = "kSmartPointerGetter";

const char *CONV_RULE_OUT_VAR_SUFFIX = "_out";
const char *BEGIN_ALLOW_THREADS =
    "PyThreadState *_save = PyEval_SaveThread(); // Py_BEGIN_ALLOW_THREADS";
const char *END_ALLOW_THREADS = "PyEval_RestoreThread(_save); // Py_END_ALLOW_THREADS";

struct GeneratorClassInfoCacheEntry
{
    ShibokenGenerator::FunctionGroups functionGroups;
    bool needsGetattroFunction = false;
};

using GeneratorClassInfoCache = QHash<const AbstractMetaClass *, GeneratorClassInfoCacheEntry>;

Q_GLOBAL_STATIC(GeneratorClassInfoCache, generatorClassInfoCache)

static const char CHECKTYPE_REGEX[] = R"(%CHECKTYPE\[([^\[]*)\]\()";
static const char ISCONVERTIBLE_REGEX[] = R"(%ISCONVERTIBLE\[([^\[]*)\]\()";
static const char CONVERTTOPYTHON_REGEX[] = R"(%CONVERTTOPYTHON\[([^\[]*)\]\()";
// Capture a '*' leading the variable name into the target
// so that "*valuePtr = %CONVERTTOCPP..." works as expected.
static const char CONVERTTOCPP_REGEX[] =
    R"((\*?%?[a-zA-Z_][\w\.]*(?:\[[^\[^<^>]+\])*)(?:\s+)=(?:\s+)%CONVERTTOCPP\[([^\[]*)\]\()";

const ShibokenGenerator::TypeSystemConverterRegExps &
    ShibokenGenerator::typeSystemConvRegExps()
{
    static const TypeSystemConverterRegExps result = {
        QRegularExpression(QLatin1String(CHECKTYPE_REGEX)),
        QRegularExpression(QLatin1String(ISCONVERTIBLE_REGEX)),
        QRegularExpression(QLatin1String(CONVERTTOCPP_REGEX)),
        QRegularExpression(QLatin1String(CONVERTTOPYTHON_REGEX))
    };
    return result;
}

ShibokenGenerator::ShibokenGenerator() = default;

ShibokenGenerator::~ShibokenGenerator() = default;

// Correspondences between primitive and Python types.
static const QHash<QString, QString> &primitiveTypesCorrespondences()
{
    static const QHash<QString, QString> result = {
        {QLatin1String("bool"), pyBoolT()},
        {QLatin1String("char"), sbkCharT()},
        {QLatin1String("signed char"), sbkCharT()},
        {QLatin1String("unsigned char"), sbkCharT()},
        {intT(), pyIntT()},
        {QLatin1String("signed int"), pyIntT()},
        {QLatin1String("uint"), pyIntT()},
        {QLatin1String("unsigned int"), pyIntT()},
        {shortT(), pyIntT()},
        {QLatin1String("ushort"), pyIntT()},
        {QLatin1String("signed short"), pyIntT()},
        {QLatin1String("signed short int"), pyIntT()},
        {unsignedShortT(), pyIntT()},
        {QLatin1String("unsigned short int"), pyIntT()},
        {longT(), pyIntT()},
        {doubleT(), pyFloatT()},
        {floatT(), pyFloatT()},
        {QLatin1String("unsigned long"), pyLongT()},
        {QLatin1String("signed long"), pyLongT()},
        {QLatin1String("ulong"), pyLongT()},
        {QLatin1String("unsigned long int"), pyLongT()},
        {QLatin1String("long long"), pyLongT()},
        {QLatin1String("__int64"), pyLongT()},
        {QLatin1String("unsigned long long"), pyLongT()},
        {QLatin1String("unsigned __int64"), pyLongT()},
        {QLatin1String("size_t"), pyLongT()}
    };
    return result;
}

// Format units for C++->Python->C++ conversion
const QHash<QString, QString> &ShibokenGenerator::formatUnits()
{
    static const QHash<QString, QString> result = {
        {QLatin1String("char"), QLatin1String("b")},
        {QLatin1String("unsigned char"), QLatin1String("B")},
        {intT(), QLatin1String("i")},
        {QLatin1String("unsigned int"), QLatin1String("I")},
        {shortT(), QLatin1String("h")},
        {unsignedShortT(), QLatin1String("H")},
        {longT(), QLatin1String("l")},
        {unsignedLongLongT(), QLatin1String("k")},
        {longLongT(), QLatin1String("L")},
        {QLatin1String("__int64"), QLatin1String("L")},
        {unsignedLongLongT(), QLatin1String("K")},
        {QLatin1String("unsigned __int64"), QLatin1String("K")},
        {doubleT(), QLatin1String("d")},
        {floatT(), QLatin1String("f")},
    };
    return result;
}

QString ShibokenGenerator::translateTypeForWrapperMethod(const AbstractMetaType &cType,
                                                         const AbstractMetaClass *context,
                                                         Options options) const
{
    if (cType.isArray())
        return translateTypeForWrapperMethod(*cType.arrayElementType(), context, options) + QLatin1String("[]");

    if (avoidProtectedHack() && cType.isEnum()) {
        auto metaEnum = api().findAbstractMetaEnum(cType.typeEntry());
        if (metaEnum && metaEnum->isProtected())
            return protectedEnumSurrogateName(metaEnum.value());
    }

    return translateType(cType, context, options);
}

bool ShibokenGenerator::shouldGenerateCppWrapper(const AbstractMetaClass *metaClass) const
{
    const auto wrapper = metaClass->cppWrapper();
    return wrapper.testFlag(AbstractMetaClass::CppVirtualMethodWrapper)
        || (avoidProtectedHack()
            && wrapper.testFlag(AbstractMetaClass::CppProtectedHackWrapper));
}

bool ShibokenGenerator::shouldWriteVirtualMethodNative(const AbstractMetaFunctionCPtr &func) const
{
    // PYSIDE-803: Extracted this because it is used multiple times.
    const AbstractMetaClass *metaClass = func->ownerClass();
    return (!avoidProtectedHack() || !metaClass->hasPrivateDestructor())
            && ((func->isVirtual() || func->isAbstract())
                 && !func->attributes().testFlag(AbstractMetaFunction::FinalCppMethod));
}

AbstractMetaFunctionCList ShibokenGenerator::implicitConversions(const TypeEntry *t) const
{
    if (!generateImplicitConversions())
        return {};
    auto *customConversion = t->customConversion();
    if (customConversion && customConversion->replaceOriginalTargetToNativeConversions())
        return {};

    auto result = api().implicitConversions(t);
    auto end = std::remove_if(result.begin(), result.end(),
                              [](const AbstractMetaFunctionCPtr &f) {
                                  return f->isUserAdded();
                              });
    result.erase(end, result.end());
    return result;
}

QString ShibokenGenerator::wrapperName(const AbstractMetaClass *metaClass) const
{
    Q_ASSERT(shouldGenerateCppWrapper(metaClass));
    QString result = metaClass->name();
    if (metaClass->enclosingClass()) // is a inner class
        result.replace(QLatin1String("::"), QLatin1String("_"));
    return result + QLatin1String("Wrapper");
}

QString ShibokenGenerator::fullPythonClassName(const AbstractMetaClass *metaClass)
{
    QString fullClassName = metaClass->name();
    const AbstractMetaClass *enclosing = metaClass->enclosingClass();
    while (enclosing) {
        if (NamespaceTypeEntry::isVisibleScope(enclosing->typeEntry()))
            fullClassName.prepend(enclosing->name() + QLatin1Char('.'));
        enclosing = enclosing->enclosingClass();
    }
    fullClassName.prepend(packageName() + QLatin1Char('.'));
    return fullClassName;
}

QString ShibokenGenerator::fullPythonFunctionName(const AbstractMetaFunctionCPtr &func, bool forceFunc)
{
    QString funcName;
    if (func->isOperatorOverload())
        funcName = ShibokenGenerator::pythonOperatorFunctionName(func);
    else
       funcName = func->name();
    if (func->ownerClass()) {
        QString fullClassName = fullPythonClassName(func->ownerClass());
        if (func->isConstructor()) {
            funcName = fullClassName;
            if (forceFunc)
                funcName.append(QLatin1String(".__init__"));
        }
        else {
            funcName.prepend(fullClassName + QLatin1Char('.'));
        }
    }
    else {
        funcName = packageName() + QLatin1Char('.') + func->name();
    }
    return funcName;
}

QString ShibokenGenerator::protectedEnumSurrogateName(const AbstractMetaEnum &metaEnum)
{
    QString result = metaEnum.fullName();
    result.replace(QLatin1Char('.'), QLatin1Char('_'));
    result.replace(QLatin1String("::"), QLatin1String("_"));
    return result + QLatin1String("_Surrogate");
}

QString ShibokenGenerator::cpythonFunctionName(const AbstractMetaFunctionCPtr &func)
{
    QString result;

    // PYSIDE-331: For inherited functions, we need to find the same labels.
    // Therefore we use the implementing class.
    if (func->implementingClass()) {
        result = cpythonBaseName(func->implementingClass()->typeEntry());
        if (func->isConstructor()) {
            result += QLatin1String("_Init");
        } else {
            result += QLatin1String("Func_");
            if (func->isOperatorOverload())
                result += ShibokenGenerator::pythonOperatorFunctionName(func);
            else
                result += func->name();
        }
    } else {
        result = QLatin1String("Sbk") + moduleName() + QLatin1String("Module_") + func->name();
    }

    return result;
}

QString ShibokenGenerator::cpythonMethodDefinitionName(const AbstractMetaFunctionCPtr &func)
{
    if (!func->ownerClass())
        return QString();
    return cpythonBaseName(func->ownerClass()->typeEntry()) + QLatin1String("Method_")
           + func->name();
}

QString ShibokenGenerator::cpythonGettersSettersDefinitionName(const AbstractMetaClass *metaClass)
{
    return cpythonBaseName(metaClass) + QLatin1String("_getsetlist");
}

QString ShibokenGenerator::cpythonSetattroFunctionName(const AbstractMetaClass *metaClass)
{
    return cpythonBaseName(metaClass) + QLatin1String("_setattro");
}


QString ShibokenGenerator::cpythonGetattroFunctionName(const AbstractMetaClass *metaClass)
{
    return cpythonBaseName(metaClass) + QLatin1String("_getattro");
}

QString ShibokenGenerator::cpythonGetterFunctionName(const QString &name,
                                                     const AbstractMetaClass *enclosingClass)
{
    return cpythonBaseName(enclosingClass) + QStringLiteral("_get_") + name;
}

QString ShibokenGenerator::cpythonSetterFunctionName(const QString &name,
                                                     const AbstractMetaClass *enclosingClass)
{
    return cpythonBaseName(enclosingClass) + QStringLiteral("_set_") + name;
}

QString ShibokenGenerator::cpythonGetterFunctionName(const AbstractMetaField &metaField)
{
    return cpythonGetterFunctionName(metaField.name(), metaField.enclosingClass());
}

QString ShibokenGenerator::cpythonSetterFunctionName(const AbstractMetaField &metaField)
{
    return cpythonSetterFunctionName(metaField.name(), metaField.enclosingClass());
}

QString ShibokenGenerator::cpythonGetterFunctionName(const QPropertySpec &property,
                                                     const AbstractMetaClass *metaClass)
{
    return cpythonGetterFunctionName(property.name(), metaClass);
}

QString ShibokenGenerator::cpythonSetterFunctionName(const QPropertySpec &property,
                                                     const AbstractMetaClass *metaClass)
{
    return cpythonSetterFunctionName(property.name(), metaClass);
}

static QString cpythonEnumFlagsName(const QString &moduleName,
                                    const QString &qualifiedCppName)
{
    QString result = QLatin1String("Sbk") + moduleName + QLatin1Char('_') + qualifiedCppName;
    result.replace(QLatin1String("::"), QLatin1String("_"));
    return result;
}

QString ShibokenGenerator::cpythonEnumName(const EnumTypeEntry *enumEntry)
{
    QString p = enumEntry->targetLangPackage();
    p.replace(QLatin1Char('.'), QLatin1Char('_'));
    return cpythonEnumFlagsName(p, enumEntry->qualifiedCppName());
}

QString ShibokenGenerator::cpythonEnumName(const AbstractMetaEnum &metaEnum)
{
    return cpythonEnumName(metaEnum.typeEntry());
}

QString ShibokenGenerator::cpythonFlagsName(const FlagsTypeEntry *flagsEntry)
{
    QString p = flagsEntry->targetLangPackage();
    p.replace(QLatin1Char('.'), QLatin1Char('_'));
    return cpythonEnumFlagsName(p, flagsEntry->originalName());
}

QString ShibokenGenerator::cpythonFlagsName(const AbstractMetaEnum *metaEnum)
{
    const FlagsTypeEntry *flags = metaEnum->typeEntry()->flags();
    if (!flags)
        return QString();
    return cpythonFlagsName(flags);
}

QString ShibokenGenerator::cpythonSpecialCastFunctionName(const AbstractMetaClass *metaClass)
{
    return cpythonBaseName(metaClass->typeEntry()) + QLatin1String("SpecialCastFunction");
}

QString ShibokenGenerator::cpythonWrapperCPtr(const AbstractMetaClass *metaClass,
                                              const QString &argName)
{
    return cpythonWrapperCPtr(metaClass->typeEntry(), argName);
}

QString ShibokenGenerator::cpythonWrapperCPtr(const AbstractMetaType &metaType,
                                              const QString &argName)
{
    if (!metaType.isWrapperType())
        return QString();
    return QLatin1String("reinterpret_cast< ::") + metaType.cppSignature()
        + QLatin1String(" *>(Shiboken::Conversions::cppPointer(") + cpythonTypeNameExt(metaType)
        + QLatin1String(", reinterpret_cast<SbkObject *>(") + argName + QLatin1String(")))");
}

QString ShibokenGenerator::cpythonWrapperCPtr(const TypeEntry *type,
                                              const QString &argName)
{
    if (!type->isWrapperType())
        return QString();
    return QLatin1String("reinterpret_cast< ::") + type->qualifiedCppName()
        + QLatin1String(" *>(Shiboken::Conversions::cppPointer(") + cpythonTypeNameExt(type)
        + QLatin1String(", reinterpret_cast<SbkObject *>(") + argName + QLatin1String(")))");
}

void ShibokenGenerator::writeToPythonConversion(TextStream & s, const AbstractMetaType &type,
                                                const AbstractMetaClass * /* context */,
                                                const QString &argumentName)
{
    s << cpythonToPythonConversionFunction(type) << argumentName << ')';
}

void ShibokenGenerator::writeToCppConversion(TextStream &s, const AbstractMetaClass *metaClass,
                                             const QString &inArgName, const QString &outArgName)
{
    s << cpythonToCppConversionFunction(metaClass) << inArgName << ", &" << outArgName << ')';
}

void ShibokenGenerator::writeToCppConversion(TextStream &s, const AbstractMetaType &type,
                                             const AbstractMetaClass *context, const QString &inArgName,
                                             const QString &outArgName)
{
    s << cpythonToCppConversionFunction(type, context) << inArgName << ", &" << outArgName << ')';
}

bool ShibokenGenerator::shouldRejectNullPointerArgument(const AbstractMetaFunctionCPtr &func,
                                                        int argIndex)
{
    if (argIndex < 0 || argIndex >= func->arguments().count())
        return false;

    const AbstractMetaArgument &arg = func->arguments().at(argIndex);
    if (arg.type().isValueTypeWithCopyConstructorOnly())
        return true;

    // Argument type is not a pointer, a None rejection should not be
    // necessary because the type checking would handle that already.
    if (!arg.type().isPointer())
        return false;
    if (arg.isModifiedRemoved())
        return false;
    for (const auto &funcMod : func->modifications()) {
        for (const ArgumentModification &argMod : funcMod.argument_mods()) {
            if (argMod.index() == argIndex + 1 && argMod.noNullPointers())
                return true;
        }
    }
    return false;
}

QString ShibokenGenerator::getFormatUnitString(const AbstractMetaFunctionCPtr &func, bool incRef)
{
    QString result;
    const char objType = (incRef ? 'O' : 'N');
    const AbstractMetaArgumentList &arguments = func->arguments();
    for (const AbstractMetaArgument &arg : arguments) {
        if (arg.isModifiedRemoved())
            continue;

        const auto &type = arg.type();
        if (arg.isTypeModified()) {
            result += QLatin1Char(objType);
        } else if (arg.type().isObject()
            || type.isValue()
            || type.isValuePointer()
            || type.isNativePointer()
            || type.isEnum()
            || type.isFlags()
            || type.isContainer()
            || type.isSmartPointer()
            || type.referenceType() == LValueReference) {
            result += QLatin1Char(objType);
        } else if (type.isPrimitive()) {
            const auto *ptype = type.typeEntry()->asPrimitive()->basicReferencedTypeEntry();
            const auto it = formatUnits().constFind(ptype->name());
            if (it != formatUnits().cend())
                result += it.value();
            else
                result += QLatin1Char(objType);
        } else if (type.isCString()) {
            result += QLatin1Char('z');
        } else {
            qCWarning(lcShiboken).noquote().nospace()
                << "Method: " << func->ownerClass()->qualifiedCppName()
                << "::" << func->signature() << " => Arg:"
                << arg.name() << "index: " << arg.argumentIndex()
                << " - cannot be handled properly. Use an inject-code to fix it!";
            result += QLatin1Char('?');
        }
    }
    return result;
}

QString ShibokenGenerator::cpythonBaseName(const AbstractMetaType &type)
{
    if (type.isCString())
        return QLatin1String("PyString");
    return cpythonBaseName(type.typeEntry());
}

QString ShibokenGenerator::cpythonBaseName(const AbstractMetaClass *metaClass)
{
    return cpythonBaseName(metaClass->typeEntry());
}

QString ShibokenGenerator::cpythonBaseName(const TypeEntry *type)
{
    QString baseName;
    if (type->isWrapperType() || type->isNamespace()) { // && type->referenceType() == NoReference) {
        baseName = QLatin1String("Sbk_") + type->name();
    } else if (type->isPrimitive()) {
        const auto *ptype = type->asPrimitive()->basicReferencedTypeEntry();
        baseName = ptype->hasTargetLangApiType()
                   ? ptype->targetLangApiName() : pythonPrimitiveTypeName(ptype->name());
    } else if (type->isEnum()) {
        baseName = cpythonEnumName(static_cast<const EnumTypeEntry *>(type));
    } else if (type->isFlags()) {
        baseName = cpythonFlagsName(static_cast<const FlagsTypeEntry *>(type));
    } else if (type->isContainer()) {
        const auto *ctype = static_cast<const ContainerTypeEntry *>(type);
        switch (ctype->containerKind()) {
            case ContainerTypeEntry::ListContainer:
                //baseName = "PyList";
                //break;
            case ContainerTypeEntry::PairContainer:
                //baseName = "PyTuple";
                baseName = cPySequenceT();
                break;
            case ContainerTypeEntry::SetContainer:
                baseName = QLatin1String("PySet");
                break;
            case ContainerTypeEntry::MapContainer:
            case ContainerTypeEntry::MultiMapContainer:
                baseName = QLatin1String("PyDict");
                break;
            default:
                Q_ASSERT(false);
        }
    } else {
        baseName = cPyObjectT();
    }
    return baseName.replace(QLatin1String("::"), QLatin1String("_"));
}

QString ShibokenGenerator::cpythonTypeName(const AbstractMetaClass *metaClass)
{
    return cpythonTypeName(metaClass->typeEntry());
}

QString ShibokenGenerator::cpythonTypeName(const TypeEntry *type)
{
    return cpythonBaseName(type) + QLatin1String("_TypeF()");
}

QString ShibokenGenerator::cpythonTypeNameExt(const TypeEntry *type)
{
    return cppApiVariableName(type->targetLangPackage()) + QLatin1Char('[')
            + getTypeIndexVariableName(type) + QLatin1Char(']');
}

QString ShibokenGenerator::converterObject(const AbstractMetaType &type)
{
    if (type.isCString())
        return QLatin1String("Shiboken::Conversions::PrimitiveTypeConverter<const char *>()");
    if (type.isVoidPointer())
        return QLatin1String("Shiboken::Conversions::PrimitiveTypeConverter<void *>()");
    const AbstractMetaTypeList nestedArrayTypes = type.nestedArrayTypes();
    if (!nestedArrayTypes.isEmpty() && nestedArrayTypes.constLast().isCppPrimitive()) {
        return QStringLiteral("Shiboken::Conversions::ArrayTypeConverter<")
            + nestedArrayTypes.constLast().minimalSignature()
            + QLatin1String(">(") + QString::number(nestedArrayTypes.size())
            + QLatin1Char(')');
    }

    auto typeEntry = type.typeEntry();
    if (typeEntry->isContainer() || typeEntry->isSmartPointer()) {
        return convertersVariableName(typeEntry->targetLangPackage())
               + QLatin1Char('[') + getTypeIndexVariableName(type) + QLatin1Char(']');
    }
    return converterObject(typeEntry);
}

QString ShibokenGenerator::converterObject(const TypeEntry *type)
{
    if (type->isExtendedCppPrimitive())
        return QString::fromLatin1("Shiboken::Conversions::PrimitiveTypeConverter<%1>()")
                                  .arg(type->qualifiedCppName());
    if (type->isWrapperType())
        return QString::fromLatin1("PepType_SOTP(reinterpret_cast<PyTypeObject *>(%1))->converter")
                                  .arg(cpythonTypeNameExt(type));
    if (type->isEnum())
        return QString::fromLatin1("PepType_SETP(reinterpret_cast<SbkEnumType *>(%1))->converter")
                                  .arg(cpythonTypeNameExt(type));
    if (type->isFlags())
        return QString::fromLatin1("PepType_PFTP(reinterpret_cast<PySideQFlagsType *>(%1))->converter")
                                  .arg(cpythonTypeNameExt(type));

    if (type->isArray()) {
        qDebug() << "Warning: no idea how to handle the Qt5 type " << type->qualifiedCppName();
        return QString();
    }

    /* the typedef'd primitive types case */
    const auto *pte = dynamic_cast<const PrimitiveTypeEntry *>(type);
    if (!pte) {
        qDebug() << "Warning: the Qt5 primitive type is unknown" << type->qualifiedCppName();
        return QString();
    }
    pte = pte->basicReferencedTypeEntry();
    if (pte->isPrimitive() && !pte->isCppPrimitive() && !pte->customConversion()) {
        return u"Shiboken::Conversions::PrimitiveTypeConverter<"_qs
               + pte->qualifiedCppName() + u">()"_qs;
    }

    return convertersVariableName(type->targetLangPackage())
           + QLatin1Char('[') + getTypeIndexVariableName(type) + QLatin1Char(']');
}

QString ShibokenGenerator::cpythonTypeNameExt(const AbstractMetaType &type)
{
    return cppApiVariableName(type.typeEntry()->targetLangPackage()) + QLatin1Char('[')
           + getTypeIndexVariableName(type) + QLatin1Char(']');
}

static inline QString unknownOperator() { return QStringLiteral("__UNKNOWN_OPERATOR__"); }

QString ShibokenGenerator::fixedCppTypeName(const CustomConversion::TargetToNativeConversion *toNative)
{
    if (toNative->sourceType())
        return fixedCppTypeName(toNative->sourceType());
    return toNative->sourceTypeName();
}
QString ShibokenGenerator::fixedCppTypeName(const AbstractMetaType &type)
{
    return fixedCppTypeName(type.typeEntry(), type.cppSignature());
}

static QString _fixedCppTypeName(QString typeName)
{
    typeName.remove(QLatin1Char(' '));
    typeName.replace(QLatin1Char('.'),  QLatin1Char('_'));
    typeName.replace(QLatin1Char(','),  QLatin1Char('_'));
    typeName.replace(QLatin1Char('<'),  QLatin1Char('_'));
    typeName.replace(QLatin1Char('>'),  QLatin1Char('_'));
    typeName.replace(QLatin1String("::"), QLatin1String("_"));
    typeName.replace(QLatin1String("*"),  QLatin1String("PTR"));
    typeName.replace(QLatin1String("&"),  QLatin1String("REF"));
    return typeName;
}
QString ShibokenGenerator::fixedCppTypeName(const TypeEntry *type, QString typeName)
{
    if (typeName.isEmpty())
        typeName = type->qualifiedCppName();
    if (!type->generateCode()) {
        typeName.prepend(QLatin1Char('_'));
        typeName.prepend(type->targetLangPackage());
    }
    return _fixedCppTypeName(typeName);
}

QString ShibokenGenerator::pythonPrimitiveTypeName(const QString &cppTypeName)
{
    const auto &mapping = primitiveTypesCorrespondences();
    const auto it = mapping.constFind(cppTypeName);
    if (it == mapping.cend())
        throw Exception(u"Primitive type not found: "_qs + cppTypeName);
    return it.value();
}

QString ShibokenGenerator::pythonOperatorFunctionName(const AbstractMetaFunctionCPtr &func)
{
    QString op = Generator::pythonOperatorFunctionName(func->originalName());
    if (op.isEmpty()) {
        qCWarning(lcShiboken).noquote().nospace() << msgUnknownOperator(func.data());
        return unknownOperator();
    }
    if (func->arguments().isEmpty()) {
        if (op == QLatin1String("__sub__"))
            op = QLatin1String("__neg__");
        else if (op == QLatin1String("__add__"))
            op = QLatin1String("__pos__");
    } else if (func->isStatic() && func->arguments().size() == 2) {
        // If a operator overload function has 2 arguments and
        // is static we assume that it is a reverse operator.
        op = op.insert(2, QLatin1Char('r'));
    }
    return op;
}

bool ShibokenGenerator::isNumber(const QString &cpythonApiName)
{
    return cpythonApiName == pyIntT()
       || cpythonApiName == pyFloatT() || cpythonApiName == pyLongT()
       || cpythonApiName == pyBoolT();
}

static std::optional<TypeSystem::CPythonType>
    targetLangApiCPythonType(const PrimitiveTypeEntry *t)
{
    if (!t->hasTargetLangApiType())
        return {};
    const auto *cte = t->targetLangApiType();
    if (cte->type() != TypeEntry::PythonType)
        return {};
    return static_cast<const PythonTypeEntry *>(cte)->cPythonType();
}

bool ShibokenGenerator::isNumber(const TypeEntry *type)
{
    if (!type->isPrimitive())
        return false;
    const auto *pte = type->asPrimitive()->basicReferencedTypeEntry();
    const auto cPythonTypeOpt = targetLangApiCPythonType(pte);
    // FIXME PYSIDE-1660: Return false here after making primitive types built-in?
    if (!cPythonTypeOpt.has_value()) {
        const auto &mapping = primitiveTypesCorrespondences();
        const auto it = mapping.constFind(pte->name());
        return it != mapping.cend() && isNumber(it.value());
    }
    const auto cPythonType = cPythonTypeOpt.value();
    return cPythonType == TypeSystem::CPythonType::Bool
           || cPythonType == TypeSystem::CPythonType::Float
           || cPythonType == TypeSystem::CPythonType::Integer;
}

bool ShibokenGenerator::isNumber(const AbstractMetaType &type)
{
    return isNumber(type.typeEntry());
}

bool ShibokenGenerator::isPyInt(const TypeEntry *type)
{
    if (!type->isPrimitive())
        return false;
    const auto *pte = type->asPrimitive()->basicReferencedTypeEntry();
    const auto cPythonTypeOpt = targetLangApiCPythonType(pte);
    // FIXME PYSIDE-1660: Return false here after making primitive types built-in?
    if (!cPythonTypeOpt.has_value()) {
        const auto &mapping = primitiveTypesCorrespondences();
        const auto it = mapping.constFind(pte->name());
        return it != mapping.cend() && it.value() ==  pyLongT();
    }
    return cPythonTypeOpt.value() == TypeSystem::CPythonType::Integer;
}

bool ShibokenGenerator::isPyInt(const AbstractMetaType &type)
{
    return isPyInt(type.typeEntry());
}

bool ShibokenGenerator::isNullPtr(const QString &value)
{
    return value == QLatin1String("0") || value == QLatin1String("nullptr")
        || value == QLatin1String("NULLPTR") || value == QLatin1String("{}");
}

QString ShibokenGenerator::cpythonCheckFunction(AbstractMetaType metaType) const
{
    const auto *typeEntry = metaType.typeEntry();
    if (typeEntry->isCustom()) {
        const auto *cte = static_cast<const CustomTypeEntry *>(typeEntry);
        if (cte->hasCheckFunction())
            return cte->checkFunction();
        throw Exception(msgUnknownCheckFunction(typeEntry));
    }

    if (metaType.isExtendedCppPrimitive()) {
        if (metaType.isCString())
            return QLatin1String("Shiboken::String::check");
        if (metaType.isVoidPointer())
            return u"true"_qs;
        return cpythonCheckFunction(typeEntry);
    }

    if (typeEntry->isContainer()) {
        QString typeCheck = QLatin1String("Shiboken::Conversions::");
        ContainerTypeEntry::ContainerKind type =
            static_cast<const ContainerTypeEntry *>(typeEntry)->containerKind();
        if (type == ContainerTypeEntry::ListContainer
            || type == ContainerTypeEntry::SetContainer) {
            const QString containerType = type == ContainerTypeEntry::SetContainer
                ? u"Iterable"_qs : u"Sequence"_qs;
            const AbstractMetaType &type = metaType.instantiations().constFirst();
            if (type.isPointerToWrapperType()) {
                typeCheck += u"check"_qs + containerType + u"Types("_qs
                             + cpythonTypeNameExt(type) + u", "_qs;
            } else if (type.isWrapperType()) {
                typeCheck += u"convertible"_qs + containerType
                             + u"Types("_qs + cpythonTypeNameExt(type) + u", "_qs;
            } else {
                typeCheck += u"convertible"_qs + containerType
                             + u"Types("_qs + converterObject(type) + u", "_qs;
            }
        } else if (type == ContainerTypeEntry::MapContainer
            || type == ContainerTypeEntry::MultiMapContainer
            || type == ContainerTypeEntry::PairContainer) {

            QString pyType;
            if (type == ContainerTypeEntry::PairContainer)
                pyType = u"Pair"_qs;
            else if (type == ContainerTypeEntry::MultiMapContainer)
                pyType = u"MultiDict"_qs;
            else
                pyType = u"Dict"_qs;

            const AbstractMetaType &firstType = metaType.instantiations().constFirst();
            const AbstractMetaType &secondType = metaType.instantiations().constLast();
            if (firstType.isPointerToWrapperType() && secondType.isPointerToWrapperType()) {
                QTextStream(&typeCheck) << "check" << pyType << "Types("
                    << cpythonTypeNameExt(firstType) << ", "
                    << cpythonTypeNameExt(secondType) << ", ";
            } else {
                QTextStream(&typeCheck) << "convertible" << pyType << "Types("
                    << converterObject(firstType) << ", "
                    << (firstType.isPointerToWrapperType() ? "true" : "false")
                    << ", " << converterObject(secondType) << ", "
                    << (secondType.isPointerToWrapperType() ? "true" :"false")
                    << ", ";
            }
        }
        return typeCheck;
    }
    return cpythonCheckFunction(typeEntry);
}

QString ShibokenGenerator::cpythonCheckFunction(const TypeEntry *type) const
{
    if (type->isCustom()) {
        const auto *cte = static_cast<const CustomTypeEntry *>(type);
        if (cte->hasCheckFunction())
            return cte->checkFunction();
        throw Exception(msgUnknownCheckFunction(type));
    }

    if (type->isEnum() || type->isFlags() || type->isWrapperType())
        return u"SbkObject_TypeCheck("_qs + cpythonTypeNameExt(type) + u", "_qs;

    if (type->isPrimitive())
        type = type->asPrimitive()->basicReferencedTypeEntry();

    if (auto *tla = type->targetLangApiType()) {
        if (tla->hasCheckFunction())
            return tla->checkFunction();
    }

    if (type->isExtendedCppPrimitive()) {
        const auto *pte = type->asPrimitive();
        return pythonPrimitiveTypeName(pte->name())
                                       + QLatin1String("_Check");
    }

    return cpythonIsConvertibleFunction(type);
}

QString ShibokenGenerator::cpythonIsConvertibleFunction(const TypeEntry *type)
{
    if (type->isWrapperType()) {
        QString result = QLatin1String("Shiboken::Conversions::");
        bool isValue = false;
        if (type->isValue()) {
            const auto *cte = static_cast<const ComplexTypeEntry *>(type);
            isValue = !cte->isValueTypeWithCopyConstructorOnly();
        }
        result += isValue
                         ? QLatin1String("isPythonToCppValueConvertible")
                         : QLatin1String("isPythonToCppPointerConvertible");
        result += QLatin1String("(")
            + cpythonTypeNameExt(type) + QLatin1String(", ");
        return result;
    }
    return QString::fromLatin1("Shiboken::Conversions::isPythonToCppConvertible(%1, ")
              .arg(converterObject(type));
}

QString ShibokenGenerator::cpythonIsConvertibleFunction(AbstractMetaType metaType)
{
    const auto *typeEntry = metaType.typeEntry();
    if (typeEntry->isCustom()) {
        const auto *cte = static_cast<const CustomTypeEntry *>(typeEntry);
        if (cte->hasCheckFunction())
            return cte->checkFunction();
        throw Exception(msgUnknownCheckFunction(typeEntry));
    }

    QString result = QLatin1String("Shiboken::Conversions::");
    if (metaType.generateOpaqueContainer()) {
        result += u"pythonToCppReferenceConversion("_qs
                  + converterObject(metaType) + u", "_qs;
        return result;
    }
    if (metaType.isWrapperType()) {
        if (metaType.isPointer() || metaType.isValueTypeWithCopyConstructorOnly())
            result += u"pythonToCppPointerConversion"_qs;
        else if (metaType.referenceType() == LValueReference)
            result += u"pythonToCppReferenceConversion"_qs;
        else
            result += u"pythonToCppValueConversion"_qs;
        result += u'(' + cpythonTypeNameExt(metaType) + u", "_qs;
        return result;
    }
    result += u"pythonToCppConversion("_qs + converterObject(metaType);
    // Write out array sizes if known
    const AbstractMetaTypeList nestedArrayTypes = metaType.nestedArrayTypes();
    if (!nestedArrayTypes.isEmpty() && nestedArrayTypes.constLast().isCppPrimitive()) {
        const int dim1 = metaType.arrayElementCount();
        const int dim2 = nestedArrayTypes.constFirst().isArray()
            ? nestedArrayTypes.constFirst().arrayElementCount() : -1;
        result += QLatin1String(", ") + QString::number(dim1)
            + QLatin1String(", ") + QString::number(dim2);
    }
    result += QLatin1String(", ");
    return result;
}

QString ShibokenGenerator::cpythonIsConvertibleFunction(const AbstractMetaArgument &metaArg)
{
    return cpythonIsConvertibleFunction(metaArg.type());
}

QString ShibokenGenerator::cpythonToCppConversionFunction(const AbstractMetaClass *metaClass)
{
    return QLatin1String("Shiboken::Conversions::pythonToCppPointer(")
        + cpythonTypeNameExt(metaClass->typeEntry()) + QLatin1String(", ");
}

QString ShibokenGenerator::cpythonToCppConversionFunction(const AbstractMetaType &type,
                                                          const AbstractMetaClass * /* context */)
{
    if (type.isWrapperType()) {
        return QLatin1String("Shiboken::Conversions::pythonToCpp")
            + (type.isPointer() ? QLatin1String("Pointer") : QLatin1String("Copy"))
            + u'(' + cpythonTypeNameExt(type) + QLatin1String(", ");
    }
    return QStringLiteral("Shiboken::Conversions::pythonToCppCopy(%1, ")
              .arg(converterObject(type));
}

QString ShibokenGenerator::cpythonToPythonConversionFunction(const AbstractMetaType &type,
                                                             const AbstractMetaClass * /* context */)
{
    if (type.isWrapperType()) {
        QString conversion;
        if (type.referenceType() == LValueReference
            && !(type.isValue() && type.isConstant()) && !type.isPointer()) {
            conversion = QLatin1String("reference");
        } else if (type.isValue() || type.isSmartPointer()) {
            conversion = QLatin1String("copy");
        } else {
            conversion = QLatin1String("pointer");
        }
        QString result = QLatin1String("Shiboken::Conversions::") + conversion
            + QLatin1String("ToPython(")
            + cpythonTypeNameExt(type) + QLatin1String(", ");
        if (conversion != QLatin1String("pointer"))
            result += QLatin1Char('&');
        return result;
    }
    return QStringLiteral("Shiboken::Conversions::copyToPython(%1, %2")
              .arg(converterObject(type),
                   (type.isCString() || type.isVoidPointer()) ? QString() : QLatin1String("&"));
}

QString ShibokenGenerator::cpythonToPythonConversionFunction(const AbstractMetaClass *metaClass)
{
    return cpythonToPythonConversionFunction(metaClass->typeEntry());
}

QString ShibokenGenerator::cpythonToPythonConversionFunction(const TypeEntry *type)
{
    if (type->isWrapperType()) {
        const QString conversion = type->isValue() ? QLatin1String("copy") : QLatin1String("pointer");
         QString result = QLatin1String("Shiboken::Conversions::") + conversion
             + QLatin1String("ToPython(") + cpythonTypeNameExt(type)
             + QLatin1String(", ");
         if (conversion != QLatin1String("pointer"))
             result += QLatin1Char('&');
        return result;
    }

    return u"Shiboken::Conversions::copyToPython("_qs
           + converterObject(type) + u", &"_qs;
}

QString ShibokenGenerator::argumentString(const AbstractMetaFunctionCPtr &func,
                                          const AbstractMetaArgument &argument,
                                          Options options) const
{
    auto type = options.testFlag(OriginalTypeDescription)
        ? argument.type() : argument.modifiedType();

    QString arg = translateType(type, func->implementingClass(), options);

    if (argument.isTypeModified())
        arg.replace(QLatin1Char('$'), QLatin1Char('.')); // Haehh?

    // "int a", "int a[]"
    const int arrayPos = arg.indexOf(QLatin1Char('['));
    if (arrayPos != -1)
        arg.insert(arrayPos, QLatin1Char(' ') + argument.name());
    else
        arg.append(QLatin1Char(' ') + argument.name());

    if ((options & Generator::SkipDefaultValues) != Generator::SkipDefaultValues &&
        !argument.originalDefaultValueExpression().isEmpty())
    {
        QString default_value = argument.originalDefaultValueExpression();
        if (default_value == QLatin1String("NULL"))
            default_value = QLatin1String(NULL_PTR);

        //WORKAROUND: fix this please
        if (default_value.startsWith(QLatin1String("new ")))
            default_value.remove(0, 4);

        arg += QLatin1String(" = ") + default_value;
    }

    return arg;
}

void ShibokenGenerator::writeArgument(TextStream &s,
                                      const AbstractMetaFunctionCPtr &func,
                                      const AbstractMetaArgument &argument,
                                      Options options) const
{
    s << argumentString(func, argument, options);
}

void ShibokenGenerator::writeFunctionArguments(TextStream &s,
                                               const AbstractMetaFunctionCPtr &func,
                                               Options options) const
{
    AbstractMetaArgumentList arguments = func->arguments();

    int argUsed = 0;
    for (int i = 0; i < arguments.size(); ++i) {
        const auto &arg = arguments.at(i);
        if (options.testFlag(Generator::SkipRemovedArguments) && arg.isModifiedRemoved())
            continue;

        if (argUsed != 0)
            s << ", ";
        writeArgument(s, func, arg, options);
        argUsed++;
    }
}

GeneratorContext ShibokenGenerator::contextForClass(const AbstractMetaClass *c) const
{
    GeneratorContext result = Generator::contextForClass(c);
    if (shouldGenerateCppWrapper(c)) {
        result.m_type = GeneratorContext::WrappedClass;
        result.m_wrappername = wrapperName(c);
    }
    return result;
}

QString ShibokenGenerator::functionReturnType(const AbstractMetaFunctionCPtr &func, Options options) const
{
    if (func->isTypeModified() && !options.testFlag(OriginalTypeDescription))
        return func->modifiedTypeName();
    return translateType(func->type(), func->implementingClass(), options);
}

QString ShibokenGenerator::functionSignature(const AbstractMetaFunctionCPtr &func,
                                             const QString &prepend,
                                             const QString &append,
                                             Options options,
                                             int /* argCount */) const
{
    StringStream s(TextStream::Language::Cpp);
    // The actual function
    if (func->isEmptyFunction() || func->needsReturnType())
        s << functionReturnType(func, options) << ' ';
    else
        options |= Generator::SkipReturnType;

    // name
    QString name(func->originalName());
    if (func->isConstructor())
        name = wrapperName(func->ownerClass());

    s << prepend << name << append << '(';
    writeFunctionArguments(s, func, options);
    s << ')';

    if (func->isConstant())
        s << " const";

    if (func->exceptionSpecification() == ExceptionSpecification::NoExcept)
        s << " noexcept";

    return s;
}

void ShibokenGenerator::writeArgumentNames(TextStream &s,
                                           const AbstractMetaFunctionCPtr &func,
                                           Options options)
{
    const AbstractMetaArgumentList arguments = func->arguments();
    int argCount = 0;
    for (const auto &argument : arguments) {
        const int index = argument.argumentIndex() + 1;
        if (options.testFlag(Generator::SkipRemovedArguments) && argument.isModifiedRemoved())
            continue;

        s << ((argCount > 0) ? ", " : "") << argument.name();

        if (((options & Generator::VirtualCall) == 0)
            && (!func->conversionRule(TypeSystem::NativeCode, index).isEmpty()
                || !func->conversionRule(TypeSystem::TargetLangCode, index).isEmpty())
            && !func->isConstructor()) {
           s << CONV_RULE_OUT_VAR_SUFFIX;
        }

        argCount++;
    }
}

void ShibokenGenerator::writeFunctionCall(TextStream &s,
                                          const AbstractMetaFunctionCPtr &func,
                                          Options options)
{
    s << (func->isConstructor() ? func->ownerClass()->qualifiedCppName() : func->originalName())
        << '(';
    writeArgumentNames(s, func, options);
    s << ')';
}

void ShibokenGenerator::writeUnusedVariableCast(TextStream &s, const QString &variableName)
{
    s << "SBK_UNUSED(" << variableName<< ")\n";
}

static bool filterFunction(const AbstractMetaFunctionCPtr &func, bool avoidProtectedHack)
{
    switch (func->functionType()) {
    case AbstractMetaFunction::DestructorFunction:
    case AbstractMetaFunction::SignalFunction:
    case AbstractMetaFunction::GetAttroFunction:
    case AbstractMetaFunction::SetAttroFunction:
        return false;
    default:
        break;
    }
    if (func->usesRValueReferences())
        return false;
    if (func->isModifiedRemoved() && !func->isAbstract()
        && (!avoidProtectedHack || !func->isProtected())) {
        return false;
    }
    return true;
}

AbstractMetaFunctionCList ShibokenGenerator::filterFunctions(const AbstractMetaClass *metaClass) const
{
    AbstractMetaFunctionCList result;
    const AbstractMetaFunctionCList &funcs = metaClass->functions();
    result.reserve(funcs.size());
    for (const auto &func : funcs) {
        if (filterFunction(func, avoidProtectedHack()))
            result.append(func);
    }
    return result;
}

ShibokenGenerator::ExtendedConverterData ShibokenGenerator::getExtendedConverters() const
{
    ExtendedConverterData extConvs;
    for (auto metaClass : api().classes()) {
        // Use only the classes for the current module.
        if (!shouldGenerate(metaClass))
            continue;
        const auto &overloads = metaClass->operatorOverloads(OperatorQueryOption::ConversionOp);
        for (const auto &convOp : overloads) {
            // Get only the conversion operators that return a type from another module,
            // that are value-types and were not removed in the type system.
            const TypeEntry *convType = convOp->type().typeEntry();
            if (convType->generateCode() || !convType->isValue()
                || convOp->isModifiedRemoved())
                continue;
            extConvs[convType].append(convOp->ownerClass());
        }
    }
    return extConvs;
}

QList<const CustomConversion *> ShibokenGenerator::getPrimitiveCustomConversions()
{
    QList<const CustomConversion *> conversions;
    const PrimitiveTypeEntryList &primitiveTypeList = primitiveTypes();
    for (const PrimitiveTypeEntry *type : primitiveTypeList) {
        if (!shouldGenerateTypeEntry(type) || !type->isUserPrimitive() || !type->customConversion())
            continue;

        conversions << type->customConversion();
    }
    return conversions;
}

static QString getArgumentsFromMethodCall(const QString &str)
{
    // It would be way nicer to be able to use a Perl like
    // regular expression that accepts temporary variables
    // to count the parenthesis.
    // For more information check this:
    // http://perl.plover.com/yak/regex/samples/slide083.html
    static QLatin1String funcCall("%CPPSELF.%FUNCTION_NAME");
    int pos = str.indexOf(funcCall);
    if (pos == -1)
        return QString();
    pos = pos + funcCall.size();
    while (str.at(pos) == QLatin1Char(' ') || str.at(pos) == QLatin1Char('\t'))
        ++pos;
    if (str.at(pos) == QLatin1Char('('))
        ++pos;
    int begin = pos;
    int counter = 1;
    while (counter != 0) {
        if (str.at(pos) == QLatin1Char('('))
            ++counter;
        else if (str.at(pos) == QLatin1Char(')'))
            --counter;
        ++pos;
    }
    return str.mid(begin, pos-begin-1);
}

QString ShibokenGenerator::getCodeSnippets(const CodeSnipList &codeSnips,
                                           TypeSystem::CodeSnipPosition position,
                                           TypeSystem::Language language)
{
    QString code;
    for (const CodeSnip &snip : codeSnips) {
        if ((position != TypeSystem::CodeSnipPositionAny && snip.position != position) || !(snip.language & language))
            continue;
        code.append(snip.code());
    }
    return code;
}

void ShibokenGenerator::processClassCodeSnip(QString &code, const GeneratorContext &context) const
{
    auto metaClass = context.metaClass();
    // Replace template variable by the Python Type object
    // for the class context in which the variable is used.
    code.replace(QLatin1String("%PYTHONTYPEOBJECT"),
                 u"(*"_qs + cpythonTypeName(metaClass) + u')');
    const QString className = context.useWrapper()
        ? context.wrapperName() : metaClass->qualifiedCppName();
    code.replace(QLatin1String("%TYPE"), className);
    code.replace(QLatin1String("%CPPTYPE"), metaClass->name());

    processCodeSnip(code);
}

void ShibokenGenerator::processCodeSnip(QString &code) const
{
    // replace "toPython" converters
    replaceConvertToPythonTypeSystemVariable(code);

    // replace "toCpp" converters
    replaceConvertToCppTypeSystemVariable(code);

    // replace "isConvertible" check
    replaceIsConvertibleToCppTypeSystemVariable(code);

    // replace "checkType" check
    replaceTypeCheckTypeSystemVariable(code);
}

ShibokenGenerator::ArgumentVarReplacementList
    ShibokenGenerator::getArgumentReplacement(const AbstractMetaFunctionCPtr &func,
                                              bool usePyArgs, TypeSystem::Language language,
                                              const AbstractMetaArgument *lastArg)
{
    ArgumentVarReplacementList argReplacements;
    TypeSystem::Language convLang = (language == TypeSystem::TargetLangCode)
                                    ? TypeSystem::NativeCode : TypeSystem::TargetLangCode;
    int removed = 0;
    for (int i = 0; i < func->arguments().size(); ++i) {
        const AbstractMetaArgument &arg = func->arguments().at(i);
        QString argValue;
        if (language == TypeSystem::TargetLangCode) {
            bool hasConversionRule = !func->conversionRule(convLang, i+1).isEmpty();
            const bool argRemoved = arg.isModifiedRemoved();
            if (argRemoved)
                ++removed;
            if (argRemoved && hasConversionRule)
                argValue = arg.name() + QLatin1String(CONV_RULE_OUT_VAR_SUFFIX);
            else if (argRemoved || (lastArg && arg.argumentIndex() > lastArg->argumentIndex()))
                argValue = QLatin1String(CPP_ARG_REMOVED) + QString::number(i);
            if (!argRemoved && argValue.isEmpty()) {
                int argPos = i - removed;
                AbstractMetaType type = arg.modifiedType();
                if (type.typeEntry()->isCustom()) {
                    argValue = usePyArgs
                               ? pythonArgsAt(argPos) : QLatin1String(PYTHON_ARG);
                } else {
                    argValue = hasConversionRule
                               ? arg.name() + QLatin1String(CONV_RULE_OUT_VAR_SUFFIX)
                               : QLatin1String(CPP_ARG) + QString::number(argPos);
                    if (type.shouldDereferenceArgument())
                        AbstractMetaType::dereference(&argValue);
                }
            }
        } else {
            argValue = arg.name();
        }
        if (!argValue.isEmpty())
            argReplacements << ArgumentVarReplacementPair(arg, argValue);

    }
    return argReplacements;
}

void ShibokenGenerator::writeClassCodeSnips(TextStream &s,
                                       const CodeSnipList &codeSnips,
                                       TypeSystem::CodeSnipPosition position,
                                       TypeSystem::Language language,
                                       const GeneratorContext &context) const
{
    QString code = getCodeSnippets(codeSnips, position, language);
    if (code.isEmpty())
        return;
    processClassCodeSnip(code, context);
    s << "// Begin code injection\n" << code << "// End of code injection\n\n";
}

void ShibokenGenerator::writeCodeSnips(TextStream &s,
                                       const CodeSnipList &codeSnips,
                                       TypeSystem::CodeSnipPosition position,
                                       TypeSystem::Language language) const
{
    QString code = getCodeSnippets(codeSnips, position, language);
    if (code.isEmpty())
        return;
    processCodeSnip(code);
    s << "// Begin code injection\n" << code << "// End of code injection\n\n";
}

static void replacePyArg0(TypeSystem::Language language, QString *code)
{
    static const QString pyArg0 = u"%PYARG_0"_qs;
    static const QString pyReturn = QLatin1String(PYTHON_RETURN_VAR);

    if (!code->contains(pyArg0))
        return;
    if (language != TypeSystem::NativeCode) {
        code->replace(pyArg0, pyReturn);
        return;
    }

    // pyResult is an AutoDecRef in overridden methods of wrapper classes which
    // has a cast operator for PyObject *. This may however not work in all
    // situations (fex _PyVarObject_CAST(op) defined as ((PyVarObject*)(op))).
    // Append ".object()" unless it is followed by a '.' indicating explicit
    // AutoDecRef member invocation.
    static const QString pyObject = pyReturn + u".object()"_qs;
    qsizetype pos{};
    while ( (pos = code->indexOf(pyArg0)) >= 0) {
        const auto next = pos + pyArg0.size();
        const bool memberInvocation = next < code->size() && code->at(next) == u'.';
        code->replace(pos, pyArg0.size(),
                      memberInvocation ? pyReturn : pyObject);
    }
}

void ShibokenGenerator::writeCodeSnips(TextStream &s,
                                       const CodeSnipList &codeSnips,
                                       TypeSystem::CodeSnipPosition position,
                                       TypeSystem::Language language,
                                       const AbstractMetaFunctionCPtr &func,
                                       bool usePyArgs,
                                       const AbstractMetaArgument *lastArg) const
{
    QString code = getCodeSnippets(codeSnips, position, language);
    if (code.isEmpty())
        return;

    // Replace %PYARG_# variables.
    replacePyArg0(language, &code);

    static const QRegularExpression pyArgsRegex(QStringLiteral("%PYARG_(\\d+)"));
    Q_ASSERT(pyArgsRegex.isValid());
    if (language == TypeSystem::TargetLangCode) {
        if (usePyArgs) {
            code.replace(pyArgsRegex, QLatin1String(PYTHON_ARGS) + QLatin1String("[\\1-1]"));
        } else {
            static const QRegularExpression pyArgsRegexCheck(QStringLiteral("%PYARG_([2-9]+)"));
            Q_ASSERT(pyArgsRegexCheck.isValid());
            const QRegularExpressionMatch match = pyArgsRegexCheck.match(code);
            if (match.hasMatch()) {
                qCWarning(lcShiboken).noquote().nospace()
                    << msgWrongIndex("%PYARG", match.captured(1), func.data());
                return;
            }
            code.replace(QLatin1String("%PYARG_1"), QLatin1String(PYTHON_ARG));
        }
    } else {
        // Replaces the simplest case of attribution to a
        // Python argument on the binding virtual method.
        static const QRegularExpression pyArgsAttributionRegex(QStringLiteral("%PYARG_(\\d+)\\s*=[^=]\\s*([^;]+)"));
        Q_ASSERT(pyArgsAttributionRegex.isValid());
        code.replace(pyArgsAttributionRegex, QLatin1String("PyTuple_SET_ITEM(")
                     + QLatin1String(PYTHON_ARGS) + QLatin1String(", \\1-1, \\2)"));
        code.replace(pyArgsRegex, QLatin1String("PyTuple_GET_ITEM(")
                     + QLatin1String(PYTHON_ARGS) + QLatin1String(", \\1-1)"));
    }

    // Replace %ARG#_TYPE variables.
    const AbstractMetaArgumentList &arguments = func->arguments();
    for (const AbstractMetaArgument &arg : arguments) {
        QString argTypeVar = u"%ARG"_qs + QString::number(arg.argumentIndex() + 1)
                             + u"_TYPE"_qs;
        QString argTypeVal = arg.type().cppSignature();
        code.replace(argTypeVar, argTypeVal);
    }

    static const QRegularExpression cppArgTypeRegexCheck(QStringLiteral("%ARG(\\d+)_TYPE"));
    Q_ASSERT(cppArgTypeRegexCheck.isValid());
    QRegularExpressionMatchIterator rit = cppArgTypeRegexCheck.globalMatch(code);
    while (rit.hasNext()) {
        QRegularExpressionMatch match = rit.next();
        qCWarning(lcShiboken).noquote().nospace()
            << msgWrongIndex("%ARG#_TYPE", match.captured(1), func.data());
    }

    // Replace template variable for return variable name.
    if (func->isConstructor()) {
        code.replace(QLatin1String("%0."), QLatin1String("cptr->"));
        code.replace(QLatin1String("%0"), QLatin1String("cptr"));
    } else if (!func->isVoid()) {
        QString returnValueOp = func->type().isPointerToWrapperType()
            ? QLatin1String("%1->") : QLatin1String("%1.");
        if (func->type().isWrapperType())
            code.replace(QLatin1String("%0."), returnValueOp.arg(QLatin1String(CPP_RETURN_VAR)));
        code.replace(QLatin1String("%0"), QLatin1String(CPP_RETURN_VAR));
    }

    // Replace template variable for self Python object.
    QString pySelf = language == TypeSystem::NativeCode
        ? QLatin1String("pySelf") : QLatin1String("self");
    code.replace(QLatin1String("%PYSELF"), pySelf);

    // Replace template variable for a pointer to C++ of this object.
    if (func->implementingClass()) {
        QString replacement = func->isStatic() ? QLatin1String("%1::") : QLatin1String("%1->");
        QString cppSelf;
        if (func->isStatic())
            cppSelf = func->ownerClass()->qualifiedCppName();
        else if (language == TypeSystem::NativeCode)
            cppSelf = QLatin1String("this");
        else
            cppSelf = QLatin1String(CPP_SELF_VAR);

        // On comparison operator CPP_SELF_VAR is always a reference.
        if (func->isComparisonOperator())
            replacement = QLatin1String("%1.");

        if (func->isVirtual() && !func->isAbstract() && (!avoidProtectedHack() || !func->isProtected())) {
            QString methodCallArgs = getArgumentsFromMethodCall(code);
            if (!methodCallArgs.isEmpty()) {
                const QString pattern = u"%CPPSELF.%FUNCTION_NAME("_qs + methodCallArgs + u')';
                QString replacement = u"(Shiboken::Object::hasCppWrapper(reinterpret_cast<SbkObject *>("_qs
                                      + pySelf + u")) ? "_qs;
                if (func->name() == QLatin1String("metaObject")) {
                    QString wrapperClassName = wrapperName(func->ownerClass());
                    QString cppSelfVar = avoidProtectedHack()
                        ? u"%CPPSELF"_qs
                        : u"reinterpret_cast<"_qs + wrapperClassName + u" *>(%CPPSELF)"_qs;
                    replacement += cppSelfVar + u"->::"_qs + wrapperClassName
                                   + u"::%FUNCTION_NAME("_qs + methodCallArgs
                                   + u") : %CPPSELF.%FUNCTION_NAME("_qs + methodCallArgs + u"))"_qs;
                } else {
                    replacement += u"%CPPSELF->::%TYPE::%FUNCTION_NAME("_qs + methodCallArgs
                                   + u") : %CPPSELF.%FUNCTION_NAME("_qs + methodCallArgs + u"))"_qs;
                }
                code.replace(pattern, replacement);
            }
        }

        code.replace(QLatin1String("%CPPSELF."), replacement.arg(cppSelf));
        code.replace(QLatin1String("%CPPSELF"), cppSelf);

        if (code.indexOf(QLatin1String("%BEGIN_ALLOW_THREADS")) > -1) {
            if (code.count(QLatin1String("%BEGIN_ALLOW_THREADS")) == code.count(QLatin1String("%END_ALLOW_THREADS"))) {
                code.replace(QLatin1String("%BEGIN_ALLOW_THREADS"), QLatin1String(BEGIN_ALLOW_THREADS));
                code.replace(QLatin1String("%END_ALLOW_THREADS"), QLatin1String(END_ALLOW_THREADS));
            } else {
                qCWarning(lcShiboken) << "%BEGIN_ALLOW_THREADS and %END_ALLOW_THREADS mismatch";
            }
        }

        // replace template variable for the Python Type object for the
        // class implementing the method in which the code snip is written
        if (func->isStatic()) {
            code.replace(QLatin1String("%PYTHONTYPEOBJECT"),
                                       u"(*"_qs + cpythonTypeName(func->implementingClass()) + u')');
        } else {
            code.replace(QLatin1String("%PYTHONTYPEOBJECT."), pySelf + QLatin1String("->ob_type->"));
            code.replace(QLatin1String("%PYTHONTYPEOBJECT"), pySelf + QLatin1String("->ob_type"));
        }
    }

    // Replaces template %ARGUMENT_NAMES and %# variables by argument variables and values.
    // Replaces template variables %# for individual arguments.
    const ArgumentVarReplacementList &argReplacements = getArgumentReplacement(func, usePyArgs, language, lastArg);

    QStringList args;
    for (const ArgumentVarReplacementPair &pair : argReplacements) {
        if (pair.second.startsWith(QLatin1String(CPP_ARG_REMOVED)))
            continue;
        args << pair.second;
    }
    code.replace(QLatin1String("%ARGUMENT_NAMES"), args.join(QLatin1String(", ")));

    for (const ArgumentVarReplacementPair &pair : argReplacements) {
        const AbstractMetaArgument &arg = pair.first;
        int idx = arg.argumentIndex() + 1;
        AbstractMetaType type = arg.modifiedType();
        if (type.isWrapperType()) {
            QString replacement = pair.second;
            if (type.shouldDereferenceArgument())
                AbstractMetaType::stripDereference(&replacement);
            if (type.referenceType() == LValueReference || type.isPointer())
                code.replace(u'%' + QString::number(idx) + u'.', replacement + u"->"_qs);
        }
        code.replace(CodeSnipAbstract::placeHolderRegex(idx), pair.second);
    }

    if (language == TypeSystem::NativeCode) {
        // Replaces template %PYTHON_ARGUMENTS variable with a pointer to the Python tuple
        // containing the converted virtual method arguments received from C++ to be passed
        // to the Python override.
        code.replace(QLatin1String("%PYTHON_ARGUMENTS"), QLatin1String(PYTHON_ARGS));

        // replace variable %PYTHON_METHOD_OVERRIDE for a pointer to the Python method
        // override for the C++ virtual method in which this piece of code was inserted
        code.replace(QLatin1String("%PYTHON_METHOD_OVERRIDE"), QLatin1String(PYTHON_OVERRIDE_VAR));
    }

    if (avoidProtectedHack()) {
        // If the function being processed was added by the user via type system,
        // Shiboken needs to find out if there are other overloads for the same method
        // name and if any of them is of the protected visibility. This is used to replace
        // calls to %FUNCTION_NAME on user written custom code for calls to the protected
        // dispatcher.
        bool isProtected = func->isProtected();
        auto owner = func->ownerClass();
        if (!isProtected && func->isUserAdded() && owner != nullptr) {
            const auto &funcs = getFunctionGroups(owner).value(func->name());
            isProtected = std::any_of(funcs.cbegin(), funcs.cend(),
                                      [](const AbstractMetaFunctionCPtr &f) {
                                          return f->isProtected();
                                      });
        }

        if (isProtected) {
            code.replace(QLatin1String("%TYPE::%FUNCTION_NAME"),
                         QStringLiteral("%1::%2_protected")
                         .arg(wrapperName(func->ownerClass()), func->originalName()));
            code.replace(QLatin1String("%FUNCTION_NAME"),
                         func->originalName() + QLatin1String("_protected"));
        }
    }

    if (func->isConstructor() && shouldGenerateCppWrapper(func->ownerClass()))
        code.replace(QLatin1String("%TYPE"), wrapperName(func->ownerClass()));

    if (func->ownerClass())
        code.replace(QLatin1String("%CPPTYPE"), func->ownerClass()->name());

    replaceTemplateVariables(code, func);

    processCodeSnip(code);
    s << "// Begin code injection\n" << code << "// End of code injection\n\n";
}

// Returns true if the string is an expression,
// and false if it is a variable.
static bool isVariable(const QString &code)
{
    static const QRegularExpression expr(QStringLiteral("^\\s*\\*?\\s*[A-Za-z_][A-Za-z_0-9.]*\\s*(?:\\[[^\\[]+\\])*$"));
    Q_ASSERT(expr.isValid());
    return expr.match(code.trimmed()).hasMatch();
}

// A miniature normalizer that puts a type string into a format
// suitable for comparison with AbstractMetaType::cppSignature()
// result.
static QString miniNormalizer(const QString &varType)
{
    QString normalized = varType.trimmed();
    if (normalized.isEmpty())
        return normalized;
    if (normalized.startsWith(QLatin1String("::")))
        normalized.remove(0, 2);
    QString suffix;
    while (normalized.endsWith(QLatin1Char('*')) || normalized.endsWith(QLatin1Char('&'))) {
        suffix.prepend(normalized.at(normalized.count() - 1));
        normalized.chop(1);
        normalized = normalized.trimmed();
    }
    const QString result = normalized + QLatin1Char(' ') + suffix;
    return result.trimmed();
}
// The position must indicate the first character after the opening '('.
// ATTENTION: do not modify this function to trim any resulting string!
// This must be done elsewhere.
static QString getConverterTypeSystemVariableArgument(const QString &code, int pos)
{
    QString arg;
    int parenthesisDepth = 0;
    int count = 0;
    while (pos + count < code.count()) {
        char c = code.at(pos+count).toLatin1(); // toAscii is gone
        if (c == '(') {
            ++parenthesisDepth;
        } else if (c == ')') {
            if (parenthesisDepth == 0) {
                arg = code.mid(pos, count).trimmed();
                break;
            }
            --parenthesisDepth;
        }
        ++count;
    }
    if (parenthesisDepth != 0)
        throw Exception("Unbalanced parenthesis on type system converter variable call.");
    return arg;
}

const QHash<int, QString> &ShibokenGenerator::typeSystemConvName()
{
    static const  QHash<int, QString> result = {
        {TypeSystemCheckFunction, QLatin1String("checkType")},
        {TypeSystemIsConvertibleFunction, QLatin1String("isConvertible")},
        {TypeSystemToCppFunction, QLatin1String("toCpp")},
        {TypeSystemToPythonFunction, QLatin1String("toPython")}
    };
    return result;
}

using StringPair = QPair<QString, QString>;

void ShibokenGenerator::replaceConverterTypeSystemVariable(TypeSystemConverterVariable converterVariable,
                                                           QString &code) const
{
    QList<StringPair> replacements;
    QRegularExpressionMatchIterator rit = typeSystemConvRegExps()[converterVariable].globalMatch(code);
    while (rit.hasNext()) {
        const QRegularExpressionMatch match = rit.next();
        const QStringList list = match.capturedTexts();
        QString conversionString = list.constFirst();
        const QString &conversionTypeName = list.constLast();
        QString message;
        const auto conversionTypeO = AbstractMetaType::fromString(conversionTypeName, &message);
        if (!conversionTypeO.has_value()) {
            throw Exception(msgCannotFindType(conversionTypeName,
                                              typeSystemConvName().value(converterVariable),
                                              message));
        }
        const auto conversionType = conversionTypeO.value();
        QString conversion;
        switch (converterVariable) {
            case TypeSystemToCppFunction: {
                StringStream c(TextStream::Language::Cpp);
                int end = match.capturedStart();
                int start = end;
                while (start > 0 && code.at(start) != QLatin1Char('\n'))
                    --start;
                while (code.at(start).isSpace())
                    ++start;
                QString varType = code.mid(start, end - start);
                conversionString = varType + list.constFirst();
                varType = miniNormalizer(varType);
                QString varName = list.at(1).trimmed();
                if (!varType.isEmpty()) {
                    const QString conversionSignature = conversionType.cppSignature();
                    if (varType != QLatin1String("auto") && varType != conversionSignature)
                        throw Exception(msgConversionTypesDiffer(varType, conversionSignature));
                    c << getFullTypeName(conversionType) << ' ' << varName
                        << minimalConstructorExpression(api(), conversionType) << ";\n";
                }
                c << cpythonToCppConversionFunction(conversionType);
                QString prefix;
                if (!AbstractMetaType::stripDereference(&varName))
                    prefix = QLatin1Char('&');
                QString arg = getConverterTypeSystemVariableArgument(code, match.capturedEnd());
                conversionString += arg;
                c << arg << ", " << prefix << '(' << varName << ')';
                conversion = c.toString();
                break;
            }
            case TypeSystemCheckFunction:
                conversion = cpythonCheckFunction(conversionType);
                if (conversionType.typeEntry()->isPrimitive()
                    && (conversionType.typeEntry()->name() == cPyObjectT()
                        || !conversion.endsWith(QLatin1Char(' ')))) {
                    conversion += u'(';
                    break;
                }
            Q_FALLTHROUGH();
            case TypeSystemIsConvertibleFunction:
                if (conversion.isEmpty())
                    conversion = cpythonIsConvertibleFunction(conversionType);
            Q_FALLTHROUGH();
            case TypeSystemToPythonFunction:
                if (conversion.isEmpty())
                    conversion = cpythonToPythonConversionFunction(conversionType);
            Q_FALLTHROUGH();
            default: {
                QString arg = getConverterTypeSystemVariableArgument(code, match.capturedEnd());
                conversionString += arg;
                if (converterVariable == TypeSystemToPythonFunction && !isVariable(arg)) {
                    QString m;
                    QTextStream(&m) << "Only variables are acceptable as argument to %%CONVERTTOPYTHON type system variable on code snippet: '"
                        << code << '\'';
                    throw Exception(m);
                }
                if (conversion.contains(QLatin1String("%in"))) {
                    conversion.prepend(QLatin1Char('('));
                    conversion.replace(QLatin1String("%in"), arg);
                } else {
                    conversion += arg;
                }
            }
        }
        replacements.append(qMakePair(conversionString, conversion));
    }
    for (const StringPair &rep : qAsConst(replacements))
        code.replace(rep.first, rep.second);
}

bool ShibokenGenerator::injectedCodeCallsCppFunction(const GeneratorContext &context,
                                                     const AbstractMetaFunctionCPtr &func)
{
    if (func->injectedCodeContains(u"%FUNCTION_NAME("))
        return true;
    QString funcCall = func->originalName() + QLatin1Char('(');
    if (func->isConstructor())
        funcCall.prepend(QLatin1String("new "));
    if (func->injectedCodeContains(funcCall))
        return true;
    if (!func->isConstructor())
        return false;
    if (func->injectedCodeContains(u"new %TYPE("))
        return true;
     const auto owner = func->ownerClass();
     if (!owner->isPolymorphic())
         return false;
    const QString className = context.useWrapper()
        ? context.wrapperName() : owner->qualifiedCppName();
    const QString wrappedCtorCall = QLatin1String("new ") + className + QLatin1Char('(');
    return func->injectedCodeContains(wrappedCtorCall);
}

bool ShibokenGenerator::useOverrideCaching(const AbstractMetaClass *metaClass)
{
    return metaClass->isPolymorphic();
}

ShibokenGenerator::AttroCheck ShibokenGenerator::checkAttroFunctionNeeds(const AbstractMetaClass *metaClass) const
{
    AttroCheck result;
    if (metaClass->typeEntry()->isSmartPointer()) {
        result |= AttroCheckFlag::GetattroSmartPointer | AttroCheckFlag::SetattroSmartPointer;
    } else {
        if (getGeneratorClassInfo(metaClass).needsGetattroFunction)
            result |= AttroCheckFlag::GetattroOverloads;
        if (metaClass->queryFirstFunction(metaClass->functions(),
                                          FunctionQueryOption::GetAttroFunction)) {
            result |= AttroCheckFlag::GetattroUser;
        }
        if (usePySideExtensions() && metaClass->qualifiedCppName() == qObjectT())
            result |= AttroCheckFlag::SetattroQObject;
        if (useOverrideCaching(metaClass))
            result |= AttroCheckFlag::SetattroMethodOverride;
        if (metaClass->queryFirstFunction(metaClass->functions(),
                                          FunctionQueryOption::SetAttroFunction)) {
            result |= AttroCheckFlag::SetattroUser;
        }
        // PYSIDE-1255: If setattro is generated for a class inheriting
        // QObject, the property code needs to be generated, too.
        if ((result & AttroCheckFlag::SetattroMask) != 0
            && !result.testFlag(AttroCheckFlag::SetattroQObject)
            && metaClass->isQObject()) {
            result |= AttroCheckFlag::SetattroQObject;
        }
    }
    return result;
}

bool ShibokenGenerator::classNeedsGetattroFunctionImpl(const AbstractMetaClass *metaClass)
{
    if (!metaClass)
        return false;
    if (metaClass->typeEntry()->isSmartPointer())
        return true;
    const auto &functionGroup = getFunctionGroups(metaClass);
    for (auto it = functionGroup.cbegin(), end = functionGroup.cend(); it != end; ++it) {
        AbstractMetaFunctionCList overloads;
        for (const auto &func : qAsConst(it.value())) {
            if (func->isAssignmentOperator() || func->isConversionOperator()
                || func->isModifiedRemoved()
                || func->isPrivate() || func->ownerClass() != func->implementingClass()
                || func->isConstructor() || func->isOperatorOverload())
                continue;
            overloads.append(func);
        }
        if (overloads.isEmpty())
            continue;
        if (OverloadData::hasStaticAndInstanceFunctions(overloads))
            return true;
    }
    return false;
}

AbstractMetaFunctionCList
    ShibokenGenerator::getMethodsWithBothStaticAndNonStaticMethods(const AbstractMetaClass *metaClass)
{
    AbstractMetaFunctionCList methods;
    if (metaClass) {
        const auto &functionGroups = getFunctionGroups(metaClass);
        for (auto it = functionGroups.cbegin(), end = functionGroups.cend(); it != end; ++it) {
            AbstractMetaFunctionCList overloads;
            for (const auto &func : qAsConst(it.value())) {
                if (func->isAssignmentOperator() || func->isConversionOperator()
                    || func->isModifiedRemoved()
                    || func->isPrivate() || func->ownerClass() != func->implementingClass()
                    || func->isConstructor() || func->isOperatorOverload())
                    continue;
                overloads.append(func);
            }
            if (overloads.isEmpty())
                continue;
            if (OverloadData::hasStaticAndInstanceFunctions(overloads))
                methods.append(overloads.constFirst());
        }
    }
    return methods;
}

const AbstractMetaClass *ShibokenGenerator::getMultipleInheritingClass(const AbstractMetaClass *metaClass)
{
    if (!metaClass || metaClass->baseClassNames().isEmpty())
        return nullptr;
    if (metaClass->baseClassNames().size() > 1)
        return metaClass;
    return getMultipleInheritingClass(metaClass->baseClass());
}

QString ShibokenGenerator::getModuleHeaderFileBaseName(const QString &moduleName)
{
    return moduleCppPrefix(moduleName).toLower() + QStringLiteral("_python");
}

QString ShibokenGenerator::getModuleHeaderFileName(const QString &moduleName)
{
    return getModuleHeaderFileBaseName(moduleName) + QStringLiteral(".h");
}

QString ShibokenGenerator::getPrivateModuleHeaderFileName(const QString &moduleName)
{
    return getModuleHeaderFileBaseName(moduleName) + QStringLiteral("_p.h");
}

/*
static void dumpFunction(AbstractMetaFunctionList lst)
{
    qDebug() << "DUMP FUNCTIONS: ";
    for (AbstractMetaFunction *func : qAsConst(lst))
        qDebug() << "*" << func->ownerClass()->name()
                        << func->signature()
                        << "Private: " << func->isPrivate()
                        << "Empty: " << func->isEmptyFunction()
                        << "Static:" << func->isStatic()
                        << "Signal:" << func->isSignal()
                        << "ClassImplements: " <<  (func->ownerClass() != func->implementingClass())
                        << "is operator:" << func->isOperatorOverload()
                        << "is global:" << func->isInGlobalScope();
}
*/

static bool isGroupable(const AbstractMetaFunctionCPtr &func)
{
    switch (func->functionType()) {
    case AbstractMetaFunction::DestructorFunction:
    case AbstractMetaFunction::SignalFunction:
    case AbstractMetaFunction::GetAttroFunction:
    case AbstractMetaFunction::SetAttroFunction:
    case AbstractMetaFunction::ArrowOperator: // weird operator overloads
    case AbstractMetaFunction::SubscriptOperator:
        return false;
    default:
        break;
    }
    if (func->isModifiedRemoved() && !func->isAbstract())
        return false;
    return true;
}

static void insertIntoFunctionGroups(const AbstractMetaFunctionCList &lst,
                                     ShibokenGenerator::FunctionGroups *results)
{
    for (const auto &func : lst) {
        if (isGroupable(func))
            (*results)[func->name()].append(func);
    }
}

ShibokenGenerator::FunctionGroups ShibokenGenerator::getGlobalFunctionGroups() const
{
    FunctionGroups results;
    insertIntoFunctionGroups(api().globalFunctions(), &results);
    for (auto nsp : invisibleTopNamespaces())
        insertIntoFunctionGroups(nsp->functions(), &results);
    return results;
}

const GeneratorClassInfoCacheEntry &ShibokenGenerator::getGeneratorClassInfo(const AbstractMetaClass *scope)
{
    auto cache = generatorClassInfoCache();
    auto it = cache->find(scope);
    if (it == cache->end()) {
        it = cache->insert(scope, {});
        it.value().functionGroups = getFunctionGroupsImpl(scope);
        it.value().needsGetattroFunction = classNeedsGetattroFunctionImpl(scope);
    }
    return it.value();
}

ShibokenGenerator::FunctionGroups ShibokenGenerator::getFunctionGroups(const AbstractMetaClass *scope)
{
    Q_ASSERT(scope);
    return getGeneratorClassInfo(scope).functionGroups;
}

// Use non-const overloads only, for example, "foo()" and "foo()const"
// the second is removed.
static void removeConstOverloads(AbstractMetaFunctionCList *overloads)
{
    for (qsizetype i = overloads->size() - 1; i >= 0; --i) {
        const auto &f = overloads->at(i);
        if (f->isConstant()) {
            for (qsizetype c = 0, size = overloads->size(); c < size; ++c) {
                if (f->isConstOverloadOf(overloads->at(c).data())) {
                    overloads->removeAt(i);
                    break;
                }
            }
        }
    }
}

ShibokenGenerator::FunctionGroups ShibokenGenerator::getFunctionGroupsImpl(const AbstractMetaClass *scope)
{
    AbstractMetaFunctionCList lst = scope->functions();
    scope->getFunctionsFromInvisibleNamespacesToBeGenerated(&lst);

    FunctionGroups results;
    for (const auto &func : lst) {
        if (isGroupable(func)
            && func->ownerClass() == func->implementingClass()
            && func->generateBinding()) {
            auto it = results.find(func->name());
            if (it == results.end()) {
                it = results.insert(func->name(), AbstractMetaFunctionCList(1, func));
            } else {
                // If there are virtuals methods in the mix (PYSIDE-570,
                // QFileSystemModel::index(QString,int) and
                // QFileSystemModel::index(int,int,QModelIndex)) override, make sure
                // the overriding method of the most-derived class is seen first
                // and inserted into the "seenSignatures" set.
                if (func->isVirtual())
                    it.value().prepend(func);
                else
                    it.value().append(func);
            }
            getInheritedOverloads(scope, &it.value());
            removeConstOverloads(&it.value());
        }
    }
    return results;
}

static bool hidesBaseClassFunctions(const AbstractMetaFunctionCPtr &f)
{
    return 0 == (f->attributes()
                 & (AbstractMetaFunction::OverriddenCppMethod | AbstractMetaFunction::FinalCppMethod));
}

void ShibokenGenerator::getInheritedOverloads(const AbstractMetaClass *scope,
                                             AbstractMetaFunctionCList *overloads)
{
    if (overloads->isEmpty() || scope->isNamespace() || scope->baseClasses().isEmpty())
        return;

    // PYSIDE-331: look also into base classes. Check for any non-overriding
    // function hiding the base class functions.
    const bool hideBaseClassFunctions =
        std::any_of(overloads->cbegin(), overloads->cend(), hidesBaseClassFunctions);

    const QString &functionName = overloads->constFirst()->name();
    const bool hasUsingDeclarations = scope->hasUsingMemberFor(functionName);
    if (hideBaseClassFunctions && !hasUsingDeclarations)
        return; // No base function is visible

    // Collect base candidates by name and signature
    bool staticEncountered = false;
    QSet<QString> seenSignatures;
    for (const auto &func : *overloads) {
        seenSignatures.insert(func->minimalSignature());
        staticEncountered |= func->isStatic();
    }

    AbstractMetaFunctionCList baseCandidates;

    auto basePredicate = [&functionName, &seenSignatures, &baseCandidates](const AbstractMetaClass *b) {
        for (const auto &f : b->functions()) {
            if (f->generateBinding() && f->name() == functionName) {
                const QString signature = f->minimalSignature();
                if (!seenSignatures.contains(signature)) {
                    seenSignatures.insert(signature);
                    baseCandidates.append(f);
                }
            }
        }
        return false; // Keep going
    };

    for (const auto *baseClass : scope->baseClasses())
        recurseClassHierarchy(baseClass, basePredicate);

    // Remove the ones that are not made visible with using declarations
    if (hideBaseClassFunctions && hasUsingDeclarations) {
        const auto pred =  [scope](const AbstractMetaFunctionCPtr &f) {
            return !scope->isUsingMember(f->ownerClass(), f->name(), f->access());
        };
        auto end = std::remove_if(baseCandidates.begin(), baseCandidates.end(), pred);
        baseCandidates.erase(end, baseCandidates.end());
    }

    // PYSIDE-886: If the method does not have any static overloads declared
    // in the class in question, remove all inherited static methods as setting
    // METH_STATIC in that case can cause crashes for the instance methods.
    // Manifested as crash when calling QPlainTextEdit::find() (clash with
    // static QWidget::find(WId)).
    if (!staticEncountered) {
        auto end =  std::remove_if(baseCandidates.begin(), baseCandidates.end(),
                                   [](const AbstractMetaFunctionCPtr &f) { return f->isStatic(); });
         baseCandidates.erase(end, baseCandidates.end());
    }

    for (const auto &baseCandidate : baseCandidates) {
        AbstractMetaFunction *newFunc = baseCandidate->copy();
        newFunc->setImplementingClass(scope);
        overloads->append(AbstractMetaFunctionCPtr(newFunc));
    }
}

Generator::OptionDescriptions ShibokenGenerator::options() const
{
    auto result = Generator::options();
    result.append({
        {QLatin1String(AVOID_PROTECTED_HACK),
         QLatin1String("Avoid the use of the '#define protected public' hack.")},
        {QLatin1String(DISABLE_VERBOSE_ERROR_MESSAGES),
         QLatin1String("Disable verbose error messages. Turn the python code hard to debug\n"
                       "but safe few kB on the generated bindings.")},
        {QLatin1String(PARENT_CTOR_HEURISTIC),
         QLatin1String("Enable heuristics to detect parent relationship on constructors.")},
        {QLatin1String(RETURN_VALUE_HEURISTIC),
         QLatin1String("Enable heuristics to detect parent relationship on return values\n"
                       "(USE WITH CAUTION!)")},
        {QLatin1String(USE_ISNULL_AS_NB_NONZERO),
         QLatin1String("If a class have an isNull() const method, it will be used to compute\n"
                       "the value of boolean casts")},
        {QLatin1String(USE_OPERATOR_BOOL_AS_NB_NONZERO),
         QLatin1String("If a class has an operator bool, it will be used to compute\n"
                       "the value of boolean casts")},
        {QLatin1String(NO_IMPLICIT_CONVERSIONS),
         u"Do not generate implicit_conversions for function arguments."_qs},
        {QLatin1String(WRAPPER_DIAGNOSTICS),
         QLatin1String("Generate diagnostic code around wrappers")}
    });
    return result;
}

bool ShibokenGenerator::handleOption(const QString &key, const QString &value)
{
    if (Generator::handleOption(key, value))
        return true;
    if (key == QLatin1String(PARENT_CTOR_HEURISTIC))
        return (m_useCtorHeuristic = true);
    if (key == QLatin1String(RETURN_VALUE_HEURISTIC))
        return (m_userReturnValueHeuristic = true);
    if (key == QLatin1String(DISABLE_VERBOSE_ERROR_MESSAGES))
        return (m_verboseErrorMessagesDisabled = true);
    if (key == QLatin1String(USE_ISNULL_AS_NB_NONZERO))
        return (m_useIsNullAsNbNonZero = true);
    if (key == QLatin1String(USE_OPERATOR_BOOL_AS_NB_NONZERO))
        return (m_useOperatorBoolAsNbNonZero = true);
    if (key == QLatin1String(AVOID_PROTECTED_HACK))
        return (m_avoidProtectedHack = true);
    if (key == QLatin1String(NO_IMPLICIT_CONVERSIONS)) {
        return m_generateImplicitConversions = false;
        return true;
    }
    if (key == QLatin1String(WRAPPER_DIAGNOSTICS))
        return (m_wrapperDiagnostics = true);
    return false;
}

static void getCode(QStringList &code, const CodeSnipList &codeSnips)
{
    for (const CodeSnip &snip : qAsConst(codeSnips))
        code.append(snip.code());
}

static void getCode(QStringList &code, const TypeEntry *type)
{
    getCode(code, type->codeSnips());

    CustomConversion *customConversion = type->customConversion();
    if (!customConversion)
        return;

    if (!customConversion->nativeToTargetConversion().isEmpty())
        code.append(customConversion->nativeToTargetConversion());

    const CustomConversion::TargetToNativeConversions &toCppConversions = customConversion->targetToNativeConversions();
    if (toCppConversions.isEmpty())
        return;

    for (CustomConversion::TargetToNativeConversion *toNative : qAsConst(toCppConversions))
        code.append(toNative->conversion());
}

bool ShibokenGenerator::doSetup()
{
    QStringList snips;
    const PrimitiveTypeEntryList &primitiveTypeList = primitiveTypes();
    for (const PrimitiveTypeEntry *type : primitiveTypeList)
        getCode(snips, type);
    const ContainerTypeEntryList &containerTypeList = containerTypes();
    for (const ContainerTypeEntry *type : containerTypeList)
        getCode(snips, type);
    for (auto metaClass : api().classes())
        getCode(snips, metaClass->typeEntry());

    const TypeSystemTypeEntry *moduleEntry = TypeDatabase::instance()->defaultTypeSystemType();
    Q_ASSERT(moduleEntry);
    getCode(snips, moduleEntry);

    const auto &functionGroups = getGlobalFunctionGroups();
    for (auto it = functionGroups.cbegin(), end = functionGroups.cend(); it != end; ++it) {
        for (const auto &func : it.value())
            getCode(snips, func->injectedCodeSnips());
    }

    for (const QString &code : qAsConst(snips)) {
        collectContainerTypesFromConverterMacros(code, true);
        collectContainerTypesFromConverterMacros(code, false);
    }

    return true;
}

void ShibokenGenerator::collectContainerTypesFromConverterMacros(const QString &code, bool toPythonMacro)
{
    QString convMacro = toPythonMacro ? QLatin1String("%CONVERTTOPYTHON[") : QLatin1String("%CONVERTTOCPP[");
    int offset = toPythonMacro ? sizeof("%CONVERTTOPYTHON") : sizeof("%CONVERTTOCPP");
    int start = 0;
    QString errorMessage;
    while ((start = code.indexOf(convMacro, start)) != -1) {
        int end = code.indexOf(QLatin1Char(']'), start);
        start += offset;
        if (code.at(start) != QLatin1Char('%')) {
            QString typeString = code.mid(start, end - start);
            auto type = AbstractMetaType::fromString(typeString, &errorMessage);
            if (type.has_value()) {
                addInstantiatedContainersAndSmartPointers(type.value(), type->originalTypeDescription());
            } else {
                QString m;
                QTextStream(&m) << __FUNCTION__ << ": Cannot translate type \""
                    << typeString << "\": " << errorMessage;
                throw Exception(m);
            }
        }
        start = end;
    }
}

bool ShibokenGenerator::useCtorHeuristic() const
{
    return m_useCtorHeuristic;
}

bool ShibokenGenerator::useReturnValueHeuristic() const
{
    return m_userReturnValueHeuristic;
}

bool ShibokenGenerator::useIsNullAsNbNonZero() const
{
    return m_useIsNullAsNbNonZero;
}

bool ShibokenGenerator::useOperatorBoolAsNbNonZero() const
{
    return m_useOperatorBoolAsNbNonZero;
}

bool ShibokenGenerator::avoidProtectedHack() const
{
    return m_avoidProtectedHack;
}

bool ShibokenGenerator::generateImplicitConversions() const
{
    return m_generateImplicitConversions;
}

QString ShibokenGenerator::moduleCppPrefix(const QString &moduleName)
 {
    QString result = moduleName.isEmpty() ? packageName() : moduleName;
    result.replace(QLatin1Char('.'), QLatin1Char('_'));
    return result;
}

QString ShibokenGenerator::cppApiVariableName(const QString &moduleName)
{
    return QLatin1String("Sbk") + moduleCppPrefix(moduleName)
        + QLatin1String("Types");
}

QString ShibokenGenerator::pythonModuleObjectName(const QString &moduleName)
{
    return QLatin1String("Sbk") + moduleCppPrefix(moduleName)
        + QLatin1String("ModuleObject");
}

QString ShibokenGenerator::convertersVariableName(const QString &moduleName)
{
    QString result = cppApiVariableName(moduleName);
    result.chop(1);
    result.append(QLatin1String("Converters"));
    return result;
}

static QString processInstantiationsVariableName(const AbstractMetaType &type)
{
    QString res = QLatin1Char('_') + _fixedCppTypeName(type.typeEntry()->qualifiedCppName()).toUpper();
    for (const auto &instantiation : type.instantiations()) {
        res += instantiation.isContainer()
               ? processInstantiationsVariableName(instantiation)
               : QLatin1Char('_') + _fixedCppTypeName(instantiation.cppSignature()).toUpper();
    }
    return res;
}

static void appendIndexSuffix(QString *s)
{
    if (!s->endsWith(QLatin1Char('_')))
        s->append(QLatin1Char('_'));
    s->append(QStringLiteral("IDX"));
}

QString ShibokenGenerator::getTypeAlternateTemplateIndexVariableName(const AbstractMetaClass *metaClass)
{
    const AbstractMetaClass *templateBaseClass = metaClass->templateBaseClass();
    Q_ASSERT(templateBaseClass);
    QString result = QLatin1String("SBK_")
        + _fixedCppTypeName(templateBaseClass->typeEntry()->qualifiedCppName()).toUpper();
    for (const auto &instantiation : metaClass->templateBaseClassInstantiations())
        result += processInstantiationsVariableName(instantiation);
    appendIndexSuffix(&result);
    return result;
}

QString ShibokenGenerator::getTypeIndexVariableName(const AbstractMetaClass *metaClass)
{
    return getTypeIndexVariableName(metaClass->typeEntry());
}
QString ShibokenGenerator::getTypeIndexVariableName(const TypeEntry *type)
{
    if (type->isCppPrimitive())
        type = type->asPrimitive()->basicReferencedTypeEntry();
    QString result = QLatin1String("SBK_");
    // Disambiguate namespaces per module to allow for extending them.
    if (type->isNamespace()) {
        QString package = type->targetLangPackage();
        const int dot = package.lastIndexOf(QLatin1Char('.'));
        result += QStringView{package}.right(package.size() - (dot + 1));
    }
    result += _fixedCppTypeName(type->qualifiedCppName()).toUpper();
    appendIndexSuffix(&result);
    return result;
}
QString ShibokenGenerator::getTypeIndexVariableName(const AbstractMetaType &type)
{
    QString result = QLatin1String("SBK");
    if (type.typeEntry()->isContainer())
        result += QLatin1Char('_') + moduleName().toUpper();
    result += processInstantiationsVariableName(type);
    appendIndexSuffix(&result);
    return result;
}

bool ShibokenGenerator::verboseErrorMessagesDisabled() const
{
    return m_verboseErrorMessagesDisabled;
}

bool ShibokenGenerator::pythonFunctionWrapperUsesListOfArguments(const AbstractMetaFunctionCPtr &func) const
{
    const auto &groups = func->implementingClass()
       ? getFunctionGroups(func->implementingClass())
       : getGlobalFunctionGroups();
    OverloadData od(groups.value(func->name()), api());
    return od.pythonFunctionWrapperUsesListOfArguments();
}

QString ShibokenGenerator::minimalConstructorExpression(const ApiExtractorResult &api,
                                                          const AbstractMetaType &type)
{
    if (type.isExtendedCppPrimitive() || type.isSmartPointer())
        return {};
    QString errorMessage;
    const auto ctor = minimalConstructor(api, type, &errorMessage);
    if (ctor.has_value())
        return ctor->initialization();

    const QString message =
        msgCouldNotFindMinimalConstructor(QLatin1String(__FUNCTION__),
                                          type.cppSignature(), errorMessage);
    qCWarning(lcShiboken()).noquote() << message;
    return u";\n#error "_qs + message + u'\n';
}

QString ShibokenGenerator::minimalConstructorExpression(const ApiExtractorResult &api,
                                                        const TypeEntry *type)
{
    if (type->isExtendedCppPrimitive())
        return {};
    const auto ctor = minimalConstructor(api, type);
    if (ctor.has_value())
        return ctor->initialization();

    const QString message = msgCouldNotFindMinimalConstructor(QLatin1String(__FUNCTION__),
                                                              type->qualifiedCppName());
    qCWarning(lcShiboken()).noquote() << message;
    return u";\n#error "_qs + message + u'\n';
}

QString ShibokenGenerator::pythonArgsAt(int i)
{
    return QLatin1String(PYTHON_ARGS) + QLatin1Char('[')
           + QString::number(i) + QLatin1Char(']');
}

void ShibokenGenerator::replaceTemplateVariables(QString &code,
                                                 const AbstractMetaFunctionCPtr &func) const
{
    const AbstractMetaClass *cpp_class = func->ownerClass();
    if (cpp_class)
        code.replace(QLatin1String("%TYPE"), cpp_class->name());

    const AbstractMetaArgumentList &argument = func->arguments();
    for (const AbstractMetaArgument &arg : argument)
        code.replace(QLatin1Char('%') + QString::number(arg.argumentIndex() + 1), arg.name());

    //template values
    code.replace(QLatin1String("%RETURN_TYPE"), translateType(func->type(), cpp_class));
    code.replace(QLatin1String("%FUNCTION_NAME"), func->originalName());

    if (code.contains(QLatin1String("%ARGUMENT_NAMES"))) {
        StringStream aux_stream;
        writeArgumentNames(aux_stream, func, Generator::SkipRemovedArguments);
        code.replace(QLatin1String("%ARGUMENT_NAMES"), aux_stream);
    }

    if (code.contains(QLatin1String("%ARGUMENTS"))) {
        StringStream aux_stream;
        writeFunctionArguments(aux_stream, func, Options(SkipDefaultValues) | SkipRemovedArguments);
        code.replace(QLatin1String("%ARGUMENTS"), aux_stream);
    }
}
