/****************************************************************************
**
** This file is part of the LibreCAD project, a 2D CAD program
**
** Copyright (C) 2010 R. van Twisk (librecad@rvt.dds.nl)
** Copyright (C) 2001-2003 RibbonSoft. All rights reserved.
**
**
** This file may be distributed and/or modified under the terms of the
** GNU General Public License version 2 as published by the Free Software
** Foundation and appearing in the file gpl-2.0.txt included in the
** packaging of this file.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
**
** This copyright notice MUST APPEAR in all copies of the script!
**
**********************************************************************/



#include <iostream>
#include <QTextStream>
#include <QTextCodec>
#include <QFileInfo>
#include <QRawFont>
#include <QTransform>

#include "rs_font.h"
#include "rs_entitycontainer.h"
#include "rs_graphicview.h"
#include "rs_insert.h"
#include "rs_painter.h"
#include "rs_arc.h"
#include "rs_line.h"
#include "rs_polyline.h"
#include "rs_fontchar.h"
#include "rs_system.h"
#include "rs_math.h"
#include "rs_debug.h"

/**
 * Constructor.
 *
 * @param owner true if the font owns the letters (blocks). Otherwise
 *              the letters will be deleted when the font is deleted.
 */
RS_Font::RS_Font(const QString& fileName, bool owner)
    :	letterList(owner), fileName(fileName), fileLicense("unknown") {
    loaded = false;
    letterSpacing = 3.0;
    wordSpacing = 6.75;
    lineSpacingFactor = 1.0;
    rawLffFontList.clear();
}



/**
 * Loads the font into memory.
 *
 * @retval true font was already loaded or is loaded now.
 * @retval false font could not be loaded.
 */
bool RS_Font::loadFont() {
    RS_DEBUG->print("RS_Font::loadFont");

    if (loaded) {
        return true;
    }

    QString path;

    // Search for the appropriate font if we have only the name of the font:
    if (!fileName.contains(".cxf", Qt::CaseInsensitive) &&
        !fileName.contains(".lff", Qt::CaseInsensitive)) {
        QStringList fonts = RS_SYSTEM->getNewFontList();
        fonts.append(RS_SYSTEM->getFontList());

        for (const QString& font: fonts) {
            if (QFileInfo(font).baseName().toLower()==fileName.toLower()) {
                path = font;
                break;
            }
        }
    }

    // We have the full path of the font:
    else {
        path = fileName;
    }

    // No font paths found:
    if (path.isEmpty()) {
        RS_DEBUG->print(RS_Debug::D_WARNING,
                        "RS_Font::loadFont: No fonts available.");
        return false;
    }

    // Open cxf file:
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        LC_LOG(RS_Debug::D_WARNING) <<"RS_Font::loadFont: Cannot open font file: "<<path;
        return false;
    } else {
        LC_LOG<< "RS_Font::loadFont: Successfully opened font file: " << path;
    }
    f.close();

    if (path.contains(".cxf"))
        readCXF(path);
    if (path.contains(".lff")) {
        readLFF(path);
        loadOutline(path);
    }

    RS_Block* bk = letterList.find(QChar(0xfffd));
	if (!bk) {
        // create new letter:
		RS_FontChar* letter = new RS_FontChar(nullptr, QChar(0xfffd), RS_Vector(0.0, 0.0));
        RS_Polyline* pline = new RS_Polyline(letter, RS_PolylineData());
        pline->setPen(RS_Pen(RS2::FlagInvalid));
		pline->setLayer(nullptr);
        pline->addVertex(RS_Vector(1, 0), 0);
        pline->addVertex(RS_Vector(0, 2), 0);
        pline->addVertex(RS_Vector(1, 4), 0);
        pline->addVertex(RS_Vector(2, 2), 0);
        pline->addVertex(RS_Vector(1, 0), 0);
        letter->addEntity(pline);
        letter->calculateBorders();
        letterList.add(letter);
    }

    loaded = true;

    RS_DEBUG->print("RS_Font::loadFont OK");

    return true;
}


