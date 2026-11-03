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


#ifndef RS_SPLINE_H
#define RS_SPLINE_H

#include <vector>
#include "rs_entitycontainer.h"

namespace lc {
namespace splinefit {
struct Vec;
}
namespace splinegeom {
struct Spline;
}
}

/**
 * Holds the data that defines a line.
 */
struct RS_SplineData {
	/**
	 * Default constructor. Leaves the data object uninitialized.
	 */
	RS_SplineData() = default;

	RS_SplineData(int degree, bool closed);


	/** Degree of the spline (1, 2, 3) */
    int degree = 3;
	/** Closed flag. */
    bool closed = false;
	/** Control points of the spline. */
	std::vector<RS_Vector> controlPoints;
	std::vector<double> knotslist;
};

std::ostream& operator << (std::ostream& os, const RS_SplineData& ld);

/**
 * Class for a spline entity.
 *
 * @author Andrew Mustun
 */
class RS_Spline : public RS_EntityContainer {
public:
    RS_Spline(RS_EntityContainer* parent,
            const RS_SplineData& d);

	RS_Entity* clone() const override;


    /**	@return RS2::EntitySpline */
	RS2::EntityType rtti() const override{
        return RS2::EntitySpline;
    }
    /** @return false */
	bool isEdge() const override{
        return false;
    }

	/** @return Copy of data that defines the spline. */
	const RS_SplineData& getData() const {
		return data;
	}

	/** Sets the splines degree (1-3). */
    void setDegree(int degree);

	/** @return Degree of this spline curve (1-3).*/
    int getDegree() const;

	/** @return 0. */
	int getNumberOfKnots() {
		return 0;
	}

	/** @return Number of control points. */
	size_t getNumberOfControlPoints() const;

	/**
		 * @retval true if the spline is closed.
		 * @retval false otherwise.
		 */
	bool isClosed() const;

	/**
		 * Sets the closed flag of this spline.
		 */
	void setClosed(bool c);

	RS_VectorSolutions getRefPoints() const override;
	RS_Vector getNearestRef( const RS_Vector& coord, double* dist = nullptr) const override;
	RS_Vector getNearestSelectedRef( const RS_Vector& coord, double* dist = nullptr) const override;

    /** @return Start point of the entity */
	RS_Vector getStartpoint() const override;
    /** @return End point of the entity */
	RS_Vector getEndpoint() const override;
    /** Sets the startpoint */
    /** Sets the endpoint */
	void update() override;

	RS_Vector getNearestEndpoint(const RS_Vector& coord,
										 double* dist = nullptr)const override;
	//RS_Vector getNearestPointOnEntity(const RS_Vector& coord,
	//        bool onEntity=true, double* dist = nullptr, RS_Entity** entity=nullptr);
	RS_Vector getNearestCenter(const RS_Vector& coord,
									   double* dist = nullptr)const override;
	RS_Vector getNearestMiddle(const RS_Vector& coord,
									   double* dist = nullptr,
									   int middlePoints = 1)const override;
	RS_Vector getNearestDist(double distance,
                                     const RS_Vector& coord,
									 double* dist = nullptr)const override;

		void addControlPoint(const RS_Vector& v);
		void removeLastControlPoint();

		void move(const RS_Vector& offset) override;
		void rotate(const RS_Vector& center, const double& angle) override;
		void rotate(const RS_Vector& center, const RS_Vector& angleVector) override;
		void scale(const RS_Vector& center, const RS_Vector& factor) override;
		void mirror(const RS_Vector& axisPoint1, const RS_Vector& axisPoint2) override;
        RS_Entity& shear(double k) override;

		void moveRef(const RS_Vector& ref, const RS_Vector& offset) override;
		void revertDirection() override;

