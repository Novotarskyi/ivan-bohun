// ============================================================================
// KURIN - the barracks: a ventilated case for one LILYGO T-ETH-Lite blade.
// IVAN BOHUN.  ONE base + ONE lid, identical for all four blades;
// mark them with a paint pen after printing rather than reprinting per name.
//
// ORIENTATION (as built):
//   the blade sits LED/solder face UP, header pins and the chip side DOWN.
//   The RJ45 is soldered to the board, ~2 mm above its top face - so the jack's
//   height off the shelf IS the board's height off the shelf. The cable has to
//   enter low, near the feet, which means the board has to sit low.
//
// WHY THE BOARD SITS LOW. An earlier revision put a 46 mm wiring plenum UNDER the board, which pushed the
// board 48 mm up and the RJ45 window with it - halfway up a 67 mm tower. Two
// faults: the jack was nowhere near the shelf, and the blade could only be fitted
// by lowering it down a 29 mm shaft and threading duponts into a blind cavity.
// The under-board gap does NOT need to hold the wiring. It only needs to clear
// the pins and the dupont housings pushed onto them. THE BAY holds the loom.
// ============================================================================

// ---- pick what to render ----
part  = "both";        // "base" | "lid" | "both" (preview)

// ---- the blade (measured with a roller rule - loose by intent) ----
// Rough numbers deserve slack: a case that rattles is a nuisance, a case that
// will not close is a reprint. Tighten `fit` and `lip_fit` after a test print.
board_l   = 55;        // measured ~55 mm
board_w   = 28;        // measured ~28 mm
pcb_t     = 1.6;
fit       = 2.4;       // side clearance, deliberately generous. A board that
                       // rattles 1 mm is fine; one that will not go in is four
                       // wasted prints.
// The RJ45 overhangs the PCB by 2-3 mm on the clip side. `nose` must exceed that
// or the jack meets the wall before the board is home.
nose      = 5.0;

// ---- the height budget, which is the whole design ----
// BELOW the board: 11 mm of pin, plus a dupont housing seated over it. This sets
// how low the RJ45 can possibly sit, and it is a floor we cannot argue with -
// it comes from the headers being soldered pins-down toward the chip side.
stand_h   = 16;        // pin 11 + housing + a little; the RJ45 rides this high
// ABOVE the board: the RJ45 body, then room for the loom to arc over the board
// and run aft into the bay. The wires live here and in the bay, NOT underneath.
above     = 26;        // ~13.5 jack + ~12 of loom headroom

// ---- the wire bay: where the loom and the breakout actually live ----
// Sized by the VERTICAL breakout, the awkward one: 30 mm tall x 15 wide with its
// port protruding ~10 mm. Needs 15 + 10 + slack of depth and 30 mm of height.
// Both horizontal breakouts (20 x 15, ~1 mm overhang) fit inside that envelope.
bay_l     = 34;

// ---- shell ----
wall      = 2.4;
floor_t   = 2.4;
lid_t     = 2.4;
lip_d     = 3;         // lid lip drops inside the cavity
lip_fit   = 0.40;      // per-side lid clearance. Loosened for the same reason:
                       // a lid that is slightly loose can be taped; one that
                       // does not fit cannot be rescued without a printer.

// ---- openings ----
rj45_w    = 22;  rj45_h = 18;  rj45_xoff = 0;   // oversized further - the jack's
                                                // position on the board was eyeballed,
                                                // and an over-large hole still works
rj45_drop = 1.5;       // start the cut below the PCB top - the jack body sits on
                       // the board and a little margin costs nothing

// ONE SLOT FITS ALL THREE BREAKOUTS. The port is a standard 7-8 x 2-3 mm, but the
// three boards present it at wildly different heights. Rather than guess one, the
// opening is a tall rounded slot: whichever goes in, its port meets it somewhere.
usb_w     = 16;        // 2x the port width, room for a cable boot
usb_z0    = 4;
usb_z1    = 34;

