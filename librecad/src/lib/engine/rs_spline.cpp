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

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>

#include "lc_splinegeom.h"

#include "rs_spline.h"

#include "rs_line.h"
#include "rs_debug.h"
#include "rs_graphicview.h"
#include "rs_painter.h"

namespace {
using GVec = lc::splinefit::Vec;

GVec toG(const RS_Vector& v) { return {v.x, v.y}; }
RS_Vector fromG(const GVec& v) { return {v.x, v.y}; }

RS_SplineData toData(const lc::splinegeom::Spline& g) {
    RS_SplineData out(g.p, false);
    out.controlPoints.reserve(g.P.size());
    for (const GVec& q : g.P) {
        out.controlPoints.push_back(fromG(q));
    }
    out.knotslist = g.U;
    return out;
}
}


RS_SplineData::RS_SplineData(int _degree, bool _closed):
	degree(_degree)
  ,closed(_closed)
{
}

std::ostream& operator << (std::ostream& os, const RS_SplineData& ld) {
	os << "( degree: " << ld.degree <<
	      " closed: " << ld.closed;
	if (ld.controlPoints.size()) {
		os << "\n(control points:\n";
		for (auto const& v: ld.controlPoints)
			os<<v;
		os<<")\n";
	}
	if (ld.knotslist.size()) {
		os << "\n(knot vector:\n";
		for (auto const& v: ld.knotslist)
			os<<v;
		os<<")\n";
	}
	os  << ")";
	return os;
}

/**
 * Constructor.
 */
RS_Spline::RS_Spline(RS_EntityContainer* parent,
                     const RS_SplineData& d)
        :RS_EntityContainer(parent), data(d) {
    calculateBorders();
}

RS_Entity* RS_Spline::clone() const{
    RS_Spline* l = new RS_Spline(*this);
    l->setOwner(isOwner());
    l->initId();
    l->detach();
    return l;
}



void RS_Spline::calculateBorders() {
    /*minV = RS_Vector::minimum(data.startpoint, data.endpoint);
    maxV = RS_Vector::maximum(data.startpoint, data.endpoint);

    QList<RS_Vector>::iterator it;
    for (it = data.controlPoints.begin();
    it!=data.controlPoints.end(); ++it) {

    minV = RS_Vector::minimum(*it, minV);
    maxV = RS_Vector::maximum(*it, maxV);
}
    */
}


void RS_Spline::setDegree(int degree) {
    if (degree>=1 && degree<=3) {
        data.degree = degree;
    } else {
        RS_DEBUG->print(RS_Debug::D_CRITICAL, "%s(%d): invalid degree = %d", __func__, degree, degree);
    }
}

/** @return Degree of this spline curve (1-3).*/
int RS_Spline::getDegree() const{
	return data.degree;
}

size_t RS_Spline::getNumberOfControlPoints() const {
	return data.controlPoints.size();
}

/**
 * @retval true if the spline is closed.
 * @retval false otherwise.
 */
bool RS_Spline::isClosed() const {
		return data.closed;
}

/**
 * Sets the closed flag of this spline.
 */
void RS_Spline::setClosed(bool c) {
    if (data.closed == c)
        return;

    // when switching from closed to open, remove wrapped control points
    if (data.closed && hasWrappedControlPoints()) {
        data.controlPoints.erase(data.controlPoints.begin() + (data.controlPoints.size() - data.degree), data.controlPoints.end());
    }
    data.closed = c;

    update();
}

RS_VectorSolutions RS_Spline::getRefPoints() const
{
    return {data.controlPoints};
}

RS_Vector RS_Spline::getNearestRef( const RS_Vector& coord,
                                    double* dist /*= nullptr*/) const
{
    // override the RS_EntityContainer method
    // use RS_Entity instead for spline point dragging
    return RS_Entity::getNearestRef(coord, dist);
}

