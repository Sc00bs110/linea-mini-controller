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
// Display pod: right-hand side, screen at 70 deg from horizontal, yawed 10 deg
// inward (toward the machine centre / operator) to keep the cup-warming tray
// accessible. DFR1092 mounts from behind against the screen face: M2 screws
// into printed bosses, viewing aperture in the face skin.
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
disp_yaw       = 10;     // inward turn toward operator (deg, +ve = face left)
pod_center_x   = 225;    // pod centre X (right side, clear of 290mm holes)
pod_center_y   = 60;     // pod centre Y

// DFR1092 module (reference/dfrobot/dfr1092-3.5-display.md)
disp_mod_w     = 95.0;   // module outline, landscape
disp_mod_h     = 54.5;
disp_mod_t     = 5.7;
disp_hole_dx   = 90;     // M2 mounting hole spacing
disp_hole_dy   = 50;
disp_hole_d    = 1.8;    // self-tap M2 into printed boss
disp_view_w    = 73.94;  // active area
disp_view_h    = 48.96;
disp_margin    = 1.0;    // aperture margin around active area

pod_wall       = 3;      // walls + face skin thickness
pod_clear      = 0.6;    // module fit clearance
disp_boss_h    = 4;      // module standoff behind the face skin
wire_space     = 12;     // depth behind the module for the hand-wired harness

cable_slot_w   = 30;     // slot through the panel inside the pod cavity
cable_slot_l   = 12;

// ── PARAMETERS — ESP32-S3 mounting (under-panel, left of the pod) ────────────
s3_holes       = true;   // 4 through-holes for nylon standoffs
s3_hole_d      = 2.7;    // M2.5 standoff screws
s3_hole_dx     = 55;     // MEASURE against your DFR0975 + wiring
s3_hole_dy     = 20;
s3_center_x    = 150;    // left of the pod, wires reach the cable slot
s3_center_y    = 60;

$fn = 48;

// ── Derived ──────────────────────────────────────────────────────────────────
housing_t  = pod_wall + disp_boss_h + disp_mod_t + wire_space;  // slab depth
slab_w     = disp_mod_w + 2*(pod_wall + pod_clear);             // outer width
slab_l     = disp_mod_h + 2*(pod_wall + pod_clear);             // face length
footY0     = -housing_t * sin(disp_angle);       // footprint front extent
footY1     =  slab_l    * cos(disp_angle);       // footprint back extent
foot_mid   = (footY0 + footY1) / 2;

// ── Panel ────────────────────────────────────────────────────────────────────
module rounded_plate(l, w, t, r) {
    linear_extrude(t)
        offset(r) offset(-r)
            square([l, w]);
}

// Places children in the pod frame: origin = pod centre on the panel top,
// yawed inward. Children use the tilted-slab local frame (see pod modules).
module pod_transform() {
    translate([pod_center_x, pod_center_y, panel_thick])
        rotate([0, 0, disp_yaw])
            translate([0, -foot_mid, 0])
                children();
}

module panel() {
    difference() {
        rounded_plate(panel_len, panel_wid, panel_thick, corner_r);
        for (h = mount_holes)
            translate([h[0], h[1], -1])
                cylinder(d = mount_hole_d, h = panel_thick + 2);
        // cable slot through the panel, centred in the pod cavity, yawed too
        translate([pod_center_x, pod_center_y, -1])
            rotate([0, 0, disp_yaw])
                translate([-cable_slot_w/2, -cable_slot_l/2, 0])
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
// Slab local frame (before tilt): the screen FACE is the plane z=housing_t,
// body extends down to z=0; x centred, y from 0 (front/low edge) to slab_l.
// tilt() stands it up by disp_angle about the X axis through y=0,z=0, so the
// face leans back at disp_angle from horizontal, looking front-and-up.
module tilt() {
    rotate([disp_angle, 0, 0])
        children();
}

module slab_outer() {
    tilt()
        translate([-slab_w/2, 0, 0])
            cube([slab_w, slab_l, housing_t]);
}

// Outer wedge: hull of the tilted slab and its footprint on the panel.
module pod_outer() {
    hull() {
        slab_outer();
        translate([-slab_w/2, footY0, 0])
            cube([slab_w, footY1 - footY0, 0.5]);
    }
}

module display_pod() {
    difference() {
        pod_outer();
        // interior cavity behind the face skin (module + boss space); stays
        // inside the slab so the pod's outer back/side walls remain closed
        tilt()
            translate([-slab_w/2 + pod_wall, pod_wall, 0])
                cube([slab_w - 2*pod_wall,
                      slab_l - 2*pod_wall,
                      housing_t - pod_wall]);
        // cable chimney: vertical passage from the cavity down through the
        // wedge belly to the panel's cable slot (does not breach outer walls)
        translate([-20, foot_mid - 8, -1])
            cube([40, 16, 27]);
        // viewing aperture through the face skin
        tilt()
            translate([-(disp_view_w/2 + disp_margin),
                       slab_l/2 - (disp_view_h/2 + disp_margin),
                       housing_t - pod_wall - 1])
                cube([disp_view_w + 2*disp_margin,
                      disp_view_h + 2*disp_margin,
                      pod_wall + 2]);
    }
    // M2 bosses hanging from the back of the face skin (module screws on
    // from behind; screen looks through the aperture)
    tilt()
        for (dx = [-disp_hole_dx/2, disp_hole_dx/2],
             dy = [-disp_hole_dy/2, disp_hole_dy/2])
            translate([dx, slab_l/2 + dy, housing_t - pod_wall - disp_boss_h])
                difference() {
                    cylinder(d = 5, h = disp_boss_h);
                    translate([0, 0, -1])
                        cylinder(d = disp_hole_d, h = disp_boss_h + 2);
                }
}

// ── Assembly ─────────────────────────────────────────────────────────────────
panel();
pod_transform()
    display_pod();