void RS_Font::readCXF(QString path) {
    QString line;
    QFile f(path);
    f.open(QIODevice::ReadOnly);
    QTextStream ts(&f);

    // Read line by line until we find a new letter:
    while (!ts.atEnd()) {
        line = ts.readLine();

        if (line.isEmpty())
            continue;

        // Read font settings:
        if (line.at(0)=='#') {
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
            QStringList lst = ( line.right(line.length()-1) ).split(':', Qt::SkipEmptyParts);
#else
            QStringList lst = ( line.right(line.length()-1) ).split(':', QString::SkipEmptyParts);
#endif
            QStringList::Iterator it3 = lst.begin();

            // RVT_PORT sometimes it happens that the size is < 2
            if (lst.size()<2)
                continue;

            QString identifier = (*it3).trimmed();
            it3++;
            QString value = (*it3).trimmed();

            if (identifier.toLower()=="letterspacing") {
                letterSpacing = value.toDouble();
            } else if (identifier.toLower()=="wordspacing") {
                wordSpacing = value.toDouble();
            } else if (identifier.toLower()=="linespacingfactor") {
                lineSpacingFactor = value.toDouble();
            } else if (identifier.toLower()=="author") {
                authors.append(value);
            } else if (identifier.toLower()=="name") {
                names.append(value);
            } else if (identifier.toLower()=="encoding") {
                ts.setCodec(QTextCodec::codecForName(value.toLatin1()));
                encoding = value;
            }
        }

        // Add another letter to this font:
        else if (line.at(0)=='[') {

            // uniode character:
            QChar ch;

            // read unicode:
            QRegExp regexp("[0-9A-Fa-f]{4,4}");
            regexp.indexIn(line);
            QString cap = regexp.cap();
            if (!cap.isNull()) {
				int uCode = cap.toInt(nullptr, 16);
                ch = QChar(uCode);
            }

            // read UTF8 (LibreCAD 1 compatibility)
            else if (line.indexOf(']')>=3) {
                int i = line.indexOf(']');
                QString mid = line.mid(1, i-1);
                ch = QString::fromUtf8(mid.toLatin1()).at(0);
            }

            // read normal ascii character:
            else {
                ch = line.at(1);
            }

            // create new letter:
            RS_FontChar* letter =
					new RS_FontChar(nullptr, ch, RS_Vector(0.0, 0.0));

            // Read entities of this letter:
            QString coordsStr;
            QStringList coords;
            QStringList::Iterator it2;
            do {
                line = ts.readLine();

                if (line.isEmpty()) {
                    continue;
                }

                coordsStr = line.right(line.length()-2);
                //                coords = QStringList::split(',', coordsStr);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
                coords = coordsStr.split(',', Qt::SkipEmptyParts);
#else
                coords = coordsStr.split(',', QString::SkipEmptyParts);
#endif
                it2 = coords.begin();

                // Line:
                if (line.at(0)=='L') {
                    double x1 = (*it2++).toDouble();
                    double y1 = (*it2++).toDouble();
                    double x2 = (*it2++).toDouble();
                    double y2 = (*it2).toDouble();

					RS_Line* line = new RS_Line{letter, {{x1, y1}, {x2, y2}}};
                    line->setPen(RS_Pen(RS2::FlagInvalid));
					line->setLayer(nullptr);
                    letter->addEntity(line);
                }

                // Arc:
                else if (line.at(0)=='A') {
                    double cx = (*it2++).toDouble();
                    double cy = (*it2++).toDouble();
                    double r = (*it2++).toDouble();
					double a1 = RS_Math::deg2rad((*it2++).toDouble());
					double a2 = RS_Math::deg2rad((*it2).toDouble());
                    bool reversed = (line.at(1)=='R');

                    RS_ArcData ad(RS_Vector(cx,cy),
                                  r, a1, a2, reversed);
                    RS_Arc* arc = new RS_Arc(letter, ad);
                    arc->setPen(RS_Pen(RS2::FlagInvalid));
					arc->setLayer(nullptr);
                    letter->addEntity(arc);
                }
            } while (!line.isEmpty());

            if (letter->isEmpty()) {
                delete letter;
            } else {
                letter->calculateBorders();
                letterList.add(letter);
            }
        }
    }
    f.close();
}