RS_Vector RS_Spline::getNearestSelectedRef( const RS_Vector& coord,
                                            double* dist /*= nullptr*/) const
{
    // override the RS_EntityContainer method
    // use RS_Entity instead for spline point dragging
    return RS_Entity::getNearestSelectedRef(coord, dist);
}

/**
 * Updates the internal polygon of this spline. Called when the
 * spline or it's data, position, .. changes.
 */
void RS_Spline::update() {

    RS_DEBUG->print("RS_Spline::update");

    clear();
    m_tessParams.clear();

    if (isUndone()) {
        return;
    }

    if (data.degree<1 || data.degree>3) {
        RS_DEBUG->print("RS_Spline::update: invalid degree: %d", data.degree);
        return;
    }

    // Issue #1689: allow closed splines by 3 control points
    if ( (!data.closed && data.controlPoints.size() < size_t(data.degree)+1) || data.controlPoints.size() < 3) {
        RS_DEBUG->print("RS_Spline::update: not enough control points");
        return;
    }

    resetBorders();

    // wrap control points, if it's not wrapped yet
    std::vector<RS_Vector>& tControlPoints = data.controlPoints;
    if (data.closed && !hasWrappedControlPoints()) {
        std::vector<RS_Vector> wrappedPoints{data.controlPoints.cbegin(), data.controlPoints.cbegin() + data.degree};
        tControlPoints.insert(tControlPoints.end(), wrappedPoints.cbegin(), wrappedPoints.cend());
        RS_DEBUG->print(RS_Debug::D_NOTICE, "%s: controlPoints: size=%zu\n", __func__, data.controlPoints.size());
    }

    // 2026-11-03 step 25: lines within a chord error of the exact curve (was $SPLINESEGS lines per control
    // point, uniform in the parameter: 0.016 off a 5-point vp spline fitted to 0.01)
    lc::splinegeom::Spline g;
    if (!curveData(g)) {
        RS_DEBUG->print("RS_Spline::update: no curve for these data");
        return;
    }
    std::vector<GVec> gp;
    lc::splinegeom::tessellate(g, lc::splinegeom::defaultChordError(g), gp, m_tessParams);
    std::vector<RS_Vector> p;
    p.reserve(gp.size());
    for (const GVec& q : gp) {
        p.push_back(fromG(q));
    }

	RS_Vector prev{};
	for (auto const& vp: p) {
		if (prev.valid) {
			RS_Line* line = new RS_Line{this, prev, vp};
			line->setLayer(nullptr);
			line->setPen(RS2::FlagInvalid);
			addEntity(line);
		}
		prev = vp;
		minV = RS_Vector::minimum(prev, minV);
		maxV = RS_Vector::maximum(prev, maxV);
	}
}

// 2026-11-02 (spline explode actions): the same computation as update() above, on a copy of the
// control points so it can be const, with the caller's resolution instead of $SPLINESEGS.
std::vector<RS_Vector> RS_Spline::getStrokePoints(int segmentsPerPoint) const {
    if (data.degree < 1 || data.degree > 3) {
        return {};
    }
    if ((!data.closed && data.controlPoints.size() < size_t(data.degree) + 1) || data.controlPoints.size() < 3) {
        return {};
    }
    std::vector<RS_Vector> cp = data.controlPoints;
    if (data.closed && !hasWrappedControlPoints()) {
        std::vector<RS_Vector> wrappedPoints{cp.cbegin(), cp.cbegin() + data.degree};
        cp.insert(cp.end(), wrappedPoints.cbegin(), wrappedPoints.cend());
    }
    const size_t npts = cp.size();
    const size_t k = data.degree + 1;
    const size_t p1 = size_t(std::max(1, segmentsPerPoint)) * npts;
    std::vector<double> h(npts + 1, 1.);
    std::vector<RS_Vector> p(p1, {0., 0.});
    if (data.closed) {
        rbsplinu(npts, k, p1, cp, h, p);
    } else {
        rbspline(npts, k, p1, cp, h, p);
    }
    return p;
}