		void draw(RS_Painter* painter, RS_GraphicView* view, double& patternOffset) override;
		const std::vector<RS_Vector>& getControlPoints() const;
		/**
		 * Points along the curve, computed exactly as update() computes the lines it draws:
		 * segmentsPerPoint * (number of control points, wrapped ones included) points, the file's
		 * knot vector when it has the right size. A closed spline ends on its first point.
		 * update() uses $SPLINESEGS; the spline explode actions pass their own count.
		 */
		std::vector<RS_Vector> getStrokePoints(int segmentsPerPoint) const;

        friend std::ostream& operator << (std::ostream& os, const RS_Spline& l);

		void calculateBorders() override;

		/*
		 * 2026-11-03 (plans-win librecad-factory-drawings step 25): the exact curve (lc_splinegeom.h)
		 * instead of the lines inside. update() makes those lines within a chord error of the curve
		 * (they were $SPLINESEGS per control point, off by more than a vp tolerance on few points), and
		 * the functions below answer from the curve itself. Parameters t run over [paramStart(),
		 * paramEnd()]; a closed spline's run once round.
		 */
		RS_Vector getNearestPointOnEntity(const RS_Vector& coord, bool onEntity = true,
										  double* dist = nullptr, RS_Entity** entity = nullptr) const override;
		double getLength() const override;
		/** On the curve itself (the lines inside are off it by up to their chord error). */
		bool isPointOnEntity(const RS_Vector& coord, double tolerance = 20. * RS_TOLERANCE) const override;
		/** Moves the control points inside the window (all of it inside: moves the spline). */
		void stretch(const RS_Vector& firstCorner, const RS_Vector& secondCorner,
					 const RS_Vector& offset) override;
		/** Parallel curve at `distance` on coord's side, fitted back to a spline (see lc_splinegeom.h). */
		bool offset(const RS_Vector& coord, const double& distance) override;

		/** Enough control points for a curve. */
		bool hasCurve() const;
		double paramStart() const;
		double paramEnd() const;
		double nearestParam(const RS_Vector& coord, double* dist = nullptr) const;
		RS_Vector pointAt(double t, RS_Vector* derivative = nullptr) const;
		double lengthBetween(double a, double b) const;
		/** The parameter `len` along the curve after a (clamped to the end). */
		double paramAtLength(double a, double len) const;
		/** The piece [a, b] as an open spline with its own knots (layer/pen not included). */
		bool subSpline(double a, double b, RS_SplineData& out) const;
		/** Open: [start, t] and [t, end]; closed: one open spline from t round to t (hasSecond false). */
		bool splitAt(double t, RS_SplineData& first, RS_SplineData& second, bool& hasSecond) const;
		/** Parameter for a point v on one of the lines inside (that line's share of the parameter); -1: not ours. */
		double paramOnLine(const RS_Entity* line, const RS_Vector& v) const;
		/** Diagonal of the box around the curve. */
		double extentSize() const;

        friend class RS_FilterDXFRW;

private:
		/** The curve and the points of the lines inside with their parameters (false: no curve). */
		bool curveAndLines(lc::splinegeom::Spline& g, std::vector<lc::splinefit::Vec>& pts,
						   std::vector<double>& ts) const;
		std::vector<double> m_tessParams; ///< parameter of each line point made by update()

		std::vector<double> knot(size_t num, size_t order) const;
		void rbspline(size_t npts, size_t k, size_t p1,
		              const std::vector<RS_Vector>& b,
		              const std::vector<double>& h,
		              std::vector<RS_Vector>& p) const;

		std::vector<double> knotu(size_t num, size_t order) const;
		void rbsplinu(size_t npts, size_t k, size_t p1,
		              const std::vector<RS_Vector>& b,
		              const std::vector<double>& h,
		              std::vector<RS_Vector>& p) const;

        /**
         * @brief hasWrappedControlPoints whether the control points are wrapped, needed for a closed spline.
         *          only implemented for cubic splines
         * @return bool - true, if the control points are already wrapped.
         *          for a cubic spline with wrapped splines, the last three control points are the same as the first three.
         */
        bool hasWrappedControlPoints() const;
        bool curveData(lc::splinegeom::Spline& g) const;

protected:
		RS_SplineData data;
}
;

#endif
