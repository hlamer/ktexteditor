/*  This file is part of the KDE libraries and the Kate part.
 *
 *  Copyright (C) 2003-2005 Anders Lund <anders@alweb.dk>
 *  Copyright (C) 2001-2010 Christoph Cullmann <cullmann@kde.org>
 *  Copyright (C) 2001 Charles Samuels <charles@kde.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 */

#include <vimode/cmds.h>

#include "katedocument.h"
#include "kateview.h"
#include "kateglobal.h"
#include <vimode/modes/normalvimode.h>
#include <vimode/emulatedcommandbar.h>
#include "katecmd.h"
#include "katepartdebug.h"
#include "kateviinputmode.h"
#include <vimode/inputmodemanager.h>
#include "globalstate.h"
#include "marks.h"

#include <KLocalizedString>

#include <QDir>
#include <QRegExp>
#include <QUrl>

using namespace KateVi;

//BEGIN ViCommands
Commands *Commands::m_instance = 0;

bool Commands::exec(KTextEditor::View *view, const QString &_cmd,
                    QString &msg, const KTextEditor::Range &range)
{
    Q_UNUSED(range)
    // cast it hardcore, we know that it is really a kateview :)
    KTextEditor::ViewPrivate *v = static_cast<KTextEditor::ViewPrivate *>(view);

    if (!v) {
        msg = i18n("Could not access view");
        return false;
    }

    //create a list of args
    QStringList args(_cmd.split(QRegExp(QLatin1String("\\s+")), QString::SkipEmptyParts));
    QString cmd(args.takeFirst());

    // ALL commands that takes no arguments.
    if (mappingCommands().contains(cmd)) {
        if (cmd.endsWith(QLatin1String("unmap"))) {
            if (args.count() == 1) {
                m_viGlobal->mappings()->remove(modeForMapCommand(cmd), args.at(0));
                return true;
            } else {
                msg = i18n("Missing argument. Usage: %1 <from>",  cmd);
                return false;
            }
        }
        if (args.count() == 1) {
            msg = m_viGlobal->mappings()->get(modeForMapCommand(cmd), args.at(0), true);
            if (msg.isEmpty()) {
                msg = i18n("No mapping found for \"%1\"", args.at(0));
                return false;
            } else {
                msg = i18n("\"%1\" is mapped to \"%2\"", args.at(0), msg);
            }
        } else if (args.count() == 2) {
            Mappings::MappingRecursion mappingRecursion = (isMapCommandRecursive(cmd)) ? Mappings::Recursive : Mappings::NonRecursive;
            m_viGlobal->mappings()->add(modeForMapCommand(cmd), args.at(0), args.at(1), mappingRecursion);
        } else {
            msg = i18n("Missing argument(s). Usage: %1 <from> [<to>]",  cmd);
            return false;
        }

        return true;
    }

    NormalViMode *nm = m_viInputModeManager->getViNormalMode();

    if (cmd == QLatin1String("d") || cmd == QLatin1String("delete") || cmd == QLatin1String("j") ||
            cmd == QLatin1String("c") || cmd == QLatin1String("change") ||  cmd == QLatin1String("<") || cmd == QLatin1String(">") ||
            cmd == QLatin1String("y") || cmd == QLatin1String("yank")) {

        KTextEditor::Cursor start_cursor_position = v->cursorPosition();

        int count = 1;
        if (range.isValid()) {
            count = qAbs(range.end().line() - range.start().line()) + 1;
            v->setCursorPosition(KTextEditor::Cursor(qMin(range.start().line(),
                                 range.end().line()), 0));
        }

        QRegExp number(QLatin1String("^(\\d+)$"));
        for (int i = 0; i < args.count(); i++) {
            if (number.indexIn(args.at(i)) != -1) {
                count += number.cap().toInt() - 1;
            }

            QChar r = args.at(i).at(0);
            if (args.at(i).size() == 1 && ((r >= QLatin1Char('a') && r <= QLatin1Char('z')) || r == QLatin1Char('_') || r == QLatin1Char('+') || r == QLatin1Char('*'))) {
                nm->setRegister(r);
            }
        }

        nm->setCount(count);

        if (cmd == QLatin1String("d") || cmd == QLatin1String("delete")) {
            nm->commandDeleteLine();
        }
        if (cmd == QLatin1String("j")) {
            nm->commandJoinLines();
        }
        if (cmd == QLatin1String("c") || cmd == QLatin1String("change")) {
            nm->commandChangeLine();
        }
        if (cmd == QLatin1String("<")) {
            nm->commandUnindentLine();
        }
        if (cmd == QLatin1String(">")) {
            nm->commandIndentLine();
        }
        if (cmd == QLatin1String("y") || cmd == QLatin1String("yank")) {
            nm->commandYankLine();
            v->setCursorPosition(start_cursor_position);
        }

        // TODO - should we resetParser, here? We'd have to make it public, if so.
        // Or maybe synthesise a KateViCommand to execute instead ... ?
        nm->setCount(0);

        return true;
    }

    if (cmd == QLatin1String("mark") || cmd == QLatin1String("ma") || cmd == QLatin1String("k")) {
        if (args.count() == 0) {
            if (cmd == QLatin1String("mark")) {
                // TODO: show up mark list;
            } else {
                msg = i18n("Wrong arguments");
                return false;
            }
        } else if (args.count() == 1) {

            QChar r = args.at(0).at(0);
            int line;
            if ((r >= QLatin1Char('a') && r <= QLatin1Char('z')) || r == QLatin1Char('_') || r == QLatin1Char('+') || r == QLatin1Char('*')) {
                if (range.isValid()) {
                    line = qMax(range.end().line(), range.start().line());
                } else {
                    line = v->cursorPosition().line();
                }

                m_viInputModeManager->marks()->setUserMark(r, KTextEditor::Cursor(line, 0));
            }
        } else {
            msg = i18n("Wrong arguments");
            return false;
        }
        return true;
    }

    // should not happen :)
    msg = i18n("Unknown command '%1'", cmd);
    return false;
}