RS_Vector RS_Spline::getStartpoint() const {
   if (data.closed) return RS_Vector(false);
   return static_cast<RS_Line*>(const_cast<RS_Spline*>(this)->firstEntity())->getStartpoint();
}

RS_Vector RS_Spline::getEndpoint() const {
   if (data.closed) return RS_Vector(false);
   return static_cast<RS_Line*>(const_cast<RS_Spline*>(this)->lastEntity())->getEndpoint();
}


RS_Vector RS_Spline::getNearestEndpoint(const RS_Vector& coord,
                                        double* dist)const {
    double minDist = RS_MAXDOUBLE;
    RS_Vector ret(false);
    if(! data.closed) { // no endpoint for closed spline
       RS_Vector vp1(getStartpoint());
       RS_Vector vp2(getEndpoint());
       double d1( (coord-vp1).squared());
       double d2( (coord-vp2).squared());
       if( d1<d2){
           ret=vp1;
           minDist=std::sqrt(d1);
       }else{
           ret=vp2;
           minDist=std::sqrt(d2);
       }
//        for (int i=0; i<data.controlPoints.count(); i++) {
//            d = (data.controlPoints.at(i)).distanceTo(coord);

//            if (d<minDist) {
//                minDist = d;
//                ret = data.controlPoints.at(i);
//            }
//        }
    }
	if (dist) {
        *dist = minDist;
    }
    return ret;
}



/*
// The default implementation of RS_EntityContainer is inaccurate but
//   has to do for now..
RS_Vector RS_Spline::getNearestPointOnEntity(const RS_Vector& coord,
        bool onEntity, double* dist, RS_Entity** entity) {
}
*/



RS_Vector RS_Spline::getNearestCenter(const RS_Vector& /*coord*/,
									  double* dist) const{

	if (dist) {
        *dist = RS_MAXDOUBLE;
    }

    return RS_Vector(false);
}



// 2026-11-03 step 25: middle points and points at a distance along the curve (were "none")
RS_Vector RS_Spline::getNearestMiddle(const RS_Vector& coord,
                                      double* dist,
                                      int middlePoints)const {
    if (dist) {
        *dist = RS_MAXDOUBLE;
    }
    lc::splinegeom::Spline g;
    if (middlePoints < 1 || !curveData(g)) {
        return RS_Vector(false);
    }
    const double L = lc::splinegeom::length(g);
    RS_Vector best(false);
    double bestD = RS_MAXDOUBLE;
    for (int i = 1; i <= middlePoints; ++i) {
        const double t = lc::splinegeom::paramAtLength(g, g.t0(), L * i / (middlePoints + 1));
        const RS_Vector v = fromG(lc::splinegeom::eval(g, t));
        const double d = v.distanceTo(coord);
        if (d < bestD) {
            bestD = d;
            best = v;
        }
    }
    if (dist) {
        *dist = bestD;
    }
    return best;
}



RS_Vector RS_Spline::getNearestDist(double distance,
                                    const RS_Vector& coord,
									double* dist) const{
    if (dist)
        *dist = RS_MAXDOUBLE;
    lc::splinegeom::Spline g;
    if (data.closed || !curveData(g)) {
        return {};
    }
    // as RS_Line: `distance` along the curve from the endpoint nearer to coord
    const double L = lc::splinegeom::length(g);
    if (distance < 0. || distance > L) {
        return {};
    }
    const RS_Vector a = fromG(lc::splinegeom::eval(g, g.t0()));
    const RS_Vector b = fromG(lc::splinegeom::eval(g, g.t1()));
    const bool fromStart = coord.distanceTo(a) <= coord.distanceTo(b);
    const double t = lc::splinegeom::paramAtLength(g, g.t0(), fromStart ? distance : L - distance);
    const RS_Vector v = fromG(lc::splinegeom::eval(g, t));
    if (dist)
        *dist = v.distanceTo(coord);
    return v;
}



