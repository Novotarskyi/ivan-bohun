# KURIN - the blade barracks (3D-printed case for the T-ETH-Lite blades)

One ventilated case per blade: the PCB rests on corner posts, the wiring lives
in the plenum beneath it, and the wire bay beyond the board's tail holds the
USB-C breakout plus every centimetre of dupont slack. Two parts: **base** +
**lid**, one geometry for all four blades - lids are told apart by a
paint-pen mark, not a reprint.

What the case provides:

1. fits the blade with pins soldered, dupont housings seated
2. side + floor + lid vents; bay sized for all the wiring, zip-tie slots in
   its floor
3. breakout-agnostic: generous rounded USB-C slot + open bay - horizontal,
   sunken and vertical breakouts all sit behind it (VHB to floor or wall,
   zip-tie the leads)
4. RJ45 aperture in the nose wall, USB-C slot in the tail wall
5. LED slit in the lid over the board's status corner; the RJ45's own
   green/yellow shine out through the RJ45 aperture anyway

## Measurements that drive the parameters

Every clearance is deliberately loose - the inputs were roller-rule
approximations, so the case fits on the first try. Tighten `fit` (2.4) and
`lip_fit` (0.40) only after a trial fit says so.

| Measure | Value | Parameter(s) |
|---|---|---|
| PCB and jack | 55 × 28 mm; the RJ45 overhangs the board 2-3 mm on the clip side, so `nose` = 5 mm | `board_l`, `board_w`, `nose` |
| Stack height | 11 mm pins below the PCB plus seated dupont housings; ~13.5 mm of jack plus loom headroom above | `stand_h`, `above` |
| LED cluster | three LEDs starting 6 mm from the RJ45 edge, spanning 6 mm, 2 mm from the near long edge; lid window 12 × 6 mm centred on the cluster, offset 12 mm from the centreline | `led_sy`, `led_sx`, `led_sl` |
| Breakouts | vertical: 30 × 15 mm, port protruding ~10 mm; horizontals: 20 × 15 mm. The bay is sized to the vertical one and the tail slot admits any of the three at any height | `usb_w`, `usb_z0/z1`, `bay_l` |

**Resulting case: 99 × 35.2 × 46 mm.** The bay leaves depth and height slack
around the worst-case (vertical) breakout.

## Design rationale

- **The blade sits LED-face up, header pins down.** The under-board space
  only needs to clear the pins and the dupont housings seated on them; **the
  tail bay holds the loom**. So the board sits low and the RJ45 sits low with
  it, at the height the Ethernet lead actually runs along the shelf.
- **Corner posts, never side ledges.** The pin rows run down both long
  edges, so a ledge would rest the board on its own pins; the corners are the
  only pin-free real estate. Ribs in the lid trap the board when it closes.
- **The mirror constraint** (`led_side`): the lid window must land over the
  LED cluster whichever way the lid is printed - the coordinate table in
  [`docs/1_hardware/15_blade_cases.md`](../docs/1_hardware/15_blade_cases.md)
  is the standing rule.

## Workflow

1. Open `kurin.scad` in OpenSCAD (free; openscad.org).
2. Set `part="both"` to preview; adjust parameters per the table above.
3. **Print ONE base + ONE lid at defaults first.** Trial-fit blade, breakout
   and lid; adjust; only then print the fleet.
4. Ship files are `kurin.scad` + `stl/kurin_base_v4.stl` +
   `stl/kurin_lid_v4.stl`. One base and one lid geometry serves every blade -
   mark each lid `N1..N4` with a paint pen after printing.

## Print settings

- **PETG** (warm electronics in a warm rack; PLA sags at sustained 60°C)
- 0.2 mm layers, 3 perimeters, 20% infill, **no supports** (all openings
  bridge)
- Base prints floor-down, lid top-up
- ~45-60 g per case pair

## Assembly

1. **Pre-wire the blade, then lower it in** - once it is on its posts you
   cannot reach underneath. RJ45 nose first, until it meets the nose gap.
2. Seat the breakout in the bay behind the USB-C slot; VHB pad down; zip-tie
   the dupont bundle through the floor slots.
3. Lid on (friction lip); paint-pen mark faces up.
