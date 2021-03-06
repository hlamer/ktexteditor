/* This file is part of the KDE libraries
   Copyright (C) 2007 Matthew Woehlke <mw_triad@users.sourceforge.net>
   Copyright (C) 2003, 2004 Anders Lund <anders@alweb.dk>
   Copyright (C) 2003 Hamish Rodda <rodda@kde.org>
   Copyright (C) 2001,2002 Joseph Wenninger <jowenn@kde.org>
   Copyright (C) 2001 Christoph Cullmann <cullmann@kde.org>
   Copyright (C) 1999 Jochen Wilhelmy <digisnap@cs.tu-berlin.de>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/

//BEGIN INCLUDES
#include "katesyntaxmanager.h"

#include "katetextline.h"
#include "katedocument.h"
#include "katesyntaxdocument.h"
#include "katerenderer.h"
#include "kateglobal.h"
#include "kateschema.h"
#include "kateconfig.h"
#include "kateextendedattribute.h"
#include "katehighlight.h"
#include "katepartdebug.h"
#include "katedefaultcolors.h"

#include <KConfigGroup>
#include <KColorUtils>
#include <KMessageBox>

#include <QSet>
#include <QAction>
#include <QStringList>
#include <QTextStream>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QXmlStreamReader>
//END

using namespace KTextEditor;

bool compareKateHighlighting(const KateHighlighting *const left, const KateHighlighting *const right)
{
    int comparison = left->section().compare(right->section(), Qt::CaseInsensitive);
    if (comparison == 0) {
        comparison = left->nameTranslated().compare(right->nameTranslated(), Qt::CaseInsensitive);
    }
    return comparison < 0;
}

//BEGIN KateHlManager
KateHlManager::KateHlManager()
    : QObject()
    , m_config(KTextEditor::EditorPrivate::unitTestMode() ? QString() :QStringLiteral("katesyntaxhighlightingrc")
        , KTextEditor::EditorPrivate::unitTestMode() ? KConfig::SimpleConfig : KConfig::NoGlobals) // skip config for unit tests!
    , commonSuffixes({QStringLiteral(".orig"), QStringLiteral(".new"), QStringLiteral("~"), QStringLiteral(".bak"), QStringLiteral(".BAK")})
    , dynamicCtxsCount(0)
    , forceNoDCReset(false)
{
    // Let's build the Mode List
    setupModeList();

    lastCtxsReset.start();
}

KateHlManager::~KateHlManager()
{
    qDeleteAll(hlList);
    qDeleteAll(myModeList);
}

void KateHlManager::setupModeList()
{
    // only allow each name once!
    QHash<QString, KateSyntaxModeListItem *> hlNames;

    /**
     * first: use the index file in the resource
     */
    QFile indexJson (QLatin1String(":/ktexteditor/syntax/index.json"));
    if (indexJson.open(QFile::ReadOnly)) {
        /**
         * parse the whole file
         */
        const QJsonDocument indexDoc (QJsonDocument::fromBinaryData(indexJson.readAll()));

        /**
         * iterate over all hls in the index
         */
        const QJsonObject index = indexDoc.object();
        for (auto it = index.begin(); it != index.end(); ++it) {
            if (!it.value().isObject()) {
                continue;
            }
            // get map
            const QJsonObject map = it.value().toObject();

            // get name, only allow hls once!
            const QString name = map[QStringLiteral("name")].toString();

            // let's make the mode list item.
            KateSyntaxModeListItem *mli = new KateSyntaxModeListItem;

            mli->name      = name;
            mli->section   = map[QStringLiteral("section")].toString();
            mli->mimetype  = map[QStringLiteral("mimetype")].toString();
            mli->extension = map[QStringLiteral("extensions")].toString();
            mli->version   = map[QStringLiteral("version")].toString();
            mli->priority  = map[QStringLiteral("priority")].toString();
            mli->style     = map[QStringLiteral("style")].toString();
            mli->author    = map[QStringLiteral("author")].toString();
            mli->license   = map[QStringLiteral("license")].toString();
            mli->indenter  = map[QStringLiteral("indenter")].toString();
            mli->hidden    = map[QStringLiteral("hidden")].toBool();

            mli->identifier = QLatin1String(":/ktexteditor/syntax/") + it.key();

            // translate section + name
            mli->section    = i18nc("Language Section", mli->section.toUtf8().data());
            mli->nameTranslated = i18nc("Language", mli->name.toUtf8().data());

            // check for existance
            if (hlNames.contains(name)) {
                // replace old with new version
                if (hlNames[name]->version.toDouble() < mli->version.toDouble()) {
                    *hlNames[name] = *mli;
                }
                delete mli;
                continue;
            }

            // Append the new item to the list.
            myModeList.append(mli);
            hlNames[name] = mli;
        }
    }

    /**
     * now: process all xml files on the filesystem!
     * e.g. stuff downloaded in the $HOME or additional hl files from 3rdparty apps/plugins
     */
    QSet<QString> xmlFiles;
    const QStringList dirs = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("katepart5/syntax"), QStandardPaths::LocateDirectory);
    foreach (const QString &dir, dirs) {
        const QStringList fileNames = QDir(dir).entryList(QStringList() << QStringLiteral("*.xml"));
        foreach (const QString &file, fileNames) {
            xmlFiles.insert(dir + QLatin1Char('/') + file);
        }
    }
    Q_FOREACH (const QString xmlFile, xmlFiles) {
        // We're forced to read the xml files or the mode doesn't exist in the katesyntax...rc
        QFile f(xmlFile);
        if (!f.open(QIODevice::ReadOnly))
            continue;

        // read file as stream
        QXmlStreamReader xml(&f);
        if (!xml.readNextStartElement() || (xml.name() != QLatin1String("language")))
            continue;

        // get name, only allow hls once!
        const QString name = xml.attributes().value(QStringLiteral("name")).toString();

        // let's make the mode list item.
        KateSyntaxModeListItem *mli = new KateSyntaxModeListItem;

        mli->name      = name;
        mli->section   = xml.attributes().value(QStringLiteral("section")).toString();
        mli->mimetype  = xml.attributes().value(QStringLiteral("mimetype")).toString();
        mli->extension = xml.attributes().value(QStringLiteral("extensions")).toString();
        mli->version   = xml.attributes().value(QStringLiteral("version")).toString();
        mli->priority  = xml.attributes().value(QStringLiteral("priority")).toString();
        mli->style     = xml.attributes().value(QStringLiteral("style")).toString();
        mli->author    = xml.attributes().value(QStringLiteral("author")).toString();
        mli->license   = xml.attributes().value(QStringLiteral("license")).toString();
        mli->indenter  = xml.attributes().value(QStringLiteral("indenter")).toString();

        QString hidden = xml.attributes().value(QStringLiteral("hidden")).toString();
        mli->hidden    = (hidden == QLatin1String("true") || hidden == QLatin1String("1"));

        mli->identifier = xmlFile;

        // translate section + name
        mli->section    = i18nc("Language Section", mli->section.toUtf8().data());
        mli->nameTranslated = i18nc("Language", mli->name.toUtf8().data());

        // check for existance
        if (hlNames.contains(name)) {
            // replace old with new version
            if (hlNames[name]->version.toDouble() < mli->version.toDouble()) {
                *hlNames[name] = *mli;
            }
            delete mli;
            continue;
        }

        // Append the new item to the list.
        myModeList.append(mli);
        hlNames[name] = mli;
    }

    hlList.reserve(myModeList.size() + 1);
    hlDict.reserve(myModeList.size() + 1);
    for (int i = 0; i < myModeList.count(); i++) {
        KateHighlighting *hl = new KateHighlighting(myModeList[i]);

        hlList.insert(qLowerBound(hlList.begin(), hlList.end(), hl, compareKateHighlighting), hl);
        hlDict.insert(hl->name(), hl);
    }

    // Normal HL
    KateHighlighting *hl = new KateHighlighting(0);
    hlList.prepend(hl);
    hlDict.insert(hl->name(), hl);
}