void RS_Spline::move(const RS_Vector& offset) {
    RS_EntityContainer::move(offset);
	for (RS_Vector& vp: data.controlPoints) {
		vp.move(offset);
    }
}



void RS_Spline::rotate(const RS_Vector& center, const double& angle) {
    rotate(center,RS_Vector(angle));
}



void RS_Spline::rotate(const RS_Vector& center, const RS_Vector& angleVector) {
	RS_EntityContainer::rotate(center, angleVector);
	for (RS_Vector& vp: data.controlPoints) {
		vp.rotate(center, angleVector);
	}
//    update();
}

void RS_Spline::scale(const RS_Vector& center, const RS_Vector& factor) {
	for (RS_Vector& vp: data.controlPoints) {
		vp.scale(center, factor);
	}

    update();
}

RS_Entity& RS_Spline::shear(double k)
{
    for (RS_Vector& vp: data.controlPoints) {
        vp.shear(k);
    }

    update();
    return *this;
}

void RS_Spline::mirror(const RS_Vector& axisPoint1, const RS_Vector& axisPoint2) {
	for (RS_Vector& vp: data.controlPoints) {
		vp.mirror(axisPoint1, axisPoint2);
	}

	update();
}



void RS_Spline::moveRef(const RS_Vector& ref, const RS_Vector& offset) {
	for (RS_Vector& vp: data.controlPoints) {
		if (ref.distanceTo(vp)<1.0e-4) {
			vp.move(offset);
		}
	}

    update();
}

// 2026-11-03 step 25: the knots are mirrored too (reversing only the points changed the shape of a spline
// with uneven knots, as from a DWG), and a closed spline drops its wrapped points first (reversed, they no
// longer repeat the first ones and update() would wrap them again).
void RS_Spline::revertDirection() {
    if (data.closed && hasWrappedControlPoints()) {
        data.controlPoints.erase(data.controlPoints.end() - data.degree, data.controlPoints.end());
    }
	std::reverse(data.controlPoints.begin(), data.controlPoints.end());
    if (!data.knotslist.empty()) {
        const double lo = data.knotslist.front();
        const double hi = data.knotslist.back();
        std::vector<double> k(data.knotslist.size());
        for (size_t i = 0; i < k.size(); ++i) {
            k[i] = lo + hi - data.knotslist[k.size() - 1 - i];
        }
        data.knotslist.swap(k);
    }
    update();
}

bool RS_Spline::curveData(lc::splinegeom::Spline& g) const {
    std::vector<GVec> cp;
    cp.reserve(data.controlPoints.size());
    for (const RS_Vector& v : data.controlPoints) {
        cp.push_back(toG(v));
    }
    return lc::splinegeom::build(data.degree, data.closed, std::move(cp), data.knotslist, g);
}

bool RS_Spline::curveAndLines(lc::splinegeom::Spline& g, std::vector<GVec>& pts, std::vector<double>& ts) const {
    if (!curveData(g)) {
        return false;
    }
    pts.clear();
    ts = m_tessParams;
    if (!entities.isEmpty() && size_t(entities.size()) + 1 == ts.size()) {
        pts.reserve(ts.size());
        for (RS_Entity* e : entities) {
            const auto* line = dynamic_cast<const RS_Line*>(e);
            if (line == nullptr) {
                pts.clear();
                break;
            }
            if (pts.empty()) {
                pts.push_back(toG(line->getStartpoint()));
            }
            pts.push_back(toG(line->getEndpoint()));
        }
    }
    if (pts.size() != ts.size() || pts.size() < 2) {
        lc::splinegeom::tessellate(g, lc::splinegeom::defaultChordError(g), pts, ts);
    }
    return pts.size() >= 2;
}

bool RS_Spline::hasCurve() const {
    lc::splinegeom::Spline g;
    return curveData(g);
}

double RS_Spline::paramStart() const {
    lc::splinegeom::Spline g;
    return curveData(g) ? g.t0() : 0.;
}

