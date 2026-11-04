# Realistic example: microfluidic sorter concept

Original illustrative CAD authored for this portal, not a customer design, reproduced
published design, validated fluidic device or fabrication-ready layout. Source units are
millimetres, overall extent 12 × 8 mm. Ports, mixer walls, an observation region, outlet
branches and corner alignment crosses make the workflow recognizable.

`create_sample.py` authors the DXF using the existing ezdxf dependency. Upload
`web/public/demo/sorter-concept.dxf` with units `mm` through the real portal, confirm the
normalized 12000 × 8000 micrometre preview, then download combined DXF and map ZIP.
`render_sample.py <combined.dxf> <mib-map.zip>` derives the overview and cropped before/after
SVGs and manifest summary. Orange dots are actual exported circles; no aesthetic grid is
invented. The display keeps the same XY orientation for both images (positive Y downward).

Verified 2026-11-04 through the tailnet review API: design
`dg-b20850c7-f5af-4f6c-9886-c7a9be90c4af`, review seed 11, 87,907 dots, codec 1.
The original CAD hash is recorded in `web/public/demo/sample.json`. All three exports
remain downloadable from the ready review workspace design. These review identities are
not production registry reservations. The example may be used under the repository license.

Keep-outs are the current sampled-geometry implementation, not whole-channel interior
exclusion or manufacturing DRC. This example makes the real behavior visible, including
dots in sufficiently wide open regions. Optical readability and fluidic performance have
not been tested. No real-camera localization claim is made.