KateHlManager *KateHlManager::self()
{
    return KTextEditor::EditorPrivate::self()->hlManager();
}

KateHighlighting *KateHlManager::getHl(int n)
{
    if (n < 0 || n >= hlList.count()) {
        n = 0;
    }

    return hlList.at(n);
}

int KateHlManager::nameFind(const QString &name)
{
    for (int i = 0; i < hlList.count(); ++i) {
        if (hlList.at(i)->name().compare(name, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }

    return -1;
}

int KateHlManager::defaultStyleCount()
{
    return KTextEditor::dsError;
}

QString KateHlManager::defaultStyleName(int n, bool translateNames)
{
    static QStringList names;
    static QStringList translatedNames;

    if (names.isEmpty()) {
        names << QStringLiteral("Normal");
        names << QStringLiteral("Keyword");
        names << QStringLiteral("Function");
        names << QStringLiteral("Variable");
        names << QStringLiteral("Control Flow");
        names << QStringLiteral("Operator");
        names << QStringLiteral("Built-in");
        names << QStringLiteral("Extension");
        names << QStringLiteral("Preprocessor");
        names << QStringLiteral("Attribute");

        names << QStringLiteral("Character");
        names << QStringLiteral("Special Character");
        names << QStringLiteral("String");
        names << QStringLiteral("Verbatim String");
        names << QStringLiteral("Special String");
        names << QStringLiteral("Import");

        names << QStringLiteral("Data Type");
        names << QStringLiteral("Decimal/Value");
        names << QStringLiteral("Base-N Integer");
        names << QStringLiteral("Floating Point");
        names << QStringLiteral("Constant");

        names << QStringLiteral("Comment");
        names << QStringLiteral("Documentation");
        names << QStringLiteral("Annotation");
        names << QStringLiteral("Comment Variable");
        // this next one is for denoting the beginning/end of a user defined folding region
        names << QStringLiteral("Region Marker");
        names << QStringLiteral("Information");
        names << QStringLiteral("Warning");
        names << QStringLiteral("Alert");

        names << QStringLiteral("Others");
        // this one is for marking invalid input
        names << QStringLiteral("Error");

        translatedNames << i18nc("@item:intable Text context", "Normal");
        translatedNames << i18nc("@item:intable Text context", "Keyword");
        translatedNames << i18nc("@item:intable Text context", "Function");
        translatedNames << i18nc("@item:intable Text context", "Variable");
        translatedNames << i18nc("@item:intable Text context", "Control Flow");
        translatedNames << i18nc("@item:intable Text context", "Operator");
        translatedNames << i18nc("@item:intable Text context", "Built-in");
        translatedNames << i18nc("@item:intable Text context", "Extension");
        translatedNames << i18nc("@item:intable Text context", "Preprocessor");
        translatedNames << i18nc("@item:intable Text context", "Attribute");

        translatedNames << i18nc("@item:intable Text context", "Character");
        translatedNames << i18nc("@item:intable Text context", "Special Character");
        translatedNames << i18nc("@item:intable Text context", "String");
        translatedNames << i18nc("@item:intable Text context", "Verbatim String");
        translatedNames << i18nc("@item:intable Text context", "Special String");
        translatedNames << i18nc("@item:intable Text context", "Imports, Modules, Includes");

        translatedNames << i18nc("@item:intable Text context", "Data Type");
        translatedNames << i18nc("@item:intable Text context", "Decimal/Value");
        translatedNames << i18nc("@item:intable Text context", "Base-N Integer");
        translatedNames << i18nc("@item:intable Text context", "Floating Point");
        translatedNames << i18nc("@item:intable Text context", "Constant");

        translatedNames << i18nc("@item:intable Text context", "Comment");
        translatedNames << i18nc("@item:intable Text context", "Documentation");
        translatedNames << i18nc("@item:intable Text context", "Annotation");
        translatedNames << i18nc("@item:intable Text context", "Comment Variable");
        // this next one is for denoting the beginning/end of a user defined folding region
        translatedNames << i18nc("@item:intable Text context", "Region Marker");
        translatedNames << i18nc("@item:intable Text context", "Information");
        translatedNames << i18nc("@item:intable Text context", "Warning");
        translatedNames << i18nc("@item:intable Text context", "Alert");

        translatedNames << i18nc("@item:intable Text context", "Others");
        // this one is for marking invalid input
        translatedNames << i18nc("@item:intable Text context", "Error");
    }

    // sanity checks
    Q_ASSERT(n >= 0);
    Q_ASSERT(n < names.size());

    return translateNames ? translatedNames[n] : names[n];
}

int KateHlManager::defaultStyleNameToIndex(const QString &name)
{
    //
    // Normal text and source code
    //
    if (name == QLatin1String("dsNormal")) {
        return KTextEditor::dsNormal;
    } else if (name == QLatin1String("dsKeyword")) {
        return KTextEditor::dsKeyword;
    } else if (name == QLatin1String("dsFunction")) {
        return KTextEditor::dsFunction;
    } else if (name == QLatin1String("dsVariable")) {
        return KTextEditor::dsVariable;
    } else if (name == QLatin1String("dsControlFlow")) {
        return KTextEditor::dsControlFlow;
    } else if (name == QLatin1String("dsOperator")) {
        return KTextEditor::dsOperator;
    } else if (name == QLatin1String("dsBuiltIn")) {
        return KTextEditor::dsBuiltIn;
    } else if (name == QLatin1String("dsExtension")) {
        return KTextEditor::dsExtension;
    } else if (name == QLatin1String("dsPreprocessor")) {
        return KTextEditor::dsPreprocessor;
    } else if (name == QLatin1String("dsAttribute")) {
        return KTextEditor::dsAttribute;
    }

    //
    // Strings & Characters
    //
    if (name == QLatin1String("dsChar")) {
        return KTextEditor::dsChar;
    } else if (name == QLatin1String("dsSpecialChar")) {
        return KTextEditor::dsSpecialChar;
    } else if (name == QLatin1String("dsString")) {
        return KTextEditor::dsString;
    } else if (name == QLatin1String("dsVerbatimString")) {
        return KTextEditor::dsVerbatimString;
    } else if (name == QLatin1String("dsSpecialString")) {
        return KTextEditor::dsSpecialString;
    } else if (name == QLatin1String("dsImport")) {
        return KTextEditor::dsImport;
    }

    //
    // Numbers, Types & Constants
    //
    if (name == QLatin1String("dsDataType")) {
        return KTextEditor::dsDataType;
    } else if (name == QLatin1String("dsDecVal")) {
        return KTextEditor::dsDecVal;
    } else if (name == QLatin1String("dsBaseN")) {
        return KTextEditor::dsBaseN;
    } else if (name == QLatin1String("dsFloat")) {
        return KTextEditor::dsFloat;
    } else if (name == QLatin1String("dsConstant")) {
        return KTextEditor::dsConstant;
    }

    //
    // Comments & Documentation
    //
    if (name == QLatin1String("dsComment")) {
        return KTextEditor::dsComment;
    } else if (name == QLatin1String("dsDocumentation")) {
        return KTextEditor::dsDocumentation;
    } else if (name == QLatin1String("dsAnnotation")) {
        return KTextEditor::dsAnnotation;
    } else if (name == QLatin1String("dsCommentVar")) {
        return KTextEditor::dsCommentVar;
    } else if (name == QLatin1String("dsRegionMarker")) {
        return KTextEditor::dsRegionMarker;
    } else if (name == QLatin1String("dsInformation")) {
        return KTextEditor::dsInformation;
    } else if (name == QLatin1String("dsWarning")) {
        return KTextEditor::dsWarning;
    } else if (name == QLatin1String("dsAlert")) {
        return KTextEditor::dsAlert;
    }

    //
    // Misc
    //
    if (name == QLatin1String("dsOthers")) {
        return KTextEditor::dsOthers;
    } else if (name == QLatin1String("dsError")) {
        return KTextEditor::dsError;
    }

    return KTextEditor::dsNormal;
}

void KateHlManager::getDefaults(const QString &schema, KateAttributeList &list, KConfig *cfg)
{
    const KColorScheme &scheme(KTextEditor::EditorPrivate::self()->defaultColors().view());
    const KColorScheme &schemeSelected(KTextEditor::EditorPrivate::self()->defaultColors().selection());

    ///NOTE: it's important to append in the order of the KTextEditor::DefaultStyle
    ///      enum, to make KTextEditor::DocumentPrivate::defaultStyle() work properly.

    {
        // dsNormal
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground().color());
        attrib->setSelectedForeground(schemeSelected.foreground().color());
        list.append(attrib);
    }
    {
        // dsKeyword
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground().color());
        attrib->setSelectedForeground(schemeSelected.foreground().color());
        attrib->setFontBold(true);
        list.append(attrib);
    }
    {
        // dsFunction
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::NeutralText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::NeutralText).color());
        list.append(attrib);
    }
    {
        // dsVariable
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::LinkText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::LinkText).color());
        list.append(attrib);
    }
    {
        // dsControlFlow
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground().color());
        attrib->setSelectedForeground(schemeSelected.foreground().color());
        attrib->setFontBold(true);
        list.append(attrib);
    }
    {
        // dsOperator
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground().color());
        attrib->setSelectedForeground(schemeSelected.foreground().color());
        list.append(attrib);
    }
    {
        // dsBuiltIn
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::VisitedText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::VisitedText).color());
        list.append(attrib);
    }
    {
        // dsExtension
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(QColor(0, 149, 255));
        attrib->setSelectedForeground(schemeSelected.foreground().color());
        attrib->setFontBold(true);
        list.append(attrib);
    }
    {
        // dsPreprocessor
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::PositiveText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::PositiveText).color());
        list.append(attrib);
    }
    {
        // dsAttribute
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::LinkText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::LinkText).color());
        list.append(attrib);
    }

    {
        // dsChar
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::ActiveText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::ActiveText).color());
        list.append(attrib);
    }
    {
        // dsSpecialChar
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::ActiveText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::ActiveText).color());
        list.append(attrib);
    }
    {
        // dsString
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::NegativeText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::NegativeText).color());
        list.append(attrib);
    }
    {
        // dsVerbatimString
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::NegativeText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::NegativeText).color());
        list.append(attrib);
    }
    {
        // dsSpecialString
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::NegativeText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::NegativeText).color());
        list.append(attrib);
    }
    {
        // dsImport
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::VisitedText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::VisitedText).color());
        list.append(attrib);
    }

    {
        // dsDataType
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::LinkText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::LinkText).color());
        list.append(attrib);
    }
    {
        // dsDecVal
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::NeutralText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::NeutralText).color());
        list.append(attrib);
    }
    {
        // dsBaseN
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::NeutralText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::NeutralText).color());
        list.append(attrib);
    }
    {
        // dsFloat
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::NeutralText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::NeutralText).color());
        list.append(attrib);
    }
    {
        // dsConstant
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground().color());
        attrib->setSelectedForeground(schemeSelected.foreground().color());
        attrib->setFontBold(true);
        list.append(attrib);
    }

    {
        // dsComment
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::InactiveText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::InactiveText).color());
        list.append(attrib);
    }
    {
        // dsDocumentation
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::NegativeText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::NegativeText).color());
        list.append(attrib);
    }
    {
        // dsAnnotation
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::VisitedText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::VisitedText).color());
        list.append(attrib);
    }
    {
        // dsCommentVar
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::VisitedText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::VisitedText).color());
        list.append(attrib);
    }
    {
        // dsRegionMarker
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::LinkText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::LinkText).color());
        attrib->setBackground(scheme.background(KColorScheme::LinkBackground).color());
        list.append(attrib);
    }
    {
        // dsInformation
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::NeutralText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::NeutralText).color());
        list.append(attrib);
    }
    {
        // dsWarning
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::NegativeText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::NegativeText).color());
        list.append(attrib);
    }
    {
        // dsAlert
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::NegativeText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::NegativeText).color());
        attrib->setFontBold(true);
        attrib->setBackground(scheme.background(KColorScheme::NegativeBackground).color());
        list.append(attrib);
    }

    {
        // dsOthers
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::PositiveText).color());
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::PositiveText).color());
        list.append(attrib);
    }
    {
        // dsError
        Attribute::Ptr attrib(new KTextEditor::Attribute());
        attrib->setForeground(scheme.foreground(KColorScheme::NegativeText));
        attrib->setSelectedForeground(schemeSelected.foreground(KColorScheme::NegativeText).color());
        attrib->setFontUnderline(true);
        list.append(attrib);
    }

    KConfigGroup config(cfg ? cfg : KateHlManager::self()->self()->getKConfig(),
                        QLatin1String("Default Item Styles - Schema ") + schema);

    for (int z = 0; z < defaultStyleCount(); z++) {
        KTextEditor::Attribute::Ptr i = list.at(z);
        QStringList s = config.readEntry(defaultStyleName(z), QStringList());
        if (!s.isEmpty()) {
            while (s.count() < 9) {
                s << QString();
            }

            QString tmp;
            QRgb col;

            tmp = s[0]; if (!tmp.isEmpty()) {
                col = tmp.toUInt(0, 16); i->setForeground(QColor(col));
            }

            tmp = s[1]; if (!tmp.isEmpty()) {
                col = tmp.toUInt(0, 16); i->setSelectedForeground(QColor(col));
            }

            tmp = s[2]; if (!tmp.isEmpty()) {
                i->setFontBold(tmp != QLatin1String("0"));
            }

            tmp = s[3]; if (!tmp.isEmpty()) {
                i->setFontItalic(tmp != QLatin1String("0"));
            }

            tmp = s[4]; if (!tmp.isEmpty()) {
                i->setFontStrikeOut(tmp != QLatin1String("0"));
            }

            tmp = s[5]; if (!tmp.isEmpty()) {
                i->setFontUnderline(tmp != QLatin1String("0"));
            }

            tmp = s[6]; if (!tmp.isEmpty()) {
                if (tmp != QLatin1String("-")) {
                    col = tmp.toUInt(0, 16);
                    i->setBackground(QColor(col));
                } else {
                    i->clearBackground();
                }
            }
            tmp = s[7]; if (!tmp.isEmpty()) {
                if (tmp != QLatin1String("-")) {
                    col = tmp.toUInt(0, 16);
                    i->setSelectedBackground(QColor(col));
                } else {
                    i->clearProperty(SelectedBackground);
                }
            }
            tmp = s[8]; if (!tmp.isEmpty() && tmp != QLatin1String("---")) {
                i->setFontFamily(tmp);
            }
        }
    }
}