double RS_Spline::paramEnd() const {
    lc::splinegeom::Spline g;
    return curveData(g) ? g.t1() : 0.;
}

double RS_Spline::nearestParam(const RS_Vector& coord, double* dist) const {
    lc::splinegeom::Spline g;
    std::vector<GVec> pts;
    std::vector<double> ts;
    if (!curveAndLines(g, pts, ts)) {
        if (dist != nullptr) {
            *dist = RS_MAXDOUBLE;
        }
        return 0.;
    }
    return lc::splinegeom::nearestParam(g, pts, ts, toG(coord), dist);
}

RS_Vector RS_Spline::pointAt(double t, RS_Vector* derivative) const {
    lc::splinegeom::Spline g;
    if (!curveData(g)) {
        return RS_Vector(false);
    }
    GVec d1;
    const GVec c = lc::splinegeom::eval(g, t, &d1);
    if (derivative != nullptr) {
        *derivative = fromG(d1);
    }
    return fromG(c);
}

double RS_Spline::lengthBetween(double a, double b) const {
    lc::splinegeom::Spline g;
    return curveData(g) ? lc::splinegeom::length(g, std::max(a, g.t0()), std::min(b, g.t1())) : 0.;
}

double RS_Spline::paramAtLength(double a, double len) const {
    lc::splinegeom::Spline g;
    return curveData(g) ? lc::splinegeom::paramAtLength(g, a, len) : a;
}

bool RS_Spline::subSpline(double a, double b, RS_SplineData& out) const {
    lc::splinegeom::Spline g;
    lc::splinegeom::Spline piece;
    if (!curveData(g) || !lc::splinegeom::sub(g, a, b, piece)) {
        return false;
    }
    out = toData(piece);
    return true;
}

bool RS_Spline::splitAt(double t, RS_SplineData& first, RS_SplineData& second, bool& hasSecond) const {
    lc::splinegeom::Spline g;
    lc::splinegeom::Spline a;
    lc::splinegeom::Spline b;
    hasSecond = false;
    if (!curveData(g) || !lc::splinegeom::split(g, t, a, b, hasSecond)) {
        return false;
    }
    first = toData(a);
    if (hasSecond) {
        second = toData(b);
    }
    return true;
}

double RS_Spline::paramOnLine(const RS_Entity* line, const RS_Vector& v) const {
    const int i = entities.indexOf(const_cast<RS_Entity*>(line));
    if (i < 0 || size_t(i) + 1 >= m_tessParams.size()) {
        return -1.;
    }
    const RS_Vector a = line->getStartpoint();
    const RS_Vector b = line->getEndpoint();
    const double l2 = (b - a).squared();
    const double w = l2 > 0. ? std::clamp(RS_Vector::dotP(v - a, b - a) / l2, 0., 1.) : 0.;
    return m_tessParams[size_t(i)] + w * (m_tessParams[size_t(i) + 1] - m_tessParams[size_t(i)]);
}

double RS_Spline::extentSize() const {
    return (getMax() - getMin()).magnitude();
}

RS_Vector RS_Spline::getNearestPointOnEntity(const RS_Vector& coord, bool onEntity, double* dist,
                                             RS_Entity** entity) const {
    lc::splinegeom::Spline g;
    std::vector<GVec> pts;
    std::vector<double> ts;
    if (!curveAndLines(g, pts, ts)) {
        return RS_EntityContainer::getNearestPointOnEntity(coord, onEntity, dist, entity);
    }
    double d = 0.;
    const double t = lc::splinegeom::nearestParam(g, pts, ts, toG(coord), &d);
    if (dist != nullptr) {
        *dist = d;
    }
    if (entity != nullptr) {
        // as before: the line of the spline under the point (callers may expect an atomic entity)
        const auto it = std::upper_bound(ts.begin(), ts.end(), t);
        const int i = std::clamp(int(it - ts.begin()) - 1, 0, std::max(0, int(entities.size()) - 1));
        *entity = entities.isEmpty() ? const_cast<RS_Spline*>(this) : entities.at(i);
    }
    return fromG(lc::splinegeom::eval(g, t));
}