// LEDs (measured): three of them start 6 mm in from the RJ45-end board edge, span
// 6 mm, sit 2 mm from one long edge, with 4-5 mm clear to the nearest pin - that
// last number is the limit on widening the window before it shows solder.
// WHICH LONG EDGE. Set right by the first test print: the window
// came out on the far edge from the LEDs. Root cause, and it was not a guess -
// the soldering doc records that the LEDs live on the SOLDER side, and this case lies
// the blade solder-face UP. A feature positioned from a component-side view is
// therefore MIRRORED in y, every time, and the mirror was never applied.
// One constant now, read by BOTH the lid window and the split rib, so the two
// cannot drift apart again.
//   +1 = high-y wall (the original, wrong)      -1 = low-y wall (as printed)
led_side  = -1;
led_sx    = (board_w / 2) - 2;   // 12 mm off the centreline
led_sy    = 6;                   // cluster starts 6 mm from the board's nose edge
led_sl    = 12;  led_sw = 6;     // window, centred on the 6 mm cluster

// ---- ventilation ----
vent_w    = 3;   vent_p = 12;
// Bands with solid ribs between, not one tall slot per column: the first render
// showed a wall that was more hole than wall. Same open area, far stiffer.
// Bands are [z above floor, height]. The top one MUST clear the rim: the lid lip
// drops 3 mm inside, so anything reaching past ~41 mm turns the wall into
// battlements instead of a wall. The first v3 render did exactly that.
vent_bands = [[5, 12], [21, 11], [34, 4]];

$fn = 36;

// ---- derived ----
// Pin rows run down both long edges ~2 mm in from the PCB edge, pointing DOWN.
// The board therefore cannot rest on a normal ledge - it would land on its own
// pins. Support is four corner posts inboard of nothing and outboard of the pins:
// they rise to the board plane and take only a 2 mm bite at each corner, where
// there are no pins at all. A retaining rib above traps the board when closed.
post_w  = 4;                         // corner post footprint - kept small because
                                     // the pin rows' exact extent was never measured
post_b  = 2;                         // how far a post reaches under the board
rib_gap = 0.4;
// Posts and ribs must OVERLAP the wall, never sit flush against it. Two solids
// sharing an exact coplanar face is the classic CGAL non-manifold trigger, and
// it is what made the first one-shot export unprintable.
bite    = 0.6;                       // how far they bury into the wall
in_w    = board_w + fit;
in_l    = nose + board_l + bay_l;
in_h    = stand_h + pcb_t + above;
ow      = in_w + 2*wall;
ol      = in_l + 2*wall;
oh      = floor_t + in_h;
brd_z   = floor_t + stand_h;         // the PCB's underside
brd_x0  = wall + nose;               // the PCB's nose edge

module base() {
    difference() {
        cube([ol, ow, oh]);
        // cavity
        translate([wall, wall, floor_t]) cube([in_l, in_w, in_h + 1]);

        // RJ45 - wall A. Now LOW: it starts just under the board at ~18 mm, so the
        // ethernet lead enters close to the shelf instead of halfway up a tower.
        translate([-1, wall + in_w/2 + rj45_xoff - rj45_w/2, brd_z + pcb_t - rj45_drop])
            cube([wall + 2, rj45_w, rj45_h]);

        // USB-C - wall B, a tall rounded slot spanning the bay's working height
        translate([ol - wall - 1, wall + in_w/2, floor_t]) rotate([0, 90, 0])
            hull() {
                translate([-usb_z0, 0, 0]) cylinder(h = wall + 2, d = usb_w);
                translate([-usb_z1, 0, 0]) cylinder(h = wall + 2, d = usb_w);
            }

        // side-wall vents
        for (b = vent_bands)
            for (x = [wall + 6 : vent_p : ol - wall - 6 - vent_w])
                for (y = [-1, ow - wall - 1])
                    translate([x, y, floor_t + b[0]]) cube([vent_w, wall + 2, b[1]]);

        // floor vents - under the board gap and the bay
        for (x = [wall + 8 : vent_p * 1.5 : ol - wall - 10])
            translate([x, wall + 6, -1]) cube([vent_w, in_w - 12, floor_t + 2]);

        // zip-tie slots in the bay floor: the loom's strain relief
        for (y = [wall + 5, ow - wall - 9])
            translate([ol - wall - bay_l/2 - 2, y, -1]) cube([4, 4, floor_t + 2]);
    }

