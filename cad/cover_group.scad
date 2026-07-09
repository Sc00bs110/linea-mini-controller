// ─────────────────────────────────────────────────────────────────────────────
// La Marzocco Linea Mini — replacement COVER GROUP LMR (part C.1.743)
// with integrated DFRobot DFR1092 3.5" touchscreen pod for the ESP32-S3
// controller (LineaMini_C6 project, S3 cutover).
//
// HOW TO USE (OpenSCAD basics):
//   * Open this file in OpenSCAD, press F5 to preview, F6 + File>Export>STL
//     to slice. Edit any variable in the PARAMETERS block, F5, repeat.
//
// Stock-part dimensions measured with calipers off the real C.1.743
// (2026-07-08): 300 x 130 x 3 mm flat plate, no flanges, 4 mm corner radius,
// four 5 mm round mounting holes 18 mm in from front/back edges; left/right
// inset revised to 9.5 mm (2026-07-09: +0.5 mm gap each side vs the 9 mm
// second fit test).
//
// Display pod (v2, 2026-07-09): right-hand side, screen at 20 deg from
// horizontal, facing the front directly (no yaw), 10 mm wider than the
// module on EACH side for cable slack. The pod is SEALED — no external
// hatch. Its only opening is downward: an installation aperture in the
// panel underneath, through which the DFR1092 (screws onto M2 bosses
// behind the face skin) and the FireBeetle (screws onto M2 bosses on the
// inside of the pod's rear wall — "the back panel") are fitted. A snap-in
// cover closes the aperture from below, with an open cable notch on its
// rear edge for the harness coming up from inside the machine.
//
// Datum: X=0,Y=0 = FRONT-LEFT corner of the panel top face. X right (300),
// Y back (130), Z up. "Front" = barista-facing edge.
// ─────────────────────────────────────────────────────────────────────────────

// ── PARAMETERS — panel (measured stock C.1.743) ──────────────────────────────
panel_len      = 300;
panel_wid      = 130;
panel_thick    = 3;
corner_r       = 4;
mount_hole_d   = 5.0;
// 9.5mm in from left/right edges, 18mm in from front/back
mount_holes    = [ [9.5,18], [290.5,18], [9.5,112], [290.5,112] ];

// ── PARAMETERS — display pod ─────────────────────────────────────────────────
disp_angle     = 20;     // screen tilt from horizontal (deg)
pod_center_x   = 225;    // pod centre X (right side, clear of the 290.5 holes)
pod_center_y   = 60;     // pod centre Y
pod_fillet     = 2;      // fillet on the housing outline corners

// DFR1092 module (reference/dfrobot/dfr1092-3.5-display.md + vendor STEP)
disp_mod_w     = 95.0;   // module outline, landscape
disp_mod_h     = 54.5;
disp_mod_t     = 5.7;
disp_hole_dx   = 90;     // M2 mounting hole spacing
disp_hole_dy   = 50;
disp_hole_d    = 1.8;    // self-tap M2 into printed boss
disp_view_w    = 73.94;  // active area
disp_view_h    = 48.96;
disp_margin    = 1.0;    // aperture margin around active area

pod_wall       = 3;      // frame walls + face skin thickness
pod_clear      = 0.6;    // module fit clearance
side_extra     = 10;     // extra housing width EACH side for cable slack
disp_boss_h    = 4;      // module standoff behind the face skin
wire_space     = 15;     // interior depth behind the module: harness room
                         // PLUS the FireBeetle stack on the rear wall
                         // (boss 3 + board 1.6 + components ~4)
cheek_t        = 3;      // side cheek / rear wall thickness under the slab

// Panel aperture under the pod (installation + cabling) and its snap cover
apt_clear      = 3;      // aperture inset from the pod's under-void footprint
cover_t        = 2;      // bottom cover plate thickness
cover_lip      = 4;      // cover overlap beyond the aperture, all round
cover_clear    = 0.3;    // tab fit clearance against the aperture edges
tab_w          = 12;     // snap tab width
tab_t          = 1.6;    // snap tab thickness (flexes to clear the nub)
nub_proud      = 0.8;    // nub protrusion past the aperture edge
notch_w        = 25;     // open cable notch, middle of the cover's REAR edge
notch_d        = 12;     // notch depth into the cover