bool RS_Spline::isPointOnEntity(const RS_Vector& coord, double tolerance) const {
    lc::splinegeom::Spline g;
    std::vector<GVec> pts;
    std::vector<double> ts;
    if (!curveAndLines(g, pts, ts)) {
        return RS_EntityContainer::isPointOnEntity(coord, tolerance);
    }
    double d = RS_MAXDOUBLE;
    lc::splinegeom::nearestParam(g, pts, ts, toG(coord), &d);
    // the exact point of a snap still carries rounding: relative to the coordinates and the size
    const double floor = 1e-12 * (1. + std::abs(coord.x) + std::abs(coord.y)) + 1e-10 * extentSize();
    return d <= std::max(std::abs(tolerance), floor);
}

double RS_Spline::getLength() const {
    lc::splinegeom::Spline g;
    return curveData(g) ? lc::splinegeom::length(g) : RS_EntityContainer::getLength();
}

void RS_Spline::stretch(const RS_Vector& firstCorner, const RS_Vector& secondCorner, const RS_Vector& offset) {
    if (getMin().isInWindow(firstCorner, secondCorner) && getMax().isInWindow(firstCorner, secondCorner)) {
        move(offset);
        return;
    }
    bool moved = false;
    for (RS_Vector& v : data.controlPoints) {
        if (v.isInWindow(firstCorner, secondCorner)) {
            v.move(offset);
            moved = true;
        }
    }
    if (moved) {
        update();
    }
}

bool RS_Spline::offset(const RS_Vector& coord, const double& distance) {
    lc::splinegeom::Spline g;
    std::vector<GVec> pts;
    std::vector<double> ts;
    if (!curveAndLines(g, pts, ts) || !(std::abs(distance) > 0.)) {
        return false;
    }
    const GVec q = toG(coord);
    const double t = lc::splinegeom::nearestParam(g, pts, ts, q);
    const GVec c = lc::splinegeom::eval(g, t);
    const GVec tg = lc::splinegeom::tangentAt(g, t, 1);
    const double side = (q - c).dot(GVec(-tg.y, tg.x));
    const double d = side >= 0. ? std::abs(distance) : -std::abs(distance);
    const double eps = lc::splinegeom::defaultChordError(g);
    lc::splinegeom::OffsetError why = lc::splinegeom::OffsetError::None;
    if (g.p == 1) {
        // degree 1 is a polyline: its mitred offset is exact
        std::vector<GVec> off;
        std::vector<char> vertex;
        if (!lc::splinegeom::offsetPolyline(g, d, eps, off, vertex, &why) ||
            (!g.periodic && off.size() < 2) || off.size() < 3 - (g.periodic ? 0 : 1)) {
            return false;
        }
        data.controlPoints.clear();
        for (const GVec& v : off) {
            data.controlPoints.push_back(fromG(v));
        }
        data.knotslist.clear();
        data.closed = g.periodic;
        update();
        return true;
    }
    // Tiller-Hanson with knot refinement, corners joined (lc_splinegeom.h offsetSpline); a closed spline
    // comes back open, ending where it starts (LibreCAD's closed splines have uniform knots only)
    lc::splinegeom::Spline o;
    if (!lc::splinegeom::offsetSpline(g, d, eps, o, &why)) {
        RS_DEBUG->print(RS_Debug::D_WARNING, "RS_Spline::offset: %s",
                        why == lc::splinegeom::OffsetError::Radius ? "distance reaches the radius of curvature"
                                                                    : "no offset");
        return false;
    }
    const RS_SplineData od = toData(o);
    data.controlPoints = od.controlPoints;
    data.knotslist = od.knotslist;
    data.closed = false;
    update();
    return true;
}


void RS_Spline::draw(RS_Painter* painter, RS_GraphicView* view, double& /*patternOffset*/) {

	if (!(painter && view)) {
        return;
    }

    painter->drawSpline(*this, *view);
}



