## 2026-10-09 — The bundle names the producer's own commit

`bundle_assemble.sh` takes `PZ_CTI_COMMIT`: the pz7035-imx426 commit the producer (`libpz7035_gentl.cti`) was built from. It must exist in the pz7035 repository
and BUILD_INFO's producer line then reads `(pz7035-imx426 <commit>)`, next to the PL tag line and the Studio commit, so a bundle names all three. Unset, the line
is the one of the earlier bundles (`main 0f67861d retime`). `scripts.yofo_standing_package` covers both and an unknown commit.
