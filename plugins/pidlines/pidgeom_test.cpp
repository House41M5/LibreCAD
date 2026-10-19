// pidgeom_test.cpp - round trip check for pidgeom.h: cut a path, turn the pieces
// into what LibreCAD hands back (arcs always CCW, polylines via bulges), heal
// them through the symbols and compare with the original.
// Build: see build_pidgeom_test.bat (cl + Qt5Core). Exit code = number of failures.

#include <cstdio>
#include "pidgeom.h"

using namespace pidgeom;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// What a piece looks like after addLine/addArc/addPolyline and getData/getPolylineData.
static Path asEntity(QList<Seg> segs, bool closed = false) {
    Path p;
    p.closed = closed;
    if (closed && segs.size() == 1) {   // same split as addSegs in pidlines.cpp
        const Seg s = segs.takeFirst();
        segs << subSeg(s, 0, s.len / 2) << subSeg(s, s.len / 2, s.len);
    }
    if (segs.size() == 1 && !closed) {
        const Seg &s = segs.first();
        p.segs << (s.arc && s.sweep < 0 ? reversed(s) : s);   // addArc is CCW only
        return p;
    }
    QList<QPair<QPointF, double>> v;
    for (const Seg &s: segs) v << qMakePair(segStart(s), bulgeOf(s));
    if (!closed) v << qMakePair(segEnd(segs.last()), 0.0);
    for (int i = 0; i + 1 < v.size(); ++i) p.segs << bulgeSeg(v[i].first, v[i + 1].first, v[i].second);
    if (closed) p.segs << bulgeSeg(v.last().first, v.first().first, v.last().second);
    return p;
}

static bool sameSeg(const Seg &a, const Seg &b) {
    if (a.arc != b.arc) return false;
    if (dist(segStart(a), segStart(b)) > 1e-6 || dist(segEnd(a), segEnd(b)) > 1e-6) return false;
    if (a.arc && (dist(a.c, b.c) > 1e-6 || std::fabs(a.r - b.r) > 1e-6 || std::fabs(a.sweep - b.sweep) > 1e-9)) return false;
    return true;
}

static void run(const char *name, Path orig, double spacing, double gap, int expectCuts = -1) {
    std::printf("%s\n", name);
    orig.segs = simplify(orig.segs);
    {   // first placement goes through walkChain on the single uncut entity, as in the plugin
        QList<Path> one;
        one << orig;
        QVector<Links> none(1);
        Chain c = walkChain(one, none, 0);
        CHECK(c.merged.closed == orig.closed, "single entity: closed %d -> %d", int(orig.closed), int(c.merged.closed));
        orig = c.merged;
    }
    const double L = orig.length();
    QList<Station> st = planStations(orig, spacing, gap);
    int cuts = 0;
    for (const Station &s: st) cuts += s.cut;
    if (expectCuts >= 0) CHECK(cuts == expectCuts, "cuts %d, expected %d", cuts, expectCuts);

    const QList<QList<Seg>> pc = pieces(orig, st, gap);
    double plen = 0;
    QList<Path> ents;
    for (const QList<Seg> &segs: pc) {
        ents << asEntity(segs, cuts == 0 && orig.closed);
        plen += ents.last().length();
    }
    CHECK(std::fabs(plen - (L - cuts * gap)) < 1e-6, "pieces length %.9f, expected %.9f", plen, L - cuts * gap);

    // symbols: cut ones sit in a gap, the others on a piece
    QList<QPointF> ins;
    QList<double> reach;
    for (const Station &s: st) {
        QPointF pt; double ang;
        pathAt(orig, s.t, &pt, &ang);
        ins << pt;
        reach << 10.0;
        double d = 1e300;
        for (const Path &e: ents) d = std::min(d, pathDist(e, pt));
        // straight distance to the piece end: half the gap on a line, the (shorter) chord on an arc
        if (s.cut) CHECK(d <= gap / 2 + 1e-6 && d > 0.98 * gap / 2, "cut symbol at t=%.3f is %.6f from the pieces", s.t, d);
        else CHECK(d < 1e-6, "uncut symbol at t=%.3f is %.6f off the line", s.t, d);
    }

    QVector<Links> links;
    QVector<bool> used = linkPaths(ents, ins, reach, &links);
    for (int i = 0; i < st.size(); ++i)
        CHECK(used[i] == st[i].cut, "symbol %d linked=%d cut=%d", i, int(used[i]), int(st[i].cut));

    // heal starting from every piece: the result must not depend on what was clicked
    for (int startIdx = 0; startIdx < ents.size(); ++startIdx) {
        Chain ch = walkChain(ents, links, startIdx);
        CHECK(ch.members.size() == ents.size(), "start %d: chain has %d of %d pieces", startIdx,
              int(ch.members.size()), int(ents.size()));
        CHECK(ch.gapSymbols.size() == cuts, "start %d: %d gap symbols, expected %d", startIdx,
              int(ch.gapSymbols.size()), cuts);
        CHECK(ch.merged.closed == orig.closed, "start %d: closed=%d", startIdx, int(ch.merged.closed));
        CHECK(std::fabs(ch.merged.length() - L) < 1e-6, "start %d: healed length %.9f vs %.9f", startIdx,
              ch.merged.length(), L);
        if (orig.closed) continue;   // a loop may start anywhere; length + closed is the check
        Path m = ch.merged;
        if (dist(m.start(), orig.start()) > 1e-6) m = reversed(m);
        m.segs = simplify(m.segs);
        bool same = m.segs.size() == orig.segs.size();
        for (int i = 0; same && i < m.segs.size(); ++i) same = sameSeg(m.segs[i], orig.segs[i]);
        CHECK(same, "start %d: healed geometry differs (%d segs vs %d)", startIdx, int(m.segs.size()),
              int(orig.segs.size()));
    }
    std::printf("  stations=%d cut=%d pieces=%d L=%.3f\n", int(st.size()), cuts, int(ents.size()), L);
}