// ── ESP32-S3 (FireBeetle DFR0975) mounting ───────────────────────────────────
// No printed bosses: the rear-wall bosses clashed with the display's mounting
// screws (removed 2026-07-09). Board fixing inside the pod is TBD by the user
// (adhesive standoffs / velcro / a small carrier). For reference the board is
// 25.4 x 61.47 with 2.0mm holes on a 22 x ~51.4 grid.

// What to render (edit + F5/F6):
//   "all"          assembly preview: panel + pod + bottom cover in place
//   "main"         panel + pod only -> export cover_group.stl
//   "bottom_cover" cover alone, flat on the bed -> cover_group_bottom.stl
//   "fit_test"     first fit_test_h mm of the main part only -- quick print
//                  to verify outline/holes/pod footprint on the machine
render_part    = "all";
fit_test_h     = 1;      // fit-test slice height (mm)

$fn = 48;

// ── Derived ──────────────────────────────────────────────────────────────────
housing_t  = pod_wall + disp_boss_h + disp_mod_t + wire_space;  // face depth
slab_w     = disp_mod_w + 2*(pod_wall + pod_clear + side_extra); // outer width
slab_l     = disp_mod_h + 2*(pod_wall + pod_clear);              // face length
footY0     = -housing_t * sin(disp_angle);   // front-most footprint extent
footY1     =  slab_l    * cos(disp_angle);   // footprint depth at panel level
back_h     =  slab_l    * sin(disp_angle);   // slab underside height at rear
foot_mid   = (footY0 + footY1) / 2;

// Panel aperture rectangle. Pod-local X/Y (pod frame origin = resting edge,
// x centred): inset apt_clear from the under-void's interior footprint
// (between the cheeks and the front/rear closures).
apt_w      = slab_w - 2*cheek_t - 2*apt_clear;
apt_y0l    = apt_clear;                       // pod-local front edge
apt_l      = (footY1 - cheek_t) - 2*apt_clear;
// world-frame position of the aperture's front-left corner
apt_x0     = pod_center_x - apt_w/2;
apt_y0     = pod_center_y - foot_mid + apt_y0l;

// ── Primitives ───────────────────────────────────────────────────────────────
module rounded_plate(l, w, t, r) {
    linear_extrude(t)
        offset(r) offset(-r)
            square([l, w]);
}

module rounded_rect(w, l, r) {
    offset(r) offset(-r)
        translate([-w/2, 0])
            square([w, l]);
}

// Pod frame: origin = the slab's resting edge on the panel top, centred in
// X. No yaw — the face looks straight forward.
module pod_transform() {
    translate([pod_center_x, pod_center_y, panel_thick])
        translate([0, -foot_mid, 0])
            children();
}

// Slab local frame (before tilt): the screen FACE is the plane z=housing_t;
// z=0 is the slab's underside plane (open — the pod's only opening, facing
// the under-void and the panel aperture). x centred, y from 0 (front/low
// edge) to slab_l. tilt() leans it back by disp_angle about X through y=0,z=0.
module tilt() {
    rotate([disp_angle, 0, 0])
        children();
}

// ── Panel ────────────────────────────────────────────────────────────────────
module panel() {
    difference() {
        rounded_plate(panel_len, panel_wid, panel_thick, corner_r);
        for (h = mount_holes)
            translate([h[0], h[1], -1])
                cylinder(d = mount_hole_d, h = panel_thick + 2);
        // installation aperture under the pod (display + FireBeetle go in
        // from below; the snap cover closes it from underneath)
        translate([apt_x0, apt_y0, -1])
            linear_extrude(panel_thick + 2)
                offset(2) offset(-2)
                    square([apt_w, apt_l]);
    }
}

