import { CellImage } from "@mib/ui";
import { illustrativeCrop } from "./illustrativeCrop";

const a = illustrativeCrop(21);
const b = illustrativeCrop(23);

export const RecordWithContour = () => (
  <CellImage recordId="R-024210" {...a} overlay="contour" label="CONTOUR" size={150} caption detail="frame 82 140 · object 02" />
);

export const RecordWithMask = () => (
  <CellImage recordId="R-024210" {...a} overlay="mask" label="MASK" size={150} caption detail="frame 82 140 · object 02" />
);

export const ContourNotRecorded = () => (
  <CellImage recordId="R-024231" src={b.src} imageWidth={80} imageHeight={80} overlay="contour" label="CONTOUR" size={150} caption detail="frame 82 147 · object 01" />
);

export const ImageNotRecorded = () => (
  <CellImage recordId="R-024244" imageWidth={80} imageHeight={80} size={150} caption detail="frame 82 151 · object 03" />
);

export const InspectedSelected = () => (
  <CellImage recordId="R-024210" {...a} overlay="contour" size={150} caption detail="inspected" selected />
);