void KateHlManager::setDefaults(const QString &schema, KateAttributeList &list, KConfig *cfg)
{
    cfg = cfg ? cfg : KateHlManager::self()->self()->getKConfig();
    KConfigGroup config(cfg,
                        QLatin1String("Default Item Styles - Schema ") + schema);

    const QString zero = QStringLiteral("0");
    const QString one = QStringLiteral("1");
    const QString dash = QStringLiteral("-");

    for (int z = 0; z < defaultStyleCount(); z++) {
        QStringList settings;
        KTextEditor::Attribute::Ptr p = list.at(z);

        settings << (p->hasProperty(QTextFormat::ForegroundBrush) ? QString::number(p->foreground().color().rgb(), 16) : QString());
        settings << (p->hasProperty(SelectedForeground) ? QString::number(p->selectedForeground().color().rgb(), 16) : QString());
        settings << (p->hasProperty(QTextFormat::FontWeight) ? (p->fontBold() ? one : zero) : QString());
        settings << (p->hasProperty(QTextFormat::FontItalic) ? (p->fontItalic() ? one : zero) : QString());
        settings << (p->hasProperty(QTextFormat::FontStrikeOut) ? (p->fontStrikeOut() ? one : zero) : QString());
        settings << (p->hasProperty(QTextFormat::FontUnderline) ? (p->fontUnderline() ? one : zero) : QString());
        settings << (p->hasProperty(QTextFormat::BackgroundBrush) ? QString::number(p->background().color().rgb(), 16) : dash);
        settings << (p->hasProperty(SelectedBackground) ? QString::number(p->selectedBackground().color().rgb(), 16) : dash);
        settings << (p->hasProperty(QTextFormat::FontFamily) ? (p->fontFamily()) : QString());
        settings << QStringLiteral("---");

        config.writeEntry(defaultStyleName(z), settings);
    }

    emit changed();
}