bool Commands::supportsRange(const QString &range)
{
    static QStringList l;

    if (l.isEmpty())
        l << QLatin1String("d") << QLatin1String("delete") << QLatin1String("j") << QLatin1String("c") << QLatin1String("change") << QLatin1String("<") <<
          QLatin1String(">") << QLatin1String("y") << QLatin1String("yank") << QLatin1String("ma") << QLatin1String("mark") << QLatin1String("k");

    return l.contains(range.split(QLatin1String(" ")).at(0));
}

KCompletion *Commands::completionObject(KTextEditor::View *view, const QString &cmd)
{
    Q_UNUSED(view)

    KTextEditor::ViewPrivate *v = static_cast<KTextEditor::ViewPrivate *>(view);

    if (v && (cmd == QLatin1String("nn") || cmd == QLatin1String("nnoremap"))) {
        QStringList l = m_viGlobal->mappings()->getAll(Mappings::NormalModeMapping);

        KateCmdShellCompletion *co = new KateCmdShellCompletion();
        co->setItems(l);
        co->setIgnoreCase(false);
        return co;
    }
    return 0L;
}

const QStringList &Commands::mappingCommands()
{
    static QStringList mappingsCommands;
    if (mappingsCommands.isEmpty()) {
        mappingsCommands << QLatin1String("nmap")  << QLatin1String("nm")  << QLatin1String("noremap") << QLatin1String("nnoremap") << QLatin1String("nn") << QLatin1String("no")
                         << QLatin1String("vmap") << QLatin1String("vm") << QLatin1String("vnoremap") << QLatin1String("vn")
                         << QLatin1String("imap") << QLatin1String("im") << QLatin1String("inoremap") << QLatin1String("ino")
                         << QLatin1String("cmap") << QLatin1String("cm") << QLatin1String("cnoremap") << QLatin1String("cno");

        mappingsCommands << QLatin1String("nunmap") << QLatin1String("vunmap") << QLatin1String("iunmap") << QLatin1String("cunmap");
    }
    return mappingsCommands;
}

Mappings::MappingMode Commands::modeForMapCommand(const QString &mapCommand)
{
    static QMap<QString, Mappings::MappingMode> modeForMapCommand;
    if (modeForMapCommand.isEmpty()) {
        // Normal is the default.
        modeForMapCommand.insert(QLatin1String("vmap"), Mappings::VisualModeMapping);
        modeForMapCommand.insert(QLatin1String("vm"), Mappings::VisualModeMapping);
        modeForMapCommand.insert(QLatin1String("vnoremap"), Mappings::VisualModeMapping);
        modeForMapCommand.insert(QLatin1String("vn"), Mappings::VisualModeMapping);
        modeForMapCommand.insert(QLatin1String("imap"), Mappings::InsertModeMapping);
        modeForMapCommand.insert(QLatin1String("im"), Mappings::InsertModeMapping);
        modeForMapCommand.insert(QLatin1String("inoremap"), Mappings::InsertModeMapping);
        modeForMapCommand.insert(QLatin1String("ino"), Mappings::InsertModeMapping);
        modeForMapCommand.insert(QLatin1String("cmap"), Mappings::CommandModeMapping);
        modeForMapCommand.insert(QLatin1String("cm"), Mappings::CommandModeMapping);
        modeForMapCommand.insert(QLatin1String("cnoremap"), Mappings::CommandModeMapping);
        modeForMapCommand.insert(QLatin1String("cno"), Mappings::CommandModeMapping);

        modeForMapCommand.insert(QLatin1String("nunmap"), Mappings::NormalModeMapping);
        modeForMapCommand.insert(QLatin1String("vunmap"), Mappings::VisualModeMapping);
        modeForMapCommand.insert(QLatin1String("iunmap"), Mappings::InsertModeMapping);
        modeForMapCommand.insert(QLatin1String("cunmap"), Mappings::CommandModeMapping);
    }
    return modeForMapCommand[mapCommand];
}

bool Commands::isMapCommandRecursive(const QString &mapCommand)
{
    static QMap<QString, bool> isMapCommandRecursive;
    {
        isMapCommandRecursive.insert(QLatin1String("nmap"), true);
        isMapCommandRecursive.insert(QLatin1String("nm"), true);
        isMapCommandRecursive.insert(QLatin1String("vmap"), true);
        isMapCommandRecursive.insert(QLatin1String("vm"), true);
        isMapCommandRecursive.insert(QLatin1String("imap"), true);
        isMapCommandRecursive.insert(QLatin1String("im"), true);
        isMapCommandRecursive.insert(QLatin1String("cmap"), true);
        isMapCommandRecursive.insert(QLatin1String("cm"), true);
    }
    return isMapCommandRecursive[mapCommand];
}

//END ViCommands

//BEGIN SedReplace
SedReplace *SedReplace::m_instance = 0;

bool SedReplace::interactiveSedReplace(KTextEditor::ViewPrivate *, QSharedPointer<InteractiveSedReplacer> interactiveSedReplace)
{
    EmulatedCommandBar *emulatedCommandBar = m_viInputModeManager->inputAdapter()->viModeEmulatedCommandBar();
    emulatedCommandBar->startInteractiveSearchAndReplace(interactiveSedReplace);
    return true;
}
//END SedReplace