    // Four corner posts up to the board plane. At the corners because that is the
    // only place the PCB has no pins - the rows stop short of the ends.
    for (x = [brd_x0, brd_x0 + board_l - post_w]) {
        translate([x, wall - bite, floor_t])
            cube([post_w, post_w + bite, stand_h]);
        translate([x, ow - wall - post_w, floor_t])
            cube([post_w, post_w + bite, stand_h]);
    }

    // Retaining ribs above the board edge. They hug the walls to clear the pins -
    // but the LED cluster ALSO sits 2 mm in from a board edge, so a full-length rib
    // would land squarely on the LEDs and blank the window. The rib on that side is
    // split, with a gap spanning the cluster plus margin.
    // Built as one union of separated solids: the first attempt let segments meet
    // the corner posts at coincident faces and exported non-manifold.
    rib_z = brd_z + pcb_t + rib_gap;
    // Which wall is which follows led_side, so the rib split and the lid window
    // always land on the same edge. Previously both were hard-coded to the
    // high-y wall and both were wrong together.
    led_wall_y = (led_side > 0) ? ow - wall - post_b : wall - bite;
    far_wall_y = (led_side > 0) ? wall - bite : ow - wall - post_b;
    // far wall (no LEDs): one continuous rib, inset from the posts at both ends
    translate([brd_x0 + post_w + 1, far_wall_y, rib_z])
        cube([board_l - 2*post_w - 2, post_b + bite, 2]);
    // LED wall: two segments, skipping the cluster zone
    translate([brd_x0 + post_w + 1, led_wall_y, rib_z])
        cube([max(1, led_sy - 4 - post_w - 1), post_b + bite, 2]);
    translate([brd_x0 + led_sy + 10, led_wall_y, rib_z])
        cube([board_l - led_sy - 10 - post_w - 1, post_b + bite, 2]);

    // feet
    for (p = [[6, 6], [ol - 10, 6], [6, ow - 10], [ol - 10, ow - 10]])
        translate([p[0], p[1], -1.2]) cylinder(h = 1.2, d = 7);
}

module lid() {
    difference() {
        union() {
            cube([ol, ow, lid_t]);
            translate([wall + lip_fit, wall + lip_fit, -lip_d])
                difference() {
                    cube([in_l - 2*lip_fit, in_w - 2*lip_fit, lip_d]);
                    translate([1.6, 1.6, -1])
                        cube([in_l - 2*lip_fit - 3.2, in_w - 2*lip_fit - 3.2, lip_d + 2]);
                }
        }
        // LED window, centred on the measured cluster, on the led_side edge
        translate([brd_x0 + led_sy - (led_sl - 6) / 2,
                   ow / 2 + led_side * led_sx - led_sw / 2, -lip_d - 1])
            cube([led_sl, led_sw, lid_t + lip_d + 2]);
        // vents over the bay, where the loom sits
        for (x = [ol - wall - bay_l + 2 : vent_p : ol - wall - 4])
            translate([x, wall + 5, -1]) cube([vent_w, in_w - 10, lid_t + 2]);
        // finger notches
        for (x = [-1, ol - 4]) translate([x, ow/2 - 8, lid_t - 1]) cube([5, 16, 2]);
    }
}

if (part == "base" || part == "both") base();
if (part == "lid")  lid();
if (part == "both") translate([0, ow + 8, lip_d]) lid();