static Path line(QPointF a, QPointF b) { Path p; p.segs << lineSeg(a, b); return p; }
static Path poly(const QList<QPair<QPointF, double>> &v, bool closed = false) {
    Path p;
    p.closed = closed;
    for (int i = 0; i + 1 < v.size(); ++i) p.segs << bulgeSeg(v[i].first, v[i + 1].first, v[i].second);
    if (closed) p.segs << bulgeSeg(v.last().first, v.first().first, v.last().second);
    return p;
}

// Two parallel lines 1 mm apart cut at the same stations, plus a line crossing
// them: every chain must keep to its own line.
static void neighbours() {
    std::printf("N parallel lines 1 mm apart + crossing line\n");
    QList<Path> origs;
    origs << line({0, 0}, {100, 0}) << line({0, 1}, {100, 1}) << line({52.5, -20}, {52.5, 20});
    QList<Path> ents;
    QList<int> owner;
    QList<QPointF> ins;
    QList<double> reach;
    for (int o = 0; o < origs.size(); ++o) {
        QList<Station> st = planStations(origs[o], 15, 1.5);
        for (const QList<Seg> &segs: pieces(origs[o], st, 1.5)) { ents << asEntity(segs); owner << o; }
        for (const Station &s: st) {
            QPointF pt; double ang;
            pathAt(origs[o], s.t, &pt, &ang);
            ins << pt;
            reach << 10.0;
        }
    }
    QVector<Links> links;
    linkPaths(ents, ins, reach, &links);
    for (int i = 0; i < ents.size(); ++i) {
        Chain ch = walkChain(ents, links, i);
        bool own = true;
        for (int m: ch.members) own = own && owner[m] == owner[i];
        CHECK(own, "chain from piece %d mixes lines", i);
        CHECK(std::fabs(ch.merged.length() - origs[owner[i]].length()) < 1e-6, "chain from piece %d: length %.6f",
              i, ch.merged.length());
    }
}

int main() {
    neighbours();
    run("A line left->right", line({0, 0}, {100, 0}), 15, 1.5, 6);
    run("B line right->left", line({100, 30}, {0, 30}), 15, 1.2, 6);
    run("C vertical", line({130, 0}, {130, 80}), 15, 2.0, 5);
    { Path p; p.segs << arcSeg({60, 80}, 30, 0, kPi); run("D arc CCW", p, 15, 1.5, 6); }
    { Path p; p.segs << arcSeg({60, 80}, 30, kPi, -kPi); run("D' arc CW (LibreCAD reversed)", p, 15, 3.0, 6); }
    run("E polyline + bulge", poly({{{160, 0}, 0}, {{220, 0}, 0.5}, {{220, 40}, 0}, {{180, 60}, 0}}), 15, 1.5);
    run("F joint under a station", poly({{{0, 0}, 0}, {{26.25, 0}, 0}, {{26.25, 40}, 0}}), 15, 1.5, 4);
    run("G closed square", poly({{{0, 0}, 0}, {{50, 0}, 0}, {{50, 50}, 0}, {{0, 50}, 0}}, true), 15, 3.0);
    run("H closed, one symbol", poly({{{0, 0}, 0}, {{4, 0}, 0}, {{4, 4}, 0}}, true), 15, 1.5, 1);
    run("I shorter than the gap", line({0, 0}, {1, 0}), 15, 1.5, 0);
    run("J em_guided gap 3, one symbol", line({0, 0}, {20, 0}), 15, 3.0, 1);
    run("K spacing tighter than gap", line({0, 0}, {40, 0}), 2, 3.0, 0);
    run("L no cut (gap 0)", line({0, 0}, {100, 0}), 15, 0, 0);
    { Path p = poly({{{0, 0}, 0}, {{30, 0}, -0.4}, {{60, 0}, 0.4}, {{90, 0}, 0}}); run("M S-curve polyline", p, 12, 1.5); }
    run("O circle as closed polyline (two bulge-1 arcs)", poly({{{0, 0}, 1}, {{20, 0}, 1}}, true), 15, 1.5);
    run("P circle, gap 0 (stays one closed entity)", poly({{{0, 0}, 1}, {{20, 0}, 1}}, true), 15, 0, 0);
    std::printf(failures ? "FAILED: %d\n" : "ALL PASS\n", failures);
    return failures;
}