void RS_Font::readLFF(QString path) {
    QString line;
    QFile f(path);
    encoding = "UTF-8";
    f.open(QIODevice::ReadOnly);
    QTextStream ts(&f);

    // Read line by line until we find a new letter:
    while (!ts.atEnd()) {
        line = ts.readLine();

        if (line.isEmpty())
            continue;

        // Read font settings:
        if (line.at(0)=='#') {
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
            QStringList lst =line.remove(0,1).split(':', Qt::SkipEmptyParts);
#else
            QStringList lst =line.remove(0,1).split(':', QString::SkipEmptyParts);
#endif
            //if size is < 2 is a comentary not parameter
            if (lst.size()<2)
                continue;

            QString identifier = lst.at(0).trimmed();
            QString value = lst.at(1).trimmed();

            if (identifier.toLower()=="letterspacing") {
                letterSpacing = value.toDouble();
            } else if (identifier.toLower()=="wordspacing") {
                wordSpacing = value.toDouble();
            } else if (identifier.toLower()=="linespacingfactor") {
                lineSpacingFactor = value.toDouble();
            } else if (identifier.toLower()=="author") {
                authors.append(value);
            } else if (identifier.toLower()=="name") {
                names.append(value);
            } else if (identifier.toLower()=="license") {
                fileLicense = value;
            } else if (identifier.toLower()=="encoding") {
                ts.setCodec(QTextCodec::codecForName(value.toLatin1()));
                encoding = value;
            } else if (identifier.toLower()=="created") {
                fileCreate = value;
            }
        }

        // Add another letter to this font:
        else if (line.at(0)=='[') {

            // uniode character:
            QChar ch;

            // read unicode:
            QRegExp regexp("[0-9A-Fa-f]{1,5}");
            regexp.indexIn(line);
            QString cap = regexp.cap();
            if (!cap.isNull()) {
				int uCode = cap.toInt(nullptr, 16);
                ch = QChar(uCode);
            }
            // only unicode allowed
            else {
                LC_LOG(RS_Debug::D_WARNING)<<"Ignoring code from LFF font file: "<<line;
                continue;
            }

            QStringList fontData;
            do {
                line = ts.readLine();
                if(line.isEmpty()) break;
                fontData.push_back(line);
            } while(true);
            if (0 < fontData.size()                             // valid data
                && !rawLffFontList.contains( QString(ch))) {    // ignore duplicates
                rawLffFontList[QString(ch)] = fontData;
            }
        }
    }
    f.close();
}

void RS_Font::generateAllFonts(){
    QMap<QString, QStringList>::const_iterator i = rawLffFontList.constBegin();
    while (i != rawLffFontList.constEnd()) {
        generateLffFont(i.key());
        ++i;
    }
}

RS_Block* RS_Font::generateLffFont(const QString& key){
    if (key.isEmpty())
        return nullptr;

    if (!rawLffFontList.contains( key)) {
        LC_ERR<<QString{"RS_Font::generateLffFont([%1]): can not find the letter in LFF file "}.arg(QChar(key.front())) << fileName;
        return nullptr;
    }

    // create new letter:
    RS_FontChar* letter = new RS_FontChar(nullptr, key, RS_Vector(0.0, 0.0));

    // Read entities of this letter:
    QStringList vertex;
    QStringList coords;
    QStringList fontData = rawLffFontList[key];
    QString line;

    while(fontData.isEmpty() == false) {
        line = fontData.takeFirst();

        if (line.isEmpty()) {
            continue;
        }

        // Defined char:
        if (line.at(0)=='C') {
            line.remove(0,1);
			int uCode = line.toInt(nullptr, 16);
            QChar ch = QChar(uCode);
            if (QString(ch) == key) {   // recursion, a character can't include itself
                LC_ERR<<QString{"RS_Font::generateLffFont([%1]) : recursion, ignore this character from "}.arg(uCode, 4, 16)<<fileName;
                delete letter;
                return nullptr;
            }

            RS_Block* bk = letterList.find(ch);
            if (nullptr == bk) {
                if (!rawLffFontList.contains(ch)) {
                LC_ERR<<QString{"RS_Font::generateLffFont([%1]) : can not find the letter %2 in LFF file "}.arg(uCode, 4, 16).arg(QChar(key.front()))<<fileName;
                    delete letter;
                    return nullptr;
                }
                generateLffFont(ch);
                bk = letterList.find(ch);
            }
            if (nullptr != bk) {
                RS_Entity* bk2 = bk->clone();
                bk2->setPen(RS_Pen(RS2::FlagInvalid));
                bk2->setLayer(nullptr);
                letter->addEntity(bk2);
            }
        }
        //sequence:
        else {
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
            vertex = line.split(';', Qt::SkipEmptyParts);
#else
            vertex = line.split(';', QString::SkipEmptyParts);
#endif
            //at least is required two vertex
            if (vertex.size()<2)
                continue;
            RS_Polyline* pline = new RS_Polyline(letter, RS_PolylineData());
            pline->setPen(RS_Pen(RS2::FlagInvalid));
			pline->setLayer(nullptr);
            foreach(const QString& point, vertex) {
                double x1, y1;
                double bulge = 0;

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
                coords = point.split(',', Qt::SkipEmptyParts);
#else
                coords = point.split(',', QString::SkipEmptyParts);
#endif
                //at least X,Y is required
                if (coords.size()<2)
                    continue;
                x1 = coords.at(0).toDouble();
                y1 = coords.at(1).toDouble();
                //check presence of bulge
                if (coords.size() == 3 && coords.at(2).at(0) == QChar('A')){
                    QString bulgeStr = coords.at(2);
                    bulge = bulgeStr.remove(0,1).toDouble();
                }
                pline->setNextBulge(bulge);
                pline->addVertex(RS_Vector(x1, y1), bulge);
            }
            letter->addEntity(pline);
        }

    }

    if (letter->isEmpty()) {
        delete letter;
        return nullptr;
    } else {
        letter->calculateBorders();
        letterList.add(letter);
        return letter;
    }
}