// ── Display pod ──────────────────────────────────────────────────────────────
module display_pod() {
    // tilted slab: rounded outline, face skin with aperture, underside open
    tilt() {
        difference() {
            linear_extrude(housing_t)
                rounded_rect(slab_w, slab_l, pod_fillet);
            // interior cavity: open at the underside (z<0), stops at the
            // face skin
            translate([-slab_w/2 + pod_wall, pod_wall, -1])
                cube([slab_w - 2*pod_wall,
                      slab_l - 2*pod_wall,
                      housing_t - pod_wall + 1]);
            // viewing aperture through the face skin
            translate([-(disp_view_w/2 + disp_margin),
                       slab_l/2 - (disp_view_h/2 + disp_margin),
                       housing_t - pod_wall - 1])
                cube([disp_view_w + 2*disp_margin,
                      disp_view_h + 2*disp_margin,
                      pod_wall + 2]);
        }
        // M2 bosses hanging from the back of the face skin — the DFR1092
        // comes up through the panel aperture and screws onto these
        for (dx = [-disp_hole_dx/2, disp_hole_dx/2],
             dy = [-disp_hole_dy/2, disp_hole_dy/2])
            translate([dx, slab_l/2 + dy, housing_t - pod_wall - disp_boss_h])
                difference() {
                    cylinder(d = 5, h = disp_boss_h);
                    translate([0, 0, -1])
                        cylinder(d = disp_hole_d, h = disp_boss_h + 2);
                }
        // (FireBeetle bosses removed 2026-07-09: they would have clashed with
        // the display's mounting screws — the board's fixing is TBD by the
        // user; the rear wall is left plain.)
    }
    // side cheeks: close the triangular gap between the panel and the
    // slab's tilted underside (front vertex nudged -0.5 to overlap the
    // slab volume — keeps the mesh manifold)
    for (sx = [-slab_w/2, slab_w/2 - cheek_t])
        translate([sx, 0, 0])
            rotate([90, 0, 90])
                linear_extrude(cheek_t)
                    polygon([[-0.5, 0], [footY1, 0], [footY1, back_h]]);
    // rear closure: vertical wall from the panel up to the slab's underside
    // at the rear — the under-void is sealed except the panel aperture
    // (0.5 rises into the slab volume for manifold overlap)
    translate([-slab_w/2, footY1 - cheek_t, 0])
        cube([slab_w, cheek_t, back_h + 0.5]);
    // front apron: closes the gap between the face's bottom lip and the
    // panel (the slab leans forward of its resting edge)
    translate([-slab_w/2, 0, 0])
        rotate([90, 0, 90])
            linear_extrude(slab_w)
                polygon([[0.5, 0],
                         [footY0, 0],
                         [footY0, housing_t * cos(disp_angle)]]);
}

// ── Snap-in bottom cover (second printed part) ───────────────────────────────
// Print orientation: plate flat on the bed, centred on the aperture centre.
// In the assembly it sits UNDER the panel: the plate overlaps the aperture
// by cover_lip all round, and two snap tabs per long side rise through the
// aperture, their nubs clicking out over the panel's top surface. Open cable
// notch mid-rear edge (+y).
module bottom_cover() {
    difference() {
        linear_extrude(cover_t)
            offset(3) offset(-3)
                translate([-(apt_w/2 + cover_lip), -(apt_l/2 + cover_lip)])
                    square([apt_w + 2*cover_lip, apt_l + 2*cover_lip]);
        // open notch to the rear edge (cables drop in sideways)
        translate([-notch_w/2, apt_l/2 + cover_lip - notch_d, -1])
            cube([notch_w, notch_d + 4, cover_t + 2]);
    }
    // snap tabs: two per long (left/right) aperture edge, rising from the
    // plate through the aperture; ramped nub catches on the panel top face
    tab_rise = cover_t + panel_thick;   // nub shoulder = panel top surface
    for (sx = [-1, 1], ty = [-apt_l/4, apt_l/4]) {
        sxo = sx * (apt_w/2 - cover_clear);   // tab outer face x
        translate([sx > 0 ? sxo - tab_t : sxo, ty - tab_w/2, 0])
            cube([tab_t, tab_w, tab_rise + 2.5]);
        hull() {
            // ramp top (thin strip flush with the tab face)
            translate([sx > 0 ? sxo - 0.1 : sxo, ty - tab_w/2, tab_rise + 2.5])
                cube([0.1, tab_w, 0.1]);
            // shoulder (proud strip just above the panel top)
            translate([sx > 0 ? sxo : sxo - nub_proud, ty - tab_w/2, tab_rise])
                cube([nub_proud, tab_w, 0.5]);
        }
    }
}

// ── Assembly / part selection ────────────────────────────────────────────────
if (render_part == "fit_test")
    intersection() {
        union() {
            panel();
            pod_transform()
                display_pod();
        }
        translate([-5, -5, -1])
            cube([panel_len + 10, panel_wid + 10, fit_test_h + 1]);
    }
if (render_part != "bottom_cover" && render_part != "fit_test") {
    panel();
    pod_transform()
        display_pod();
}
if (render_part == "all")
    translate([apt_x0 + apt_w/2, apt_y0 + apt_l/2, -cover_t])
        bottom_cover();
if (render_part == "bottom_cover")
    bottom_cover();