/**
 * Todo: draw the spline, user patterns.
 */
/*
void RS_Spline::draw(RS_Painter* painter, RS_GraphicView* view) {
   if (!(painter && view)) {
       return;
   }

   / *
      if (data.controlPoints.count()>0) {
          RS_Vector prev(false);
          QList<RS_Vector>::iterator it;
          for (it = data.controlPoints.begin(); it!=data.controlPoints.end(); ++it) {
              if (prev.valid) {
                  painter->drawLine(view->toGui(prev),
                                    view->toGui(*it));
              }
              prev = (*it);
          }
      }
   * /

   int i;
   int npts = data.controlPoints.count();
   // order:
   int k = 4;
   // resolution:
   int p1 = 100;

   double* b = new double[npts*3+1];
   double* h = new double[npts+1];
   double* p = new double[p1*3+1];

   QList<RS_Vector>::iterator it;
   i = 1;
   for (it = data.controlPoints.begin(); it!=data.controlPoints.end(); ++it) {
       b[i] = (*it).x;
       b[i+1] = (*it).y;
       b[i+2] = 0.0;

        RS_DEBUG->print("RS_Spline::draw: b[%d]: %f/%f", i, b[i], b[i+1]);
        i+=3;
   }

   // set all homogeneous weighting factors to 1.0
   for (i=1; i <= npts; i++) {
       h[i] = 1.0;
   }

   //
   for (i = 1; i <= 3*p1; i++) {
       p[i] = 0.0;
   }

   rbspline(npts,k,p1,b,h,p);

   RS_Vector prev(false);
   for (i = 1; i <= 3*p1; i=i+3) {
       if (prev.valid) {
           painter->drawLine(view->toGui(prev),
                             view->toGui(RS_Vector(p[i], p[i+1])));
       }
       prev = RS_Vector(p[i], p[i+1]);
   }
}
*/



/**
 * @return The reference points of the spline.
 */
const std::vector<RS_Vector>& RS_Spline::getControlPoints() const{
    return data.controlPoints;
}



/**
 * Appends the given point to the control points.
 */
void RS_Spline::addControlPoint(const RS_Vector& v) {
    if (isClosed() && hasWrappedControlPoints()) {
        data.controlPoints.insert(data.controlPoints.begin() + (data.controlPoints.size() - data.degree), v );
    } else {
        data.controlPoints.push_back(v);
    }
}



/**
 * Removes the control point that was last added.
 */
void RS_Spline::removeLastControlPoint() {
    if (isClosed() && hasWrappedControlPoints()) {
        data.controlPoints.erase(data.controlPoints.begin() + (data.controlPoints.size() - data.degree));
    } else {
        data.controlPoints.pop_back();
    }
}


/**
 * @brief hasWrappedControlPoints whether the control points are wrapped, needed for a closed spline.
 * @return bool - true, if the control points are already wrapped.
 *          the last degree control points are the same as the first degree control points.
 */
bool RS_Spline::hasWrappedControlPoints() const {
    const std::vector<RS_Vector>& controlPoints = data.controlPoints;
    if (!data.closed || data.degree < 1 || controlPoints.size() < size_t(2 * data.degree))
        return false;

    return std::equal(controlPoints.cbegin(), controlPoints.cbegin() + data.degree,
               controlPoints.cbegin() + controlPoints.size() - data.degree);
}

//TODO: private interface cleanup; de Boor's Algorithm
/**
 * Generates B-Spline open knot vector with multiplicity
 * equal to the order at the ends.
 */
std::vector<double> RS_Spline::knot(size_t num, size_t order) const{
	if (data.knotslist.size() == num + order) {
		//use custom knot vector
		return data.knotslist;
	}

	std::vector<double> knotVector(num + order, 0.);
	//use uniform knots
	std::iota(knotVector.begin() + order, knotVector.begin() + num + 1, 1);
	std::fill(knotVector.begin() + num + 1, knotVector.end(), knotVector[num]);
	return knotVector;
}