namespace {
// Thai character classes (U+0E00 block)
bool thaiConsonant(ushort c) { return c >= 0x0E01 && c <= 0x0E2E; }
bool thaiAscender(ushort c) { return c == 0x0E1B || c == 0x0E1D || c == 0x0E1F || c == 0x0E2C; } // ป ฝ ฟ ฬ
bool thaiUpperVowel(ushort c) { return c == 0x0E31 || (c >= 0x0E34 && c <= 0x0E37) || c == 0x0E47 || c == 0x0E4D; }
bool thaiTone(ushort c) { return c >= 0x0E48 && c <= 0x0E4C; }   // ่ ้ ๊ ๋ ์
bool thaiLowerVowel(ushort c) { return c >= 0x0E38 && c <= 0x0E3A; } // ุ ู ฺ

ushort thaiUpperVowelLeft(ushort c) {
    switch (c) {
    case 0x0E31: return 0xF710;
    case 0x0E47: return 0xF712;
    case 0x0E4D: return 0xF711;
    default: return 0xF701 + (c - 0x0E34); // ิ ี ึ ื
    }
}
}

/**
 * Contextual glyph selection for Thai, as the legacy Windows Thai PUA fonts do:
 * tone marks sit low unless an upper vowel (or SARA AM) shares the cell, marks shift
 * left over the tall consonants ป ฝ ฟ ฬ, lower vowels drop under ฎ ฏ, and ญ ฐ lose
 * their tails before a lower vowel. One QChar in, one QChar out, so indices and
 * escape sequences (MText \P, \S ...) are unaffected.
 */
QString RS_Font::shapeThai(const QString& text) {
    QString out = text;
    int base = -1;          // index of the current Thai consonant, -1 = none
    bool upper = false;     // an upper vowel/tone already sits on the consonant
    for (int i = 0; i < text.length(); ++i) {
        const ushort c = text.at(i).unicode();
        if (thaiConsonant(c)) {
            base = i;
            upper = false;
            continue;
        }
        if (c < 0x0E30 || c > 0x0E4E) {   // not a Thai vowel/mark: break the cluster
            base = -1;
            continue;
        }
        if (base < 0)
            continue;
        const ushort b = text.at(base).unicode();
        if (thaiUpperVowel(c)) {
            if (thaiAscender(b))
                out[i] = QChar(thaiUpperVowelLeft(c));
            upper = true;
        } else if (thaiTone(c)) {
            const bool high = upper || (i + 1 < text.length() && text.at(i + 1).unicode() == 0x0E33);
            if (thaiAscender(b))
                out[i] = QChar((high ? 0xF713 : 0xF705) + (c - 0x0E48));
            else if (!high)
                out[i] = QChar(0xF70A + (c - 0x0E48));
            upper = true;
        } else if (thaiLowerVowel(c)) {
            if (b == 0x0E0E || b == 0x0E0F)          // ฎ ฏ: drop the vowel below the tail
                out[i] = QChar(0xF718 + (c - 0x0E38));
            else if (b == 0x0E0D)                     // ญ without tail
                out[base] = QChar(0xF70F);
            else if (b == 0x0E10)                     // ฐ without tail
                out[base] = QChar(0xF700);
        } else if (c != 0x0E33) {
            // spacing vowel (ะ า เ ...) ends the stack on this consonant
            base = -1;
        }
    }
    return out;
}

