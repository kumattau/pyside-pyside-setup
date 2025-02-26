/****************************************************************************
**
** Copyright (C) 2020 The Qt Company Ltd.
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

#include "modifications.h"
#include "modifications_p.h"
#include "exception.h"
#include "typedatabase.h"
#include "typeparser.h"
#include "typesystem.h"

#include <QtCore/QDebug>

#include <algorithm>
#include <limits>

static inline QString callOperator() { return QStringLiteral("operator()"); }

QString TemplateInstance::expandCode() const
{
    TemplateEntry *templateEntry = TypeDatabase::instance()->findTemplate(m_name);
    if (!templateEntry) {
        const QString m = QLatin1String("<insert-template> referring to non-existing template '")
                          + m_name + QLatin1String("'.");
        throw Exception(m);
    }

    QString code = templateEntry->code();
    for (auto it = replaceRules.cbegin(), end = replaceRules.cend(); it != end; ++it)
        code.replace(it.key(), it.value());
    while (!code.isEmpty() && code.at(code.size() - 1).isSpace())
        code.chop(1);
    QString result = QLatin1String("// TEMPLATE - ") + m_name + QLatin1String(" - START");
    if (!code.startsWith(QLatin1Char('\n')))
        result += QLatin1Char('\n');
    result += code;
    result += QLatin1String("\n// TEMPLATE - ") + m_name + QLatin1String(" - END\n");
    return result;
}

// ---------------------- CodeSnipFragment
QString CodeSnipFragment::code() const
{
    return m_instance ? m_instance->expandCode() : m_code;
}

// ---------------------- CodeSnipAbstract
QString CodeSnipAbstract::code() const
{
    QString res;
    for (const CodeSnipFragment &codeFrag : codeList)
        res.append(codeFrag.code());

    return res;
}

void CodeSnipAbstract::addCode(const QString &code)
{
    codeList.append(CodeSnipFragment(fixSpaces(code)));
}

QRegularExpression CodeSnipAbstract::placeHolderRegex(int index)
{
    return QRegularExpression(QLatin1Char('%') + QString::number(index) + QStringLiteral("\\b"));
}

// ---------------------- Modification
QString FunctionModification::accessModifierString() const
{
    if (isPrivate()) return QLatin1String("private");
    if (isProtected()) return QLatin1String("protected");
    if (isPublic()) return QLatin1String("public");
    if (isFriendly()) return QLatin1String("friendly");
    return QString();
}

// ---------------------- FieldModification

class FieldModificationData : public QSharedData
{
public:
    QString m_name;
    QString m_renamedToName;
    bool m_readable = true;
    bool m_writable = true;
    bool m_removed = false;
    bool m_opaqueContainer = false;
    TypeSystem::SnakeCase snakeCase = TypeSystem::SnakeCase::Unspecified;
};

FieldModification::FieldModification() : d(new FieldModificationData)
{
}

FieldModification::FieldModification(const FieldModification &) = default;
FieldModification &FieldModification::operator=(const FieldModification &) = default;
FieldModification::FieldModification(FieldModification &&) = default;
FieldModification &FieldModification::operator=(FieldModification &&) = default;
FieldModification::~FieldModification() = default;

QString FieldModification::name() const
{
    return d->m_name;
}

void FieldModification::setName(const QString &value)
{
    if (d->m_name != value)
        d->m_name = value;
}

bool FieldModification::isRenameModifier() const
{
    return !d->m_renamedToName.isEmpty();
}

QString FieldModification::renamedToName() const
{
    return d->m_renamedToName;
}

void FieldModification::setRenamedToName(const QString &value)
{
    if (d->m_renamedToName != value)
        d->m_renamedToName = value;
}

bool FieldModification::isReadable() const
{
    return d->m_readable;
}

void FieldModification::setReadable(bool e)
{
    if (d->m_readable != e)
        d->m_readable = e;
}

bool FieldModification::isWritable() const
{
    return d->m_writable;
}

void FieldModification::setWritable(bool e)
{
    if (d->m_writable != e)
        d->m_writable = e;
}

bool FieldModification::isRemoved() const
{
    return d->m_removed;
}

void FieldModification::setRemoved(bool r)
{
    if (d->m_removed != r)
        d->m_removed = r;
}

bool FieldModification::isOpaqueContainer() const
{
    return d->m_opaqueContainer;
}

void FieldModification::setOpaqueContainer(bool r)
{
    if (d->m_opaqueContainer != r)
        d->m_opaqueContainer = r;
}

TypeSystem::SnakeCase FieldModification::snakeCase() const
{
    return d->snakeCase;
}

void FieldModification::setSnakeCase(TypeSystem::SnakeCase s)
{
    if (d->snakeCase != s)
        d->snakeCase = s;
}

// Helpers to split a parameter list of <add-function>, <declare-function>
// (@ denoting names), like
// "void foo(QList<X,Y> &@list@ = QList<X,Y>{1,2}, int @b@=5, ...)"
namespace AddedFunctionParser {

bool Argument::equals(const Argument &rhs) const
{
    return type == rhs.type && name == rhs.name && defaultValue == rhs.defaultValue;
}

#ifndef QT_NO_DEBUG_STREAM
QDebug operator<<(QDebug d, const Argument &a)
{
    QDebugStateSaver saver(d);
    d.noquote();
    d.nospace();
    d << "Argument(type=\"" << a.type << '"';
    if (!a.name.isEmpty())
        d << ", name=\"" << a.name << '"';
    if (!a.defaultValue.isEmpty())
        d << ", defaultValue=\"" << a.defaultValue << '"';
    d << ')';
    return d;
}
#endif // QT_NO_DEBUG_STREAM

// Helper for finding the end of a function parameter, observing
// nested template parameters or lists.
static int parameterTokenEnd(int startPos, QStringView paramString)
{
    const int end = paramString.size();
    int nestingLevel = 0;
    for (int p = startPos; p < end; ++p) {
        switch (paramString.at(p).toLatin1()) {
        case ',':
            if (nestingLevel == 0)
                return p;
            break;
        case '<': // templates
        case '{': // initializer lists of default values
        case '(': // initialization, function pointers
        case '[': // array dimensions
            ++nestingLevel;
            break;
        case '>':
        case '}':
        case ')':
        case ']':
            --nestingLevel;
            break;
        }
    }
    return end;
}

// Split a function parameter list into string tokens containing one
// parameters (including default value, etc).
static QList<QStringView> splitParameterTokens(QStringView paramString)
{
    QList<QStringView> result;
    int startPos = 0;
    for ( ; startPos < paramString.size(); ) {
        int end = parameterTokenEnd(startPos, paramString);
        result.append(paramString.mid(startPos, end - startPos).trimmed());
        startPos = end + 1;
    }
    return result;
}

// Split a function parameter list
Arguments splitParameters(QStringView paramString, QString *errorMessage)
{
    Arguments result;
    const QList<QStringView> tokens = splitParameterTokens(paramString);

    for (const auto &t : tokens) {
        Argument argument;
        // Check defaultValue, "int @b@=5"
        const int equalPos = t.lastIndexOf(QLatin1Char('='));
        if (equalPos != -1) {
            const int defaultValuePos = equalPos + 1;
            argument.defaultValue =
                t.mid(defaultValuePos, t.size() - defaultValuePos).trimmed().toString();
        }
        QString typeString = (equalPos != -1 ? t.left(equalPos) : t).trimmed().toString();
        // Check @name@
        const int atPos = typeString.indexOf(QLatin1Char('@'));
        if (atPos != -1) {
            const int namePos = atPos + 1;
            const int nameEndPos = typeString.indexOf(QLatin1Char('@'), namePos);
            if (nameEndPos == -1) {
                if (errorMessage != nullptr) {
                    *errorMessage = QLatin1String("Mismatched @ in \"")
                                    + paramString.toString() + QLatin1Char('"');
                }
                return {};
            }
            argument.name = typeString.mid(namePos, nameEndPos - namePos).trimmed();
            typeString.remove(atPos, nameEndPos - atPos + 1);
        }
        argument.type = typeString.trimmed();
        result.append(argument);
    }

    return result;
}

} // namespace AddedFunctionParser

AddedFunction::AddedFunction(const QString &name, const QList<Argument> &arguments,
                             const TypeInfo &returnType) :
    m_name(name),
    m_arguments(arguments),
    m_returnType(returnType)
{
}

AddedFunction::AddedFunctionPtr
    AddedFunction::createAddedFunction(const QString &signatureIn, const QString &returnTypeIn,
                                       QString *errorMessage)

{
    errorMessage->clear();

    QList<Argument> arguments;
    const TypeInfo returnType = returnTypeIn.isEmpty()
                                ? TypeInfo::voidType()
                                : TypeParser::parse(returnTypeIn, errorMessage);
    if (!errorMessage->isEmpty())
        return {};

    QStringView signature = QStringView{signatureIn}.trimmed();

    // Skip past "operator()(...)"
    const int parenSearchStartPos = signature.startsWith(callOperator())
        ? callOperator().size() : 0;
    const int openParenPos = signature.indexOf(QLatin1Char('('), parenSearchStartPos);
    if (openParenPos < 0) {
        return AddedFunctionPtr(new AddedFunction(signature.toString(),
                                                  arguments, returnType));
    }

    const QString name = signature.left(openParenPos).trimmed().toString();
    const int closingParenPos = signature.lastIndexOf(QLatin1Char(')'));
    if (closingParenPos < 0) {
        *errorMessage = QLatin1String("Missing closing parenthesis");
        return {};
    }

    // Check for "foo() const"
    bool isConst = false;
    const int signatureLength = signature.length();
    const int qualifierLength = signatureLength - closingParenPos - 1;
    if (qualifierLength >= 5
        && signature.right(qualifierLength).contains(QLatin1String("const"))) {
        isConst = true;
    }

    const auto paramString = signature.mid(openParenPos + 1, closingParenPos - openParenPos - 1);
    const auto params = AddedFunctionParser::splitParameters(paramString, errorMessage);
    if (params.isEmpty() && !errorMessage->isEmpty())
        return {};
    for (const auto &p : params) {
        TypeInfo type = p.type == QLatin1String("...")
            ? TypeInfo::varArgsType() : TypeParser::parse(p.type, errorMessage);
        if (!errorMessage->isEmpty()) {
            errorMessage->prepend(u"Unable to parse added function "_qs + signatureIn
                                  + u": "_qs);
            return {};
        }
        arguments.append({type, p.name, p.defaultValue});
    }

    AddedFunctionPtr result(new AddedFunction(name, arguments, returnType));
    result->setConstant(isConst);
    return result;
}

// Remove the parameter names enclosed in '@' from an added function signature
// so that it matches the C++ type signature.
static QString removeParameterNames(QString signature)
{
    while (true) {
        const auto ampPos = signature.indexOf(u'@');
        if (ampPos == -1)
            break;
        const auto closingAmpPos = signature.indexOf(u'@', ampPos + 1);
        if (closingAmpPos == -1)
            break;
        signature.remove(ampPos, closingAmpPos - ampPos + 1);
    }
    return signature;
}

DocModification::DocModification(const QString &xpath, const QString &signature) :
    m_xpath(xpath), m_signature(removeParameterNames(signature))
{
}

DocModification::DocModification(TypeSystem::DocModificationMode mode, const QString &signature) :
    m_signature(removeParameterNames(signature)), m_mode(mode)
{
}

void DocModification::setCode(const QString &code)
{
    m_code = CodeSnipAbstract::fixSpaces(code);
}

#ifndef QT_NO_DEBUG_STREAM
QDebug operator<<(QDebug d, const ReferenceCount &r)
{
    QDebugStateSaver saver(d);
    d.noquote();
    d.nospace();
    d << "ReferenceCount(" << r.varName << ", action=" << r.action << ')';
    return d;
}

QDebug operator<<(QDebug d, const CodeSnip &s)
{
    QDebugStateSaver saver(d);
    d.noquote();
    d.nospace();
    d << "CodeSnip(language=" << s.language << ", position=" << s.position << ", \"";
    for (const auto &f : s.codeList) {
        const QString &code = f.code();
        const auto lines = QStringView{code}.split(QLatin1Char('\n'));
        for (int i = 0, size = lines.size(); i < size; ++i) {
            if (i)
                d << "\\n";
            d << lines.at(i).trimmed();
        }
    }
    d << "\")";
    return d;
}

class ArgumentModificationData : public QSharedData
{
public:
    ArgumentModificationData(int idx = -1) :  index(idx),
        removedDefaultExpression(false), removed(false),
        noNullPointers(false), resetAfterUse(false), array(false)
    {}

    QList<ReferenceCount> referenceCounts;
    QString modified_type;
    QString pyiType;
    QString replacedDefaultExpression;
    TypeSystem::Ownership m_targetOwnerShip = TypeSystem::UnspecifiedOwnership;
    TypeSystem::Ownership m_nativeOwnerShip = TypeSystem::UnspecifiedOwnership;
    CodeSnipList conversion_rules;
    ArgumentOwner owner;
    QString renamed_to;
    int index = -1;
    uint removedDefaultExpression : 1;
    uint removed : 1;
    uint noNullPointers : 1;
    uint resetAfterUse : 1;
    uint array : 1;
};

ArgumentModification::ArgumentModification() : d(new ArgumentModificationData)
{
}

ArgumentModification::ArgumentModification(int idx) : d(new ArgumentModificationData(idx))
{
}

ArgumentModification::ArgumentModification(const ArgumentModification &) = default;
ArgumentModification &ArgumentModification::operator=(const ArgumentModification &) = default;
ArgumentModification::ArgumentModification(ArgumentModification &&) = default;
ArgumentModification &ArgumentModification::operator=(ArgumentModification &&) = default;
ArgumentModification::~ArgumentModification() = default;

const QString &ArgumentModification::modifiedType() const
{
    return d->modified_type;
}

void ArgumentModification::setModifiedType(const QString &value)
{
    if (d->modified_type != value)
        d->modified_type = value;
}

bool ArgumentModification::isTypeModified() const
{
    return !d->modified_type.isEmpty();
}

QString ArgumentModification::pyiType() const
{
    return d->pyiType;
}

void ArgumentModification::setPyiType(const QString &value)
{
    if (d->pyiType != value)
        d->pyiType = value;
}

QString ArgumentModification::replacedDefaultExpression() const
{
    return d->replacedDefaultExpression;
}

void ArgumentModification::setReplacedDefaultExpression(const QString &value)
{
    if (d->replacedDefaultExpression != value)
        d->replacedDefaultExpression = value;
}

TypeSystem::Ownership ArgumentModification::targetOwnerShip() const
{
    return d->m_targetOwnerShip;
}

void ArgumentModification::setTargetOwnerShip(TypeSystem::Ownership o)
{
    if (o != d->m_targetOwnerShip)
        d->m_targetOwnerShip = o;
}

TypeSystem::Ownership ArgumentModification::nativeOwnership() const
{
    return d->m_nativeOwnerShip;
}

void ArgumentModification::setNativeOwnership(TypeSystem::Ownership o)
{
    if (o != d->m_nativeOwnerShip)
        d->m_nativeOwnerShip = o;
}

const CodeSnipList &ArgumentModification::conversionRules() const
{
    return d->conversion_rules;
}

CodeSnipList &ArgumentModification::conversionRules()
{
    return d->conversion_rules;
}

ArgumentOwner ArgumentModification::owner() const
{
    return d->owner;
}

void ArgumentModification::setOwner(const ArgumentOwner &value)
{
    d->owner = value;
}

QString ArgumentModification::renamedToName() const
{
    return d->renamed_to;
}

void ArgumentModification::setRenamedToName(const QString &value)
{
    if (d->renamed_to != value)
        d->renamed_to = value;
}

int ArgumentModification::index() const
{
    return d->index;
}

void ArgumentModification::setIndex(int value)
{
    if (d->index != value)
        d->index = value;
}

bool ArgumentModification::removedDefaultExpression() const
{
    return d->removedDefaultExpression;
}

void ArgumentModification::setRemovedDefaultExpression(const uint &value)
{
    if (d->removedDefaultExpression != value)
        d->removedDefaultExpression = value;
}

bool ArgumentModification::isRemoved() const
{
    return d->removed;
}

void ArgumentModification::setRemoved(bool value)
{
    if (d->removed != value)
        d->removed = value;
}

bool ArgumentModification::noNullPointers() const
{
    return d->noNullPointers;
}

void ArgumentModification::setNoNullPointers(bool value)
{
    if (d->noNullPointers != value)
        d->noNullPointers = value;
}

bool ArgumentModification::resetAfterUse() const
{
    return d->resetAfterUse;
}

void ArgumentModification::setResetAfterUse(bool value)
{
    if (d->resetAfterUse != value)
        d->resetAfterUse = value;
}

bool ArgumentModification::isArray() const
{
    return d->array;
}

void ArgumentModification::setArray(bool value)
{
    if (d->array != value)
        d->array = value;
}

const QList<ReferenceCount> &ArgumentModification::referenceCounts() const
{
    return d->referenceCounts;
}

void ArgumentModification::addReferenceCount(const ReferenceCount &value)
{
    d->referenceCounts.append(value);
}

class FunctionModificationData : public QSharedData
{
public:
    QString renamedToName;
    FunctionModification::Modifiers modifiers;
    CodeSnipList m_snips;
    QList<ArgumentModification> m_argument_mods;
    QString m_signature;
    QString m_originalSignature;
    QRegularExpression m_signaturePattern;
    int m_overloadNumber = TypeSystem::OverloadNumberUnset;
    bool m_thread = false;
    bool removed = false;
    TypeSystem::AllowThread m_allowThread = TypeSystem::AllowThread::Unspecified;
    TypeSystem::ExceptionHandling m_exceptionHandling = TypeSystem::ExceptionHandling::Unspecified;
    TypeSystem::SnakeCase snakeCase = TypeSystem::SnakeCase::Unspecified;
};

FunctionModification::FunctionModification() : d(new FunctionModificationData)
{
}

FunctionModification::FunctionModification(const FunctionModification &) = default;
FunctionModification &FunctionModification::operator=(const FunctionModification &) = default;
FunctionModification::FunctionModification(FunctionModification &&) = default;
FunctionModification &FunctionModification::operator=(FunctionModification &&) = default;
FunctionModification::~FunctionModification() = default;

void FunctionModification::formatDebug(QDebug &debug) const
{
    if (d->m_signature.isEmpty())
        debug << "pattern=\"" << d->m_signaturePattern.pattern();
    else
        debug << "signature=\"" << d->m_signature;
    debug << "\", modifiers=" << d->modifiers;
    if (d->removed)
        debug << ", removed";
    if (!d->renamedToName.isEmpty())
        debug << ", renamedToName=\"" << d->renamedToName << '"';
    if (d->m_allowThread != TypeSystem::AllowThread::Unspecified)
        debug << ", allowThread=" << int(d->m_allowThread);
    if (d->m_thread)
        debug << ", thread";
    if (d->m_exceptionHandling != TypeSystem::ExceptionHandling::Unspecified)
        debug << ", exceptionHandling=" << int(d->m_exceptionHandling);
    if (!d->m_snips.isEmpty())
        debug << ", snips=(" << d->m_snips << ')';
    if (!d->m_argument_mods.isEmpty())
        debug << ", argument_mods=(" << d->m_argument_mods << ')';
}

QString FunctionModification::renamedToName() const
{
    return d->renamedToName;
}

void FunctionModification::setRenamedToName(const QString &value)
{
    if (d->renamedToName != value)
        d->renamedToName = value;
}

FunctionModification::Modifiers FunctionModification::modifiers() const
{
    return d->modifiers;
}

void FunctionModification::setModifiers(Modifiers m)
{
    if (d->modifiers != m)
        d->modifiers = m;
}

void FunctionModification::setModifierFlag(FunctionModification::ModifierFlag f)
{
    auto newMods = d->modifiers | f;
    if (d->modifiers != newMods)
        d->modifiers = newMods;
}

void FunctionModification::clearModifierFlag(ModifierFlag f)
{
    auto newMods = d->modifiers & ~f;
    if (d->modifiers != newMods)
        d->modifiers = newMods;
}

bool FunctionModification::isRemoved() const
{
    return d->removed;
}

void FunctionModification::setRemoved(bool r)
{
    if (d->removed != r)
        d->removed = r;
}

const QList<ArgumentModification> &FunctionModification::argument_mods() const
{
    return d->m_argument_mods;
}

QList<ArgumentModification> &FunctionModification::argument_mods()
{
    return d->m_argument_mods;
}

void FunctionModification::setArgument_mods(const QList<ArgumentModification> &argument_mods)
{
    d->m_argument_mods = argument_mods;
}

TypeSystem::SnakeCase FunctionModification::snakeCase() const
{
    return d->snakeCase;
}

void FunctionModification::setSnakeCase(TypeSystem::SnakeCase s)
{
    if (d->snakeCase != s)
        d->snakeCase = s;
}

const CodeSnipList &FunctionModification::snips() const
{
    return d->m_snips;
}

CodeSnipList &FunctionModification::snips()
{
    return d->m_snips;
}

void FunctionModification::appendSnip(const CodeSnip &snip)
{
    d->m_snips.append(snip);
}

void FunctionModification::setSnips(const CodeSnipList &snips)
{
    d->m_snips = snips;
}

// ---------------------- FunctionModification
void FunctionModification::setIsThread(bool flag)
{
    if (d->m_thread != flag)
        d->m_thread = flag;
}

bool FunctionModification::isThread() const
{
    return d->m_thread;
}

FunctionModification::AllowThread FunctionModification::allowThread() const
{
    return d->m_allowThread;
}

void FunctionModification::setAllowThread(FunctionModification::AllowThread allow)
{
    if (d->m_allowThread != allow)
        d->m_allowThread = allow;
}

bool FunctionModification::matches(const QString &functionSignature) const
{
    return d->m_signature.isEmpty()
            ? d->m_signaturePattern.match(functionSignature).hasMatch()
            : d->m_signature == functionSignature;
}

bool FunctionModification::setSignature(const QString &s, QString *errorMessage)
{
    if (s.startsWith(QLatin1Char('^'))) {
        d->m_signaturePattern.setPattern(s);
        if (!d->m_signaturePattern.isValid()) {
            if (errorMessage) {
                *errorMessage = QLatin1String("Invalid signature pattern: \"")
                    + s + QLatin1String("\": ") + d->m_signaturePattern.errorString();
            }
            return false;
        }
    } else {
        d->m_signature = s;
    }
    return true;
}

QString FunctionModification::signature() const
{
    return d->m_signature.isEmpty() ? d->m_signaturePattern.pattern() : d->m_signature;
}

void FunctionModification::setOriginalSignature(const QString &s)
{
    if (d->m_originalSignature != s)
        d->m_originalSignature = s;
}

QString FunctionModification::originalSignature() const
{
    return d->m_originalSignature;
}

TypeSystem::ExceptionHandling FunctionModification::exceptionHandling() const
{
    return d->m_exceptionHandling;
}

void FunctionModification::setExceptionHandling(TypeSystem::ExceptionHandling e)
{
    if (d->m_exceptionHandling != e)
        d->m_exceptionHandling = e;
}

int FunctionModification::overloadNumber() const
{
    return d->m_overloadNumber;
}

void FunctionModification::setOverloadNumber(int overloadNumber)
{
    d->m_overloadNumber = overloadNumber;
}

QDebug operator<<(QDebug d, const ArgumentOwner &a)
{
    QDebugStateSaver saver(d);
    d.noquote();
    d.nospace();
    d << "ArgumentOwner(index=" << a.index << ", action=" << a.action << ')';
    return d;
}

QDebug operator<<(QDebug d, const ArgumentModification &a)
{
    QDebugStateSaver saver(d);
    d.noquote();
    d.nospace();
    d << "ArgumentModification(index=" << a.index();
    if (a.removedDefaultExpression())
        d << ", removedDefaultExpression";
    if (a.isRemoved())
        d << ", removed";
    if (a.noNullPointers())
        d << ", noNullPointers";
    if (a.isArray())
        d << ", array";
    if (!a.referenceCounts().isEmpty())
        d << ", referenceCounts=" << a.referenceCounts();
    if (!a.modifiedType().isEmpty())
        d << ", modified_type=\"" << a.modifiedType() << '"';
    if (!a.replacedDefaultExpression().isEmpty())
        d << ", replacedDefaultExpression=\"" << a.replacedDefaultExpression() << '"';
    if (a.targetOwnerShip() != TypeSystem::UnspecifiedOwnership)
        d << ", target ownership=" << a.targetOwnerShip();
    if (a.nativeOwnership() != TypeSystem::UnspecifiedOwnership)
        d << ", native ownership=" << a.nativeOwnership();
    if (!a.renamedToName().isEmpty())
        d << ", renamed_to=\"" << a.renamedToName() << '"';
     d << ", owner=" << a.owner() << ')';
    return  d;
}

QDebug operator<<(QDebug d, const FunctionModification &fm)
{
    QDebugStateSaver saver(d);
    d.noquote();
    d.nospace();
    d << "FunctionModification(";
    fm.formatDebug(d);
    d << ')';
    return d;
}

QDebug operator<<(QDebug d, const AddedFunction::Argument &a)
{
    QDebugStateSaver saver(d);
    d.noquote();
    d.nospace();
    d << "Argument(";
    d << a.typeInfo;
    if (!a.name.isEmpty())
        d << ' ' << a.name;
    if (!a.defaultValue.isEmpty())
        d << " = " << a.defaultValue;
    d << ')';
    return d;
}

QDebug operator<<(QDebug d, const AddedFunction &af)
{
    QDebugStateSaver saver(d);
    d.noquote();
    d.nospace();
    d << "AddedFunction(";
    if (af.access() == AddedFunction::Protected)
        d << "protected";
    if (af.isStatic())
        d << " static";
    d << af.returnType() << ' ' << af.name() << '(' << af.arguments() << ')';
    if (af.isConstant())
        d << " const";
    if (af.isDeclaration())
        d << " [declaration]";
    return d;
}
#endif // !QT_NO_DEBUG_STREAM