/**
 * Generates rational B-spline basis functions for an open knot vector.
 */
namespace{
std::vector<double> rbasis(int c, double t, int npts,
                                      const std::vector<double>& x,
                                      const std::vector<double>& h) {

	int const nplusc = npts + c;

	std::vector<double> temp(nplusc,0.);

    // calculate the first order nonrational basis functions n[i]
	for (int i = 0; i< nplusc-1; i++)
		if ((t >= x[i]) && (t < x[i+1])) temp[i] = 1;

    /* calculate the higher order nonrational basis functions */

	for (int k = 2; k <= c; k++) {
		for (int i = 0; i < nplusc-k; i++) {
			// if the lower order basis function is zero skip the calculation
            if (temp[i] != 0)
				temp[i] = ((t-x[i])*temp[i])/(x[i+k-1]-x[i]);
            // if the lower order basis function is zero skip the calculation
            if (temp[i+1] != 0)
				temp[i] += ((x[i+k]-t)*temp[i+1])/(x[i+k]-x[i+1]);
        }
    }

    // pick up last point
	if (t >= x[nplusc-1]) temp[npts-1] = 1;

    // calculate sum for denominator of rational basis functions
	double sum = 0.;
	for (int i = 0; i < npts; i++) {
		sum += temp[i]*h[i];
    }

	std::vector<double> r(npts, 0);
    // form rational basis functions and put in r vector
	if (sum != 0) {
		for (int i = 0; i < npts; i++)
			r[i] = (temp[i]*h[i])/sum;
	}
	return r;
}
}


/**
 * Generates a rational B-spline curve using a uniform open knot vector.
 */
void RS_Spline::rbspline(size_t npts, size_t k, size_t p1,
                         const std::vector<RS_Vector>& b,
                         const std::vector<double>& h,
                         std::vector<RS_Vector>& p) const{
	size_t const nplusc = npts + k;

	// generate the open knot vector
	auto const x = knot(npts, k);

    // calculate the points on the rational B-spline curve
    double t {x[0]};
    double const step {(x[nplusc-1] - t) / (p1-1)};

	for (auto& vp: p) {
		if (x[nplusc-1] - t < 5e-6) t = x[nplusc-1];

        // generate the basis function for this value of t
		auto const nbasis = rbasis(k, t, npts, x, h);

        // generate a point on the curve
		for (size_t i = 0; i < npts; i++)
			vp += b[i] * nbasis[i];

		t += step;
    }

}



std::vector<double> RS_Spline::knotu(size_t num, size_t order) const{
	if (data.knotslist.size() == num + order) {
		//use custom knot vector
		return data.knotslist;
	}
	std::vector<double> knotVector(num + order, 0.);
	std::iota(knotVector.begin(), knotVector.end(), 0);
	return knotVector;
}



void RS_Spline::rbsplinu(size_t npts, size_t k, size_t p1,
                         const std::vector<RS_Vector>& b,
                         const std::vector<double>& h,
                         std::vector<RS_Vector>& p) const{
	size_t const nplusc = npts + k;

	/* generate the periodic knot vector */
	std::vector<double> const x = knotu(npts, k);

    /*    calculate the points on the rational B-spline curve */
	double t = k-1;
	double const step = double(npts - k + 1)/(p1 - 1);

	for (auto& vp: p) {
		if (x[nplusc-1] - t < 5e-6) t = x[nplusc-1];

		/* generate the basis function for this value of t */
		auto const nbasis = rbasis(k, t, npts, x, h);
		/* generate a point on the curve, for x, y, z */
		for (size_t i = 0; i < npts; i++)
			vp += b[i] * nbasis[i];

		t += step;
    }

}


/**
 * Dumps the spline's data to stdout.
 */
std::ostream& operator << (std::ostream& os, const RS_Spline& l) {
    os << " Spline: " << l.getData() << "\n";
    return os;
}