bool RS_Font::isThaiNonSpacing(QChar ch) {
    const ushort c = ch.unicode();
    return c == 0x0E31 || (c >= 0x0E34 && c <= 0x0E3A) || (c >= 0x0E47 && c <= 0x0E4E)
           || (c >= 0xF701 && c <= 0xF70E) || (c >= 0xF710 && c <= 0xF71A);
}

RS_Block* RS_Font::findLetter(const QString& name) {
    RS_Block* ret= letterList.find(name);
	if (ret) return ret;
    return generateLffFont(name);

}

void RS_Font::loadOutline(const QString& lffPath) {
    QFileInfo fi(lffPath);
    QString ttf = fi.absolutePath() + "/" + fi.completeBaseName() + ".ttf";
    if (!QFileInfo::exists(ttf))
        return;
    auto raw = QSharedPointer<QRawFont>::create(ttf, 1000.0, QFont::PreferNoHinting);
    if (!raw->isValid())
        return;
    // same scale as ttf2lff.py: top of "H" = 9 LFF units
    QVector<quint32> h = raw->glyphIndexesForString("H");
    double capTop = h.size() == 1 ? -raw->pathForGlyph(h[0]).boundingRect().top() : 0.;
    if (capTop <= 0.)
        return;
    outlineFont = raw;
    outlineScale = 9.0 / capTop;
    LC_LOG << "RS_Font::loadOutline: filled letters from " << ttf;
}

QPainterPath RS_Font::outlineGlyph(const QString& name) {
    auto it = outlineCache.constFind(name);
    if (it != outlineCache.constEnd())
        return it.value();
    QPainterPath p;
    QVector<quint32> idx = outlineFont->glyphIndexesForString(name);
    if (idx.size() == 1 && idx[0] != 0) {
        // QRawFont paths are y-down
        p = QTransform(outlineScale, 0., 0., -outlineScale, 0., 0.)
                .map(outlineFont->pathForGlyph(idx[0]));
    }
    outlineCache.insert(name, p);
    return p;
}

double RS_Font::outlineAdvance(const QString& name) {
    if (!outlineFont)
        return -1.;
    QVector<quint32> idx = outlineFont->glyphIndexesForString(name);
    if (idx.size() != 1 || idx[0] == 0)
        return -1.;
    QVector<QPointF> adv = outlineFont->advancesForGlyphIndexes(idx);
    return adv.size() == 1 ? adv[0].x() * outlineScale : -1.;
}

bool RS_Font::drawOutline(RS_EntityContainer* text, RS_Painter* painter, RS_GraphicView* view) {
    if (!outlineFont)
        return false;
    const RS_Pen pen = painter->getPen();
    const QBrush brush = painter->brush();
    painter->setBrush(pen.getColor());
    painter->disablePen();
    drawOutlineLetters(text, painter, view);
    painter->setBrush(brush);
    painter->setPen(pen);
    return true;
}

void RS_Font::drawOutlineLetters(RS_EntityContainer* container, RS_Painter* painter,
                                 RS_GraphicView* view) {
    for (RS_Entity* e: *container) {
        if (e->rtti() == RS2::EntityInsert) {
            auto letter = static_cast<RS_Insert*>(e);
            QPainterPath glyph = outlineGlyph(letter->getName());
            if (glyph.isEmpty()) {
                // no TrueType glyph: stroke the LFF letter (e.g. U+FFFD box)
                painter->setPen(RS_Color(painter->brush().color()));
                double offset = 0.;
                letter->draw(painter, view, offset);
                painter->disablePen();
                continue;
            }
            // letter block -> world: scale, rotate, move; world -> screen is affine
            const RS_Vector ip = letter->getInsertionPoint();
            const RS_Vector scale = letter->getScale();
            const double angle = letter->getAngle();
            const RS_Vector o = view->toGui(ip);
            const RS_Vector ux = view->toGui(ip + RS_Vector::polar(scale.x, angle)) - o;
            const RS_Vector uy = view->toGui(ip + RS_Vector::polar(scale.y, angle + M_PI_2)) - o;
            painter->drawPath(QTransform(ux.x, ux.y, uy.x, uy.y, o.x, o.y).map(glyph));
        } else if (e->isContainer()) {
            drawOutlineLetters(static_cast<RS_EntityContainer*>(e), painter, view);
        }
    }
}
/**
 * Dumps the fonts data to stdout.
 */
std::ostream& operator << (std::ostream& os, const RS_Font& f) {
    os << " Font file name: " << f.getFileName().toLatin1().data() << "\n";
    //<< (RS_BlockList&)f << "\n";
    return os;
}

