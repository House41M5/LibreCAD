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


#ifndef RS_PEN_H
#define RS_PEN_H

#include <QString>
#include <QVector>

#include "rs.h"
#include "rs_color.h"
#include "rs_flags.h"


/**
 * A pen stores attributes for painting such as line width,
 * linetype, color, ...
 *
 * @author Andrew Mustun
 */
class RS_Pen : public RS_Flags {
public:
    /**
     * Creates a default pen (black, solid, width 0).
     */
    RS_Pen() = default;

    /**
     * Creates a pen with the given attributes.
     */
    RS_Pen(const RS_Color& c,
           RS2::LineWidth w,
           RS2::LineType t) : RS_Flags() {
        setColor(c);
        setWidth(w);
        setLineType(t);
		setScreenWidth(0);
        setAlpha(1.0);
    }
    /**
     * Creates a default pen with the given flags. This is 
     * usually used to create invalid pens.
     *
     * e.g.:
     * <pre>
     *   RS_Pen p(RS2::FlagInvalid);
     * </pre>
     */
    RS_Pen(unsigned int f) : RS_Flags(f) {
        setColor(RS_Color(0,0,0));
        setWidth(RS2::Width00);
        setLineType(RS2::SolidLine);
		setScreenWidth(0);
        setAlpha(1.0);
    }
    //RS_Pen(const RS_Pen& pen) : RS_Flags(pen.getFlags()) {
    //    lineType = pen.lineType;
    //    width = pen.width;
    //    color = pen.color;
    //}

    RS2::LineType getLineType() const {
        return lineType;
    }
    void setLineType(RS2::LineType t) {
        lineType = t;
        lineTypeName.clear();
        screenPattern.clear();
    }
    /**
     * DXF linetype name as read from a file (e.g. "HIDDEN", "WATERLINE"). While set (and the
     * graphic has that LTYPE), the entity is drawn with the file's pattern in drawing units
     * x $LTSCALE x getLineTypeScale(), like AutoCAD, instead of the built-in screen-size
     * pattern of getLineType(). setLineType() clears it.
     */
    const QString& getLineTypeName() const {
        return lineTypeName;
    }
    void setLineTypeName(const QString& name) {
        lineTypeName = name;
    }
    /** entity linetype scale, DXF group 48 */
    double getLineTypeScale() const {
        return lineTypeScale;
    }
    void setLineTypeScale(double s) {
        lineTypeScale = s;
    }
    /** dash/gap lengths in device pixels for this draw (set by RS_GraphicView::setPenForEntity) */
    const QVector<double>& getScreenPattern() const {
        return screenPattern;
    }
    void setScreenPattern(const QVector<double>& p) {
        screenPattern = p;
    }
    RS2::LineWidth getWidth() const {
        return width;
    }
    void setWidth(RS2::LineWidth w) {
        width = w;
    }
    double getScreenWidth() const {
        return screenWidth;
    }
    void setScreenWidth(double w) {
        screenWidth = w;
    }
    RS_Color getColor() const {
        return color;
    }
    void setColor(const RS_Color& c) {
        color = c;
    }
    bool isValid() {
        return !getFlag(RS2::FlagInvalid);
    }

    double getAlpha() const {
        return alpha;
    }
    void setAlpha(double a) {
        alpha = a;
    }

    //RS_Pen& operator = (const RS_Pen& p) {
    //    lineType = p.lineType;
    //    width = p.width;
    //    color = p.color;
    //    setFlags(p.getFlags());

    //    return *this;
    //}

    bool operator == (const RS_Pen& p) const {
        return (lineType==p.lineType && width==p.width && color==p.color);
    }

    bool operator != (const RS_Pen& p) const {
        return !(*this==p);
    }

    // accessor/mutator for dash pattern offset
    void setDashOffset(double offset)
    {
        m_dashOffset = offset;
    }

    double dashOffset() const
    {
        return m_dashOffset;
    }

    friend std::ostream& operator << (std::ostream& os, const RS_Pen& p);


protected:
    RS2::LineType lineType = RS2::SolidLine;
    RS2::LineWidth width = RS2::Width00;
    double screenWidth = 0.;
    RS_Color color{};
    double alpha = 1.;
    double m_dashOffset = 0.;
    QString lineTypeName;
    double lineTypeScale = 1.;
    QVector<double> screenPattern;
};

#endif
