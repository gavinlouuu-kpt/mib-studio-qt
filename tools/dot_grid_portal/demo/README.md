# Illustrative microfluidic concept — corrected no-dot regions

Original 12 × 8 mm concept, not a customer file or fabrication-qualified fluidic device.
`create_sample.py` uses the existing gdstk polygon union to build one connected filled
fluid domain (inlets, serpentine, observation chamber, branches and outlets) on `NO_DOT`.
The same closed boundary is displayed as the device and used for interior exclusion.
All generated dots remain outside it with 50 µm edge clearance.

`render_sample.py <combined.dxf> <internal-map.zip>` produces illustrations from actual
exported geometry. Public metadata includes only count, dimensions, source hash and caveat;
no seed, codec or registry record is served. The internal map is not a customer download.
`validate_sample.py <combined.dxf> <dots.gds>` independently verifies exported dot positions
using gdstk polygon offsets, checks clearance and compares DXF/GDS locations.

The previous 87,907-dot example is superseded: it excluded sampled wall edges, not entire
channel interiors. Its data is retained internally but customer downloads are blocked.
The actual Wafer_soRT DWG has not passed conversion/inspection; do not represent this concept
as evidence that the customer's source file works.