int KateHlManager::highlights()
{
    return (int) hlList.count();
}

QString KateHlManager::hlName(int n)
{
    return hlList.at(n)->name();
}

QString KateHlManager::hlNameTranslated(int n)
{
    return hlList.at(n)->nameTranslated();
}

QString KateHlManager::hlSection(int n)
{
    return hlList.at(n)->section();
}

bool KateHlManager::hlHidden(int n)
{
    return hlList.at(n)->hidden();
}

QString KateHlManager::identifierForName(const QString &name)
{
    if (hlDict.contains(name)) {
        return hlDict[name]->getIdentifier();
    }

    return QString();
}

QString KateHlManager::nameForIdentifier(const QString &identifier)
{
    for (QHash<QString, KateHighlighting *>::iterator it = hlDict.begin();
            it != hlDict.end(); ++it) {
        if ((*it)->getIdentifier() == identifier) {
            return it.key();
        }
    }

    return QString();
}

bool KateHlManager::resetDynamicCtxs()
{
    if (forceNoDCReset) {
        return false;
    }

    if (lastCtxsReset.elapsed() < KATE_DYNAMIC_CONTEXTS_RESET_DELAY) {
        return false;
    }

    foreach (KateHighlighting *hl, hlList) {
        hl->dropDynamicContexts();
    }

    dynamicCtxsCount = 0;
    lastCtxsReset.start();

    return true;
}

void KateHlManager::reload()
{
    // clear syntax document cache
    syntax.clearCache();

    resetDynamicCtxs();

    for(int i = 0; i < highlights(); i++)
    {
        getHl(i)->reload();
    }

    foreach(KTextEditor::DocumentPrivate* doc, KTextEditor::EditorPrivate::self()->kateDocuments())
    {
        doc->makeAttribs();
    }
}
//END

