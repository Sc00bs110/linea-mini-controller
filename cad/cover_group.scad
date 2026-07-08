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
// four 5 mm round mounting holes 18 mm in from front/back edges and 10 mm in
// from left/right edges, no other cutouts.
//
// Display pod: right-hand side, screen at 70 deg from horizontal, yawed
// 10 deg INWARD (face turns toward the machine centre/operator) to keep the
// cup-warming tray accessible. The pod is an OPEN-BACKED frame housing
// supported by triangular side cheeks: the DFR1092 slides in from the back
// onto M2 bosses behind the face skin (straight-on screwdriver access), and
// its harness drops through a slot in the panel below the opening. Corners
// of the housing outline carry a 2 mm fillet.
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
mount_holes    = [ [10,18], [290,18], [10,112], [290,112] ];

// ── PARAMETERS — display pod ─────────────────────────────────────────────────
disp_angle     = 70;     // screen tilt from horizontal (deg)
disp_yaw       = 10;     // inward turn toward the operator (deg)
pod_center_x   = 225;    // pod centre X (right side, clear of 290mm holes)
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
disp_boss_h    = 4;      // module standoff behind the face skin
wire_space     = 12;     // housing depth behind the module (harness room;
                         // the back is open, so this is retention, not limit)
cheek_t        = 3;      // support cheek (gusset) thickness

cable_slot_w   = 40;     // slot through the panel under the open back
cable_slot_l   = 12;

// ── PARAMETERS — ESP32-S3 mounting (under-panel, left of the pod) ────────────
s3_holes       = true;   // 4 through-holes for nylon standoffs
s3_hole_d      = 2.2;    // M2 standoff screws (DFR0975 has 2.0mm holes)
s3_hole_dx     = 51.4;   // DFR0975 hole grid (from DFR0975_2D_CAD.png:
s3_hole_dy     = 22;     // 25.4 x 61.47 board, holes 22 x ~51.4)
s3_center_x    = 150;
s3_center_y    = 60;

$fn = 48;

// ── Derived ──────────────────────────────────────────────────────────────────
housing_t  = pod_wall + disp_boss_h + disp_mod_t + wire_space;  // frame depth
slab_w     = disp_mod_w + 2*(pod_wall + pod_clear);             // outer width
slab_l     = disp_mod_h + 2*(pod_wall + pod_clear);             // face length
footY0     = -housing_t * sin(disp_angle);   // front-most footprint extent
footY1     =  slab_l    * cos(disp_angle);   // cheek depth at panel level
cheek_h    =  slab_l    * sin(disp_angle);   // cheek height at the back
foot_mid   = (footY0 + footY1) / 2;

// ── Panel ────────────────────────────────────────────────────────────────────
module rounded_plate(l, w, t, r) {
    linear_extrude(t)
        offset(r) offset(-r)
            square([l, w]);
}

// Pod frame: origin = the housing's resting edge on the panel top, centred in
// X, yawed inward. (Negative Z-rotation turns the face toward the machine
// centre when the pod is on the right-hand side.)
module pod_transform() {
    translate([pod_center_x, pod_center_y, panel_thick])
        rotate([0, 0, -disp_yaw])
            translate([0, -foot_mid, 0])
                children();
}

module panel() {
    difference() {
        rounded_plate(panel_len, panel_wid, panel_thick, corner_r);
        for (h = mount_holes)
            translate([h[0], h[1], -1])
                cylinder(d = mount_hole_d, h = panel_thick + 2);
        // harness slot just behind the housing's resting edge, under the
        // open back — same yaw as the pod
        translate([pod_center_x, pod_center_y, -1])
            rotate([0, 0, -disp_yaw])
                translate([-cable_slot_w/2, -foot_mid + 4, 0])
                    cube([cable_slot_w, cable_slot_l, panel_thick + 2]);
        // S3 standoff holes
        if (s3_holes)
            for (dx = [-s3_hole_dx/2, s3_hole_dx/2],
                 dy = [-s3_hole_dy/2, s3_hole_dy/2])
                translate([s3_center_x + dx, s3_center_y + dy, -1])
                    cylinder(d = s3_hole_d, h = panel_thick + 2);
    }
}

// ── Display pod ──────────────────────────────────────────────────────────────
// Housing local frame (before tilt): the screen FACE is the plane
// z=housing_t; the BACK (z=0) is fully open for installing the module.
// x centred, y from 0 (front/low edge) to slab_l. tilt() stands it up by
// disp_angle about the X axis through y=0,z=0.
module tilt() {
    rotate([disp_angle, 0, 0])
        children();
}

module rounded_rect(w, l, r) {
    offset(r) offset(-r)
        translate([-w/2, 0])
            square([w, l]);
}

module display_pod() {
    // frame housing: rounded outline, open back, face skin with aperture
    tilt() {
        difference() {
            linear_extrude(housing_t)
                rounded_rect(slab_w, slab_l, pod_fillet);
            // interior cavity: open at the back (z<0), stops at the face skin
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
        // M2 bosses hanging from the back of the face skin; the module
        // slides in through the open back and screws onto these
        for (dx = [-disp_hole_dx/2, disp_hole_dx/2],
             dy = [-disp_hole_dy/2, disp_hole_dy/2])
            translate([dx, slab_l/2 + dy, housing_t - pod_wall - disp_boss_h])
                difference() {
                    cylinder(d = 5, h = disp_boss_h);
                    translate([0, 0, -1])
                        cylinder(d = disp_hole_d, h = disp_boss_h + 2);
                }
    }
    // support cheeks: triangular gussets from the panel up the housing's
    // side-wall bottom edges (hypotenuse lies along the open-back plane)
    for (sx = [-slab_w/2, slab_w/2 - cheek_t])
        translate([sx, 0, 0])
            rotate([90, 0, 90])
                linear_extrude(cheek_t)
                    polygon([[0, 0], [footY1, 0], [footY1, cheek_h]]);
    // front apron: closes the gap between the face's bottom lip and the
    // panel (the housing leans forward of its resting edge)
    translate([-slab_w/2, 0, 0])
        rotate([90, 0, 90])
            linear_extrude(slab_w)
                polygon([[0, 0],
                         [footY0, 0],
                         [footY0, housing_t * cos(disp_angle)]]);
}

// ── Assembly ─────────────────────────────────────────────────────────────────
panel();
pod_transform()
    display_pod();
